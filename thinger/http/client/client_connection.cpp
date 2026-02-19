#include "client_connection.hpp"
#include "../../util/logger.hpp"
#include "../../util/compression.hpp"

namespace thinger::http {

std::atomic<unsigned long> client_connection::connections(0);

client_connection::client_connection(std::shared_ptr<thinger::asio::socket> socket,
                                     std::chrono::seconds timeout)
    : socket_(std::move(socket))
    , timeout_(timeout) {
    ++connections;
    LOG_TRACE("created http client connection with timeout: {} seconds. total: {}",
              timeout.count(), connections.load());
}

client_connection::client_connection(std::shared_ptr<thinger::asio::unix_socket> socket,
                                     const std::string& path,
                                     std::chrono::seconds timeout)
    : socket_(std::move(socket))
    , socket_path_(path)
    , timeout_(timeout) {
    ++connections;
    LOG_TRACE("created http client connection (for unix socket: {}) with timeout: {} seconds. total: {}",
              path, timeout.count(), connections.load());
}

client_connection::~client_connection() {
    --connections;
    LOG_TRACE("releasing http client connection. total: {}", connections.load());
}

awaitable<void> client_connection::ensure_connected(const http_request& request) {
    if (socket_->is_open()) {
        co_return;
    }

    LOG_TRACE("connecting to: {}:{}", request.get_host(), request.get_port());

    for (unsigned retry = 0; retry < MAX_RETRIES; ++retry) {
        boost::system::error_code ec;
        if (socket_path_.empty()) {
            ec = co_await socket_->connect(request.get_host(), request.get_port(), CONNECT_TIMEOUT);
        } else {
            auto unix_socket = std::static_pointer_cast<thinger::asio::unix_socket>(socket_);
            ec = co_await unix_socket->connect(socket_path_, CONNECT_TIMEOUT);
        }

        if (!ec) {
            LOG_TRACE("connection established");
            co_return;
        }

        LOG_ERROR("error while connecting (attempt #{}): {} ({}) ({})",
                  retry + 1, ec.message(), ec.value(), ec.category().name());

        // Don't retry for certain errors
        if (ec == boost::asio::error::operation_aborted ||
            ec == boost::asio::error::host_not_found) {
            co_return;
        }

        if (retry + 1 >= MAX_RETRIES) {
            co_return;
        }

        // Close and retry
        socket_->close();
    }
}

awaitable<std::shared_ptr<http_response>> client_connection::read_response(bool head_request) {
    response_parser_.reset();

    while (true) {
        auto bytes = co_await socket_->read_some(buffer_, MAX_BUFFER_SIZE);

        if (bytes == 0) {
            co_return nullptr;
        }

        boost::tribool result = response_parser_.parse(buffer_, buffer_ + bytes, head_request);

        if (result) {
            // Successfully parsed response
            auto response = response_parser_.consume_response();

            // Decompress if needed
            if (response && response->has_header("Content-Encoding")) {
                std::string encoding = response->get_header("Content-Encoding");
                if (encoding == "gzip") {
                    auto decompressed = ::thinger::util::gzip::decompress(response->get_content());
                    if (decompressed) {
                        response->set_content(std::move(*decompressed));
                        response->remove_header("Content-Encoding");
                    } else {
                        LOG_ERROR("Failed to decompress gzip response");
                    }
                } else if (encoding == "deflate") {
                    auto decompressed = ::thinger::util::deflate::decompress(response->get_content());
                    if (decompressed) {
                        response->set_content(std::move(*decompressed));
                        response->remove_header("Content-Encoding");
                    } else {
                        LOG_ERROR("Failed to decompress deflate response");
                    }
                }
            }

            co_return response;
        } else if (!result) {
            // Parse error
            co_return nullptr;
        }
        // else: indeterminate, keep reading
    }
}

awaitable<std::shared_ptr<http_response>> client_connection::send_request(
    std::shared_ptr<http_request> request) {

    std::shared_ptr<http_response> response;

    // Timeout timer
    boost::asio::steady_timer timer(socket_->get_io_context());
    timer.expires_after(timeout_);

    auto do_request = [&]() -> awaitable<void> {
        co_await ensure_connected(*request);
        if (!socket_->is_open()) co_return;

        request->log("CLIENT->", 0);
        response_parser_.setOnChunked(request->get_chunked_callback());

        co_await request->to_socket(socket_);

        bool is_head = request->get_method() == http::method::HEAD;
        response = co_await read_response(is_head);

        if (response && !response->keep_alive()) {
            socket_->close();
        }
    };

    auto do_timeout = [&]() -> awaitable<void> {
        auto [ec] = co_await timer.async_wait(use_nothrow_awaitable);
        if (!ec) {
            LOG_ERROR("Request timeout after {} seconds", timeout_.count());
            socket_->close();
        }
    };

    co_await (do_request() || do_timeout());

    co_return response;
}

awaitable<stream_result> client_connection::send_request_streaming(
    std::shared_ptr<http_request> request,
    stream_callback callback) {

    stream_result result;

    // Timeout timer
    boost::asio::steady_timer timer(socket_->get_io_context());
    timer.expires_after(timeout_);

    auto do_request = [&]() -> awaitable<void> {
        co_await ensure_connected(*request);

        if (!socket_->is_open()) {
            result.error = "Failed to connect";
            co_return;
        }

        request->log("CLIENT->", 0);
        response_parser_.reset();

        // Set up streaming callback
        response_parser_.setOnStreaming(
            [&callback, &result, this](
                const std::string_view& data, size_t downloaded, size_t total) -> bool {
                if (result.status_code == 0) {
                    result.status_code = response_parser_.get_status_code();
                }
                result.bytes_transferred = downloaded;

                stream_info info{data, downloaded, total, result.status_code};
                return callback(info);
            });

        co_await request->to_socket(socket_);

        // Read response with streaming
        while (true) {
            auto bytes = co_await socket_->read_some(buffer_, MAX_BUFFER_SIZE);

            if (bytes == 0) {
                result.error = "Connection closed";
                co_return;
            }

            bool is_head = request->get_method() == http::method::HEAD;
            boost::tribool parse_result = response_parser_.parse(
                buffer_, buffer_ + bytes, is_head);

            if (result.status_code == 0) {
                result.status_code = response_parser_.get_status_code();
            }

            if (parse_result) {
                auto response = response_parser_.consume_response();
                if (response) {
                    result.status_code = response->get_status_code();
                    if (result.bytes_transferred == 0) {
                        result.bytes_transferred = response->get_content_size();
                    }
                    if (!response->keep_alive()) {
                        socket_->close();
                    }
                }
                co_return;
            } else if (!parse_result) {
                result.error = "Parse error or download aborted";
                co_return;
            }
        }
    };

    auto do_timeout = [&]() -> awaitable<void> {
        auto [ec] = co_await timer.async_wait(use_nothrow_awaitable);
        if (!ec) {
            LOG_ERROR("Streaming request timeout after {} seconds", timeout_.count());
            socket_->close();
            result.error = "Request timeout";
        }
    };

    co_await (do_request() || do_timeout());

    co_return result;
}

void client_connection::close() {
    if (socket_->is_open()) {
        socket_->close();
    }
    response_parser_.reset();
}

std::shared_ptr<thinger::asio::socket> client_connection::release_socket() {
    socket_->cancel();
    return socket_;
}

}

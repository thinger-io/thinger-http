#include "client_connection.hpp"
#include "../../util/logger.hpp"
#include "../../util/compression.hpp"
#include <boost/asio/cancel_after.hpp>

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
        try {
            if (socket_path_.empty()) {
                co_await socket_->connect(request.get_host(), request.get_port(), CONNECT_TIMEOUT);
            } else {
                auto unix_socket = std::static_pointer_cast<thinger::asio::unix_socket>(socket_);
                co_await unix_socket->connect(socket_path_, CONNECT_TIMEOUT);
            }
            LOG_TRACE("connection established");
            co_return;
        } catch (const boost::system::system_error& e) {
            LOG_ERROR("error while connecting (attempt #{}): {} ({}) ({})",
                      retry + 1, e.what(), e.code().value(), e.code().category().name());

            // Don't retry for certain errors
            if (e.code() == boost::asio::error::operation_aborted ||
                e.code() == boost::asio::error::host_not_found) {
                throw;
            }

            if (retry + 1 >= MAX_RETRIES) {
                throw;
            }

            // Close and retry
            socket_->close();
        }
    }
}

awaitable<std::shared_ptr<http_response>> client_connection::read_response(bool head_request) {
    response_parser_.reset();

    while (true) {
        auto bytes = co_await socket_->read_some(buffer_, MAX_BUFFER_SIZE);

        boost::tribool result = response_parser_.parse(buffer_, buffer_ + bytes, head_request);

        if (result) {
            // Successfully parsed response
            auto response = response_parser_.consume_response();

            // Decompress if needed
            if (response && response->has_header("Content-Encoding")) {
                std::string encoding = response->get_header("Content-Encoding");
                try {
                    if (encoding == "gzip") {
                        response->set_content(::thinger::util::gzip::decompress(response->get_content()));
                        response->remove_header("Content-Encoding");
                    } else if (encoding == "deflate") {
                        response->set_content(::thinger::util::deflate::decompress(response->get_content()));
                        response->remove_header("Content-Encoding");
                    }
                } catch (const std::exception& ex) {
                    LOG_ERROR("Failed to decompress response: {}", ex.what());
                }
            }

            co_return response;
        } else if (!result) {
            // Parse error
            throw boost::system::system_error(boost::asio::error::invalid_argument);
        }
        // else: indeterminate, keep reading
    }
}

awaitable<std::shared_ptr<http_response>> client_connection::send_request(
    std::shared_ptr<http_request> request) {

    // Use cancel_after for clean timeout handling
    co_return co_await boost::asio::co_spawn(
        socket_->get_io_context(),
        [this, request]() -> awaitable<std::shared_ptr<http_response>> {
            // Ensure we're connected
            co_await ensure_connected(*request);

            // Log request
            request->log("CLIENT->", 0);

            // Set chunked callback if any
            response_parser_.setOnChunked(request->get_chunked_callback());

            // Send request
            co_await request->to_socket(socket_);

            // Read response
            bool is_head = request->get_method() == http::method::HEAD;
            auto response = co_await read_response(is_head);

            // Close if not keep-alive
            if (response && !response->keep_alive()) {
                socket_->close();
            }

            co_return response;
        },
        boost::asio::cancel_after(timeout_, use_awaitable)
    );
}

awaitable<stream_result> client_connection::send_request_streaming(
    std::shared_ptr<http_request> request,
    stream_callback callback) {

    co_return co_await boost::asio::co_spawn(
        socket_->get_io_context(),
        [this, request, callback]() -> awaitable<stream_result> {
            stream_result result;

            try {
                // Ensure we're connected
                co_await ensure_connected(*request);

                // Log request
                request->log("CLIENT->", 0);

                // Reset parser for new response
                response_parser_.reset();

                // Pointer to capture status code for the streaming callback
                int* status_ptr = &result.status_code;
                size_t* bytes_ptr = &result.bytes_transferred;

                // Set up streaming callback that converts to stream_info
                response_parser_.setOnStreaming(
                    [&callback, status_ptr, bytes_ptr, this](
                        const std::string_view& data, size_t downloaded, size_t total) -> bool {
                        // Update status code from parser if not set yet
                        if (*status_ptr == 0) {
                            *status_ptr = response_parser_.get_status_code();
                        }
                        *bytes_ptr = downloaded;

                        stream_info info{data, downloaded, total, *status_ptr};
                        return callback(info);
                    });

                // Send request
                co_await request->to_socket(socket_);

                // Read response with streaming
                while (true) {
                    auto bytes = co_await socket_->read_some(buffer_, MAX_BUFFER_SIZE);

                    bool is_head = request->get_method() == http::method::HEAD;
                    boost::tribool parse_result = response_parser_.parse(
                        buffer_, buffer_ + bytes, is_head);

                    // Update status code after headers are parsed (before content)
                    if (result.status_code == 0) {
                        result.status_code = response_parser_.get_status_code();
                    }

                    if (parse_result) {
                        // Successfully completed
                        auto response = response_parser_.consume_response();
                        if (response) {
                            result.status_code = response->get_status_code();
                            if (result.bytes_transferred == 0) {
                                result.bytes_transferred = response->get_content_size();
                            }

                            // Copy headers to result
                            // response->for_each_header([&result](const std::string& k, const std::string& v) {
                            //     result.headers[k] = v;
                            // });

                            // Close if not keep-alive
                            if (!response->keep_alive()) {
                                socket_->close();
                            }
                        }
                        co_return result;
                    } else if (!parse_result) {
                        // Parse error or user aborted
                        result.error = "Parse error or download aborted";
                        co_return result;
                    }
                    // else: indeterminate, keep reading
                }
            } catch (const boost::system::system_error& e) {
                result.error = e.what();
                co_return result;
            } catch (const std::exception& e) {
                result.error = e.what();
                co_return result;
            }
        },
        boost::asio::cancel_after(timeout_, use_awaitable)
    );
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

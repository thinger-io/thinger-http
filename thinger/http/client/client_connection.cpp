#include "client_connection.hpp"
#include "client_error.hpp"
#include "../../util/logger.hpp"
#include "../../util/compression.hpp"

namespace thinger::http {

namespace {

// Decode a gzip or deflate Content-Encoding (the body is kept as it is if it fails)
void decompress(http_response& response) {
    std::string encoding = response.get_header("Content-Encoding");
    std::optional<std::string> decoded;
    if (encoding == "gzip") {
        decoded = ::thinger::util::gzip::decompress(response.get_content());
    } else if (encoding == "deflate") {
        decoded = ::thinger::util::deflate::decompress(response.get_content());
    } else {
        return;
    }
    if (!decoded) {
        LOG_ERROR("Failed to decompress {} response", encoding);
        return;
    }
    response.set_content(std::move(*decoded));
    response.remove_header("Content-Encoding");
}

}

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

awaitable<boost::system::error_code> client_connection::ensure_connected(const http_request& request) {
    boost::system::error_code ec;
    if (socket_->is_open()) {
        co_return ec;
    }

    LOG_TRACE("connecting to: {}:{}", request.get_host(), request.get_port());

    for (unsigned retry = 0; retry < MAX_RETRIES; ++retry) {
        if (socket_path_.empty()) {
            ec = co_await socket_->connect(request.get_host(), request.get_port(), CONNECT_TIMEOUT);
        } else {
            auto unix_socket = std::static_pointer_cast<thinger::asio::unix_socket>(socket_);
            ec = co_await unix_socket->connect(socket_path_, CONNECT_TIMEOUT);
        }

        if (!ec) {
            LOG_TRACE("connection established");
            co_return ec;
        }

        LOG_ERROR("error while connecting (attempt #{}): {} ({}) ({})",
                  retry + 1, ec.message(), ec.value(), ec.category().name());

        // Don't retry for certain errors
        if (ec == boost::asio::error::operation_aborted ||
            ec == boost::asio::error::host_not_found) {
            co_return ec;
        }

        if (retry + 1 >= MAX_RETRIES) {
            co_return ec;
        }

        // Close and retry
        socket_->close();
    }
    co_return ec;
}

awaitable<std::shared_ptr<http_response>> client_connection::read_response(const http_request& request) {
    while (true) {
        auto [ec, bytes] = co_await socket_->read_some(buffer_, MAX_BUFFER_SIZE);

        uint8_t* begin = buffer_;
        uint8_t* end = buffer_ + bytes;
        boost::tribool result = boost::indeterminate;
        if (begin != end) {
            result = response_parser_.parse_some(begin, end);
        }
        if (boost::indeterminate(result) && ec) {
            // The end of the connection ends a body delimited by it, and nothing else
            if (ec != boost::asio::error::eof) {
                error_ = ec;
                co_return nullptr;
            }
            result = response_parser_.end_of_stream();
        }
        if (!result) {
            error_ = response_parser_.error();
            LOG_WARNING("invalid response from {}: {}", request.get_host(), error_.message());
            co_return nullptr;
        }
        if (boost::indeterminate(result)) continue;

        // Data past the response, other than that of a switched protocol (another response
        // than the one requested, or garbage), leaves the connection out of sync
        bool reusable = response_parser_.keep_alive() && begin == end;
        auto response = response_parser_.consume_response();
        if (response->get_status_code() == 101) {
            if (!request.has_header(header::upgrade)) {
                error_ = client_error::unexpected_upgrade;
                co_return nullptr;
            }
            // What followed the response belongs to the new protocol (e.g. WebSocket frames)
            buffered_.assign(reinterpret_cast<const char*>(begin), static_cast<size_t>(end - begin));
        } else if (!reusable) {
            socket_->close();
        }

        // A streamed body is handed as received
        if (!response_parser_.is_streaming()) decompress(*response);

        co_return response;
    }
}

awaitable<std::shared_ptr<http_response>> client_connection::exchange(std::shared_ptr<http_request> request) {
    std::shared_ptr<http_response> response;
    bool timed_out = false;
    error_ = {};
    buffered_.clear();

    // Timeout timer
    boost::asio::steady_timer timer(socket_->get_io_context());
    timer.expires_after(timeout_);

    auto do_request = [&]() -> awaitable<void> {
        if (auto ec = co_await ensure_connected(*request)) {
            error_ = ec;
            co_return;
        }
        if (!socket_->is_open()) {
            error_ = boost::asio::error::not_connected;
            co_return;
        }

        request->log("CLIENT->", 0);
        co_await request->to_socket(socket_);
        response = co_await read_response(*request);
    };

    auto do_timeout = [&]() -> awaitable<void> {
        auto [ec] = co_await timer.async_wait(use_nothrow_awaitable);
        if (!ec) {
            LOG_ERROR("Request timeout after {} seconds", timeout_.count());
            timed_out = true;
            socket_->close();
        }
    };

    co_await (do_request() || do_timeout());

    if (timed_out) {
        response.reset();
        error_ = boost::asio::error::timed_out;
    }
    // Never reused after an error: where the next response would start is unknown
    if (!response) close();

    // The response is read: the connection may serve another request now, unless it
    // switched protocols (then it is never handed out again)
    if (!response || response->get_status_code() != 101) claimed_ = false;

    co_return response;
}

awaitable<std::shared_ptr<http_response>> client_connection::send_request(
    std::shared_ptr<http_request> request) {
    response_parser_.reset();
    response_parser_.set_request_method(request->get_method());
    response_parser_.setOnChunked(request->get_chunked_callback());
    co_return co_await exchange(std::move(request));
}

awaitable<stream_result> client_connection::send_request_streaming(
    std::shared_ptr<http_request> request,
    stream_callback callback) {

    stream_result result;

    response_parser_.reset();
    response_parser_.set_request_method(request->get_method());
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

    auto response = co_await exchange(std::move(request));
    if (!response) {
        result.error = error_.message();
        co_return result;
    }

    result.status_code = response->get_status_code();
    if (result.bytes_transferred == 0) {
        result.bytes_transferred = response->get_content_size();
    }
    co_return result;
}

void client_connection::close() {
    if (socket_->is_open()) {
        socket_->close();
    }
    response_parser_.reset();
}

std::string client_connection::take_buffered() {
    return std::exchange(buffered_, {});
}

std::shared_ptr<thinger::asio::socket> client_connection::release_socket() {
    socket_->cancel();
    return socket_;
}

}

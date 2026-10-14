#include "client_connection.hpp"
#include "client_error.hpp"
#include "../../asio/sockets/ssl_socket.hpp"
#include "../../util/ascii.hpp"
#include "../../util/logger.hpp"
#include "../../util/compression.hpp"
#include <charconv>
#include <boost/asio/ssl/error.hpp>

namespace thinger::http {

namespace {

// Headers of a streamed response, as Node gives them: names in lowercase, and the values of
// a header repeated joined with ", "
void set_headers(stream_result& result, const http_response& response) {
    result.headers.clear();
    for (const auto& [name, value] : response.get_headers()) {
        std::string key = name;
        ::thinger::util::ascii::to_lower(key);
        auto [header, inserted] = result.headers.emplace(std::move(key), value);
        if (!inserted) header->second += ", " + value;
    }
}

// Decode the Content-Encoding (gzip, x-gzip, deflate, or several of them; see
// util::decode_content), up to `max_size` bytes once decoded: inflating stops as soon as the
// body goes past them, so a small body expanding to gigabytes (a decompression bomb) takes no
// more memory than the limit. Bodies with other codings (as Node's fetch), and empty bodies
// (e.g. of responses to HEAD), are kept as they are, with their Content-Encoding.
client_error decompress(http_response& response, size_t max_size) {
    if (response.get_content().empty() || !response.has_header("Content-Encoding")) return client_error::none;
    // The values of every Content-Encoding header make one list
    std::string encoding;
    for (const auto& value : response.get_headers_with_key("Content-Encoding")) {
        if (!encoding.empty()) encoding += ", ";
        encoding += value;
    }
    switch (::thinger::util::decode_content(encoding, response.get_content(), max_size)) {
        case ::thinger::util::content_decoding::decoded:
            response.remove_headers("Content-Encoding");
            return client_error::none;
        case ::thinger::util::content_decoding::unsupported:
            return client_error::none;
        case ::thinger::util::content_decoding::too_large:
            LOG_ERROR("the decompressed {} response exceeds the maximum allowed size: {} max", encoding, max_size);
            return client_error::body_too_large;
        case ::thinger::util::content_decoding::invalid:
            break;
    }
    LOG_ERROR("Failed to decompress {} response", encoding);
    return client_error::bad_content_encoding;
}

std::string_view trim(std::string_view value) {
    static constexpr std::string_view whitespace = " \t";
    auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}

// CONNECT host:port, in authority-form (RFC 9110 section 9.3.6, RFC 9112 section 3.2.3),
// with the credentials of the proxy: only it sees them
void set_connect_request(http_request& connect, const http_request& request, const std::string& authority,
                         const std::string& proxy_authorization) {
    connect.set_method(method::CONNECT);
    connect.set_uri(authority);
    connect.add_header(header::host, authority);
    if (request.has_header(header::user_agent)) {
        connect.add_header(header::user_agent, request.get_header(header::user_agent));
    }
    if (!proxy_authorization.empty()) {
        connect.add_header("Proxy-Authorization", proxy_authorization);
    }
}

// Whether a Connection header lists `option` (e.g. "close")
bool has_connection_option(std::string_view value, std::string_view option) {
    while (true) {
        auto comma = value.find(',');
        if (::thinger::util::ascii::iequals(trim(value.substr(0, comma)), option)) return true;
        if (comma == std::string_view::npos) return false;
        value.remove_prefix(comma + 1);
    }
}

// The timeout of a Keep-Alive header ("timeout=5, max=100"), in seconds
std::optional<std::chrono::seconds> keep_alive_timeout_of(std::string_view value) {
    while (!value.empty()) {
        auto comma = value.find(',');
        auto parameter = trim(value.substr(0, comma));
        if (::thinger::util::ascii::istarts_with(parameter, "timeout=")) {
            auto number = trim(parameter.substr(8));
            long seconds = 0;
            auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), seconds);
            if (ec == std::errc{} && end == number.data() + number.size() && seconds >= 0) {
                return std::chrono::seconds{seconds};
            }
            return std::nullopt;
        }
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
    }
    return std::nullopt;
}

}

std::atomic<unsigned long> client_connection::connections(0);

client_connection::client_connection(std::shared_ptr<thinger::asio::socket> socket,
                                     std::chrono::seconds timeout)
    : socket_(std::move(socket))
    , timeout_(timeout)
    , idle_timer_(socket_->get_io_context()) {
    ++connections;
    LOG_TRACE("created http client connection with timeout: {} seconds. total: {}",
              timeout.count(), connections.load());
}

client_connection::client_connection(std::shared_ptr<thinger::asio::unix_socket> socket,
                                     const std::string& path,
                                     std::chrono::seconds timeout)
    : socket_(std::move(socket))
    , socket_path_(path)
    , timeout_(timeout)
    , idle_timer_(socket_->get_io_context()) {
    ++connections;
    LOG_TRACE("created http client connection (for unix socket: {}) with timeout: {} seconds. total: {}",
              path, timeout.count(), connections.load());
}

client_connection::client_connection(boost::asio::io_context& io_context, proxy_route route,
                                     std::chrono::seconds timeout)
    : socket_(std::make_shared<thinger::asio::tcp_socket>("http_client", io_context))
    , route_(std::move(route))
    , timeout_(timeout)
    , idle_timer_(io_context) {
    ++connections;
    LOG_TRACE("created http client connection through proxy {} with timeout: {} seconds. total: {}",
              route_.proxy ? route_.proxy->url() : "", timeout.count(), connections.load());
}

client_connection::~client_connection() {
    --connections;
    LOG_TRACE("releasing http client connection. total: {}", connections.load());
}

awaitable<boost::system::error_code> client_connection::ensure_connected(const http_request& request) {
    if (socket_->is_open()) {
        co_return boost::system::error_code{};
    }
    if (route_.proxy) {
        co_return co_await connect_through_proxy(request);
    }
    co_return co_await connect_socket(request.get_host(), request.get_port());
}

awaitable<boost::system::error_code> client_connection::connect_socket(const std::string& host, const std::string& port) {
    boost::system::error_code ec;
    LOG_TRACE("connecting to: {}:{}", host, port);

    for (unsigned retry = 0; retry < MAX_RETRIES; ++retry) {
        if (socket_path_.empty()) {
            ec = co_await socket_->connect(host, port, CONNECT_TIMEOUT);
        } else {
            auto unix_socket = std::static_pointer_cast<thinger::asio::unix_socket>(socket_);
            ec = co_await unix_socket->connect(socket_path_, CONNECT_TIMEOUT);
        }

        if (!ec) {
            LOG_TRACE("connection established");
            // Each request is written at once: with Nagle, a write while the previous one is
            // unacknowledged waits for the delayed ACK of the server (40 ms), as happens on a
            // reused connection. Go, Node and curl disable it too
            if (auto tcp = std::dynamic_pointer_cast<thinger::asio::tcp_socket>(socket_)) {
                tcp->enable_tcp_no_delay();
            }
            co_return ec;
        }

        LOG_ERROR("error while connecting (attempt #{}): {} ({}) ({})",
                  retry + 1, ec.message(), ec.value(), ec.category().name());

        // Don't retry for certain errors, nor a failed TLS handshake (e.g. a certificate
        // that cannot be verified): it would fail the same way
        if (ec == boost::asio::error::operation_aborted ||
            ec == boost::asio::error::host_not_found ||
            ec.category() == boost::asio::error::get_ssl_category() ||
            ec.category() == boost::asio::ssl::error::get_stream_category()) {
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

awaitable<boost::system::error_code> client_connection::connect_through_proxy(const http_request& request) {
    // A new TCP socket every time: the previous one may have become a TLS stream
    auto tcp = std::make_shared<thinger::asio::tcp_socket>("http_client", socket_->get_io_context());
    socket_ = tcp;
    if (auto ec = co_await connect_socket(route_.proxy->host, route_.proxy->port)) {
        co_return ec;
    }
    if (!route_.tunnel) {
        co_return boost::system::error_code{};
    }
    if (auto ec = co_await open_tunnel(request)) {
        co_return ec;
    }
    if (!route_.tls) {
        co_return boost::system::error_code{};
    }

    // TLS with the origin, end to end through the tunnel: SNI and the certificate verified
    // for the name of the origin, not of the proxy
    auto tls = std::make_shared<thinger::asio::ssl_socket>("http_client", tcp, route_.tls);
    socket_ = tls;
    auto ec = co_await tls->handshake(request.get_host());
    if (ec) {
        LOG_ERROR("TLS handshake with {} through the proxy {} failed: {}",
                  request.get_host(), route_.proxy->url(), ec.message());
    }
    co_return ec;
}

awaitable<boost::system::error_code> client_connection::open_tunnel(const http_request& request) {
    const auto& host = request.get_host();
    std::string authority = (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + request.get_port();
    http_request connect;
    set_connect_request(connect, request, authority, route_.proxy->authorization);
    LOG_DEBUG("opening a tunnel to {} through the proxy {}", authority, route_.proxy->url());

    std::vector<boost::asio::const_buffer> buffers;
    connect.to_buffer(buffers);
    if (auto [ec, bytes] = co_await socket_->write(buffers); ec) {
        co_return ec;
    }

    // Its answer is parsed as strictly as any response: a 2xx to CONNECT has no body, and
    // the tunnel starts right after it
    response_factory parser;
    parser.set_request_method(method::CONNECT);
    parser.set_max_content_size(MAX_PROXY_RESPONSE_SIZE);
    while (true) {
        auto [ec, bytes] = co_await socket_->read_some(buffer_, MAX_BUFFER_SIZE);

        uint8_t* begin = buffer_;
        uint8_t* end = buffer_ + bytes;
        boost::tribool result = boost::indeterminate;
        if (begin != end) {
            result = parser.parse_some(begin, end);
        }
        if (boost::indeterminate(result) && ec) {
            if (ec != boost::asio::error::eof) co_return ec;
            result = parser.end_of_stream();
        }
        if (!result) {
            LOG_WARNING("invalid answer to CONNECT from the proxy {}: {}", route_.proxy->url(),
                        make_error_code(parser.error()).message());
            co_return parser.error();
        }
        if (boost::indeterminate(result)) continue;

        auto response = parser.consume_response();
        int status = response->get_status_code();
        if (status >= 200 && status < 300) {
            // Nothing comes from the origin before the client speaks (TLS, or its request)
            if (begin != end) co_return client_error::proxy_tunnel_failed;
            co_return boost::system::error_code{};
        }
        LOG_WARNING("the proxy {} refused a tunnel to {}: {}", route_.proxy->url(), authority, status);
        proxy_response_ = std::move(response);
        co_return status == 407 ? client_error::proxy_authentication_required : client_error::proxy_tunnel_failed;
    }
}

awaitable<io_result> client_connection::write_to_proxy(const http_request& request) {
    // The absolute-form target names the origin (RFC 9112 section 3.2.2). The proxy consumes
    // its credentials (RFC 9110 section 11.7.2): unless the request has its own, they are
    // written here, never added to the request
    std::string target = request.get_url();
    std::string credentials;
    if (!route_.proxy->authorization.empty() && !request.has_header("Proxy-Authorization")) {
        credentials = "Proxy-Authorization: " + route_.proxy->authorization + "\r\n";
    }
    std::vector<boost::asio::const_buffer> buffers;
    buffers.reserve(32);
    request.to_buffer(buffers, target, credentials);
    co_return co_await socket_->write(buffers);
}

awaitable<std::shared_ptr<http_response>> client_connection::read_response(const http_request& request) {
    while (true) {
        auto [ec, bytes] = co_await socket_->read_some(buffer_, MAX_BUFFER_SIZE);
        if (bytes > 0) response_started_ = true;

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
        co_return complete_response(request, begin, end);
    }
}

std::shared_ptr<http_response> client_connection::complete_response(const http_request& request,
                                                                   const uint8_t* begin, const uint8_t* end) {
    // Data past the response, other than that of a switched protocol (another response
    // than the one requested, or garbage), leaves the connection out of sync. A client
    // asking to close it sends nothing more on it (RFC 9112 section 9.6)
    bool reusable = response_parser_.keep_alive() && begin == end &&
                    !has_connection_option(request.get_header(header::connection), "close");
    auto response = response_parser_.consume_response();
    if (reusable) {
        reusable_ = true;
        if (response->has_header("Keep-Alive")) {
            keep_alive_timeout_ = keep_alive_timeout_of(response->get_header("Keep-Alive"));
        }
    }
    if (response->get_status_code() == 101) {
        if (!request.has_header(header::upgrade)) {
            error_ = client_error::unexpected_upgrade;
            return nullptr;
        }
        // What followed the response belongs to the new protocol, e.g. WebSocket frames
        buffered_.assign(reinterpret_cast<const char*>(begin), static_cast<size_t>(end - begin));
    } else if (!reusable) {
        socket_->close();
    }

    // A streamed body is handed as received. A body that cannot be decoded fails the
    // request, as one too large once decoded, and the connection is closed as after any
    // error
    if (decompress_ && !response_parser_.is_streaming()) {
        if (auto error = decompress(*response, response_parser_.get_max_content_size()); error != client_error::none) {
            error_ = error;
            return nullptr;
        }
    }
    return response;
}

awaitable<std::shared_ptr<http_response>> client_connection::exchange(std::shared_ptr<http_request> request) {
    std::shared_ptr<http_response> response;
    bool timed_out = false;
    error_ = {};
    buffered_.clear();
    proxy_response_.reset();
    reused_ = socket_->is_open();
    request_unsent_ = false;
    response_started_ = false;
    reusable_ = false;
    keep_alive_timeout_.reset();

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
        io_result written;
        if (route_.proxy && !route_.tunnel) {
            written = co_await write_to_proxy(*request);
        } else {
            written = co_await request->to_socket(socket_);
        }
        if (auto [write_ec, bytes] = written; write_ec) {
            error_ = write_ec;
            request_unsent_ = bytes == 0;
            co_return;
        }
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
    if (!response) {
        reusable_ = false;
        close();
    }

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
                // The head is parsed: the callback may stop the download before its end
                if (auto head = response_parser_.get_response()) set_headers(result, *head);
            }
            result.bytes_transferred = downloaded;

            stream_info info{data, downloaded, total, result.status_code};
            return callback(info);
        });

    auto response = co_await exchange(std::move(request));
    if (!response) {
        result.error = error_.message();
        // The status of a proxy refusing the tunnel, with its error
        if (proxy_response_) {
            result.status_code = proxy_response_->get_status_code();
            set_headers(result, *proxy_response_);
        }
        co_return result;
    }

    result.status_code = response->get_status_code();
    set_headers(result, *response);
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

void client_connection::expire_after(std::chrono::steady_clock::duration timeout, std::function<void()> on_expired) {
    idle_timer_.expires_after(timeout);
    idle_timer_.async_wait([on_expired = std::move(on_expired)](const boost::system::error_code& ec) {
        if (!ec) on_expired();
    });
}

std::string client_connection::take_buffered() {
    return std::exchange(buffered_, {});
}

std::shared_ptr<thinger::asio::socket> client_connection::release_socket() {
    socket_->cancel();
    return socket_;
}

}

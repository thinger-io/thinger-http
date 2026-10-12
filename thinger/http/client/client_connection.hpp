#ifndef THINGER_HTTP_CLIENT_CONNECTION_HPP
#define THINGER_HTTP_CLIENT_CONNECTION_HPP

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <boost/noncopyable.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>

#include "../common/http_request.hpp"
#include "../common/http_response.hpp"
#include "proxy.hpp"
#include "response_factory.hpp"
#include "stream_types.hpp"
#include "../../asio/sockets/tcp_socket.hpp"
#include "../../asio/sockets/unix_socket.hpp"
#include "../../util/types.hpp"

namespace thinger::http {

// How a connection reaches its origin through an HTTP proxy
struct proxy_route {
    std::shared_ptr<const proxy_endpoint> proxy;
    // Open a tunnel to the origin with CONNECT (https, ws, wss); otherwise the requests are
    // sent to the proxy, with an absolute-form target (plain http)
    bool tunnel = false;
    // TLS with the origin inside the tunnel; null for none
    std::shared_ptr<boost::asio::ssl::context> tls;
};

class client_connection : public std::enable_shared_from_this<client_connection>, public boost::noncopyable {

    static constexpr unsigned MAX_BUFFER_SIZE = 4096;
    static constexpr unsigned MAX_RETRIES = 3;
    static constexpr auto DEFAULT_TIMEOUT = std::chrono::seconds{60};
    static constexpr auto CONNECT_TIMEOUT = std::chrono::seconds{10};
    // Largest answer of a proxy refusing a tunnel (an error page) read whole
    static constexpr size_t MAX_PROXY_RESPONSE_SIZE = 1048576;

public:
    static std::atomic<unsigned long> connections;

    // Constructors
    explicit client_connection(std::shared_ptr<thinger::asio::socket> socket,
                               std::chrono::seconds timeout = DEFAULT_TIMEOUT);

    client_connection(std::shared_ptr<thinger::asio::unix_socket> socket,
                      const std::string& path,
                      std::chrono::seconds timeout = DEFAULT_TIMEOUT);

    // Through an HTTP proxy (it connects on the first request)
    client_connection(boost::asio::io_context& io_context, proxy_route route,
                      std::chrono::seconds timeout = DEFAULT_TIMEOUT);

    virtual ~client_connection();

    // Main operation - send request and get response (null on error: see last_error())
    awaitable<std::shared_ptr<http_response>> send_request(std::shared_ptr<http_request> request);

    // Streaming operation - send request and stream response through callback
    awaitable<stream_result> send_request_streaming(
        std::shared_ptr<http_request> request,
        stream_callback callback);

    // Connection management
    void close();
    std::shared_ptr<thinger::asio::socket> release_socket();

    // Why the last request failed: connection, timeout, or an invalid response
    // (client_error). The connection is closed after any of them
    const boost::system::error_code& last_error() const { return error_; }

    // The answer of the proxy when it refused to open a tunnel (last_error() is then
    // proxy_authentication_required or proxy_tunnel_failed), null otherwise
    const std::shared_ptr<http_response>& proxy_response() const { return proxy_response_; }

    // Bytes received after the last response, read along with it (e.g. data the server
    // sent right after a protocol upgrade response): they come before anything read from
    // the released socket
    std::string take_buffered();
    std::shared_ptr<thinger::asio::socket> get_socket() const { return socket_; }
    bool is_open() const { return socket_ && socket_->is_open(); }

    // A connection serves one request at a time: whoever hands it out claims it first (the
    // pool, taking it), and it is unclaimed when idle again (given back to the pool). False
    // while another request is using it
    bool claim() { return !claimed_.exchange(true); }
    void unclaim() { claimed_ = false; }

    // After a request: whether the connection can carry another one (RFC 9112 section 9.3):
    // the response was read whole, neither side asked to close it, nothing past the response
    // was received, and it did not switch protocols. Virtual for test doubles
    virtual bool reusable() const { return reusable_ && is_open(); }

    // While idle: whether it can no longer be used: closed, or the peer closed it, reset it,
    // or sent something unasked. Checked without waiting before reusing it. Virtual for test
    // doubles
    virtual bool stale() const { return !is_open() || socket_->readable_now(); }

    // How the last request failed, to tell whether it can be sent again on a new connection:
    // the connection was open before it (reused, not connected for it), nothing of the
    // request was written, or some byte of the response was received
    bool reused() const { return reused_; }
    bool request_unsent() const { return request_unsent_; }
    bool response_started() const { return response_started_; }

    // The idle timeout the server announced with its last response (Keep-Alive: timeout=N)
    std::optional<std::chrono::seconds> keep_alive_timeout() const { return keep_alive_timeout_; }

    // While idle in a pool: call `on_expired` (on the io_context of the connection) after
    // `timeout`. Calling it again replaces the previous wait (completed with an error)
    void expire_after(std::chrono::steady_clock::duration timeout, std::function<void()> on_expired);

    // Forward max response size to the underlying parser. It bounds buffered bodies once
    // decompressed too
    void set_max_content_size(size_t size) { response_parser_.set_max_content_size(size); }
    void set_timeout(std::chrono::seconds timeout) { timeout_ = timeout; }
    // Decode gzip and deflate bodies (the default); otherwise they are kept as received
    void set_decompress(bool decompress) { decompress_ = decompress; }

private:
    // Internal helpers
    awaitable<boost::system::error_code> ensure_connected(const http_request& request);
    awaitable<boost::system::error_code> connect_socket(const std::string& host, const std::string& port);
    awaitable<boost::system::error_code> connect_through_proxy(const http_request& request);
    awaitable<boost::system::error_code> open_tunnel(const http_request& request);
    awaitable<io_result> write_to_proxy(const http_request& request);
    awaitable<std::shared_ptr<http_response>> exchange(std::shared_ptr<http_request> request);
    awaitable<std::shared_ptr<http_response>> read_response(const http_request& request);

    std::shared_ptr<thinger::asio::socket> socket_;
    std::string socket_path_;
    proxy_route route_;
    std::shared_ptr<http_response> proxy_response_;
    std::chrono::seconds timeout_;
    uint8_t buffer_[MAX_BUFFER_SIZE];
    std::string buffered_;
    response_factory response_parser_;
    boost::system::error_code error_;
    std::atomic<bool> claimed_{false};
    bool reused_ = false;
    bool request_unsent_ = false;
    bool response_started_ = false;
    bool reusable_ = false;
    bool decompress_ = true;
    std::optional<std::chrono::seconds> keep_alive_timeout_;
    boost::asio::steady_timer idle_timer_;
};

}

#endif

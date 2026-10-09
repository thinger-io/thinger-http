#ifndef THINGER_HTTP_SERVER_BASE_HPP
#define THINGER_HTTP_SERVER_BASE_HPP

#include "routing/route_handler.hpp"
#include "routing/route.hpp"
#include "virtual_host.hpp"
#include "error_format.hpp"
#include "trusted_proxies.hpp"
#include "openapi.hpp"
#include "http_stream.hpp"
#include "../../asio/socket_server.hpp"
#include "../../asio/socket_server_base.hpp"
#include "../../asio/unix_socket_server.hpp"
#include "../../util/types.hpp"
#include <memory>
#include <string>
#include <functional>
#include <chrono>
#include <map>
#include <vector>

namespace thinger::http {

// Forward declarations
class request;
class response;

// Options for requests dispatched in memory (see http_server_base::dispatch)
struct dispatch_options {
    // Address of the peer, reported by request::get_peer_ip() and, unless it is a trusted
    // proxy (see set_trusted_proxies), by request::get_request_ip(). Empty by default:
    // never assume a trusted address such as 127.0.0.1 for internal calls.
    std::string remote_ip;

    // Maximum time to wait for the response, handler execution included; 504 Gateway
    // Timeout if exceeded (the handler is then cancelled: its pending awaits are aborted)
    std::chrono::milliseconds timeout{30000};
};

// Middlewares run after route matching (request::get_matched_route() is set, or null
// if no route matched) and before the request body is read, so they cannot see it.
// They share the response object with the route handler.

// Asynchronous middleware: co_return true to continue the chain, or respond through
// the response and co_return false to stop it (the route handler is not called).
using async_middleware_function = std::function<thinger::awaitable<bool>(request&, response&)>;

// Synchronous middleware: call next() before returning to continue the chain, or
// respond through the response to stop it. next() must not be called asynchronously.
using middleware_function = std::function<void(request&, response&, std::function<void()>)>;

// The server is the default virtual host: the routes registered on it (see virtual_host
// for the registration methods) answer every host without a virtual host of its own.
class http_server_base : public virtual_host {
protected:
    openapi_generator openapi_{router_};
    std::unique_ptr<asio::socket_server_base> socket_server_;
    std::vector<async_middleware_function> middlewares_;
    std::string host_ = "0.0.0.0";
    std::string port_ = "8080";
    std::string unix_path_;
    bool cors_enabled_{false};
    bool ssl_enabled_{false};
    bool use_unix_socket_{false};
    
    // Connection timeout setting
    std::chrono::seconds connection_timeout_{120};

    // Maximum allowed request body size
    size_t max_body_size_{8 * 1024 * 1024}; // 8MB default
    
    // Listening attempts (-1 = infinite)
    int max_listening_attempts_ = -1;

    // Virtual hosts by exact name, and by pattern (in registration order)
    std::map<std::string, std::unique_ptr<virtual_host>> exact_hosts_;
    std::vector<std::unique_ptr<virtual_host>> pattern_hosts_;

    // Body of error responses (default format if null)
    std::shared_ptr<const error_formatter> error_formatter_;

    // Proxies allowed to report the client address (none if null)
    std::shared_ptr<const trusted_proxies> trusted_proxies_;
    
public:
    http_server_base() = default;
    virtual ~http_server_base() = default;
    
    // Virtual hosts: the routes registered on the server itself answer any host, unless
    // the Host header (case-insensitive, port ignored) matches a host registered here:
    // exact names first, then patterns in registration order. Register before listen().
    // name: exact ("api.example.com"), "*" (this default host), or a pattern where "*" is
    // one label and ":name" a label available as a request parameter (see virtual_host)
    virtual_host& host(const std::string& name);

    // Host matched by a regular expression on the whole host name (case-insensitive);
    // its capture groups are available through request::get_host_matches()
    virtual_host& host_regex(const std::string& pattern);

    // OpenAPI document generated from the routes of the default host (configure title,
    // hooks... here). For another host: openapi_generator(server.host(name).router()).
    openapi_generator& openapi() { return openapi_; }

    // Serve the OpenAPI document as JSON at `path` (not served unless called)
    route& serve_openapi(const std::string& path = "/openapi.json");

    // Dispatch a request in memory, without a connection, through the same steps as a
    // network request: route matching, middlewares, body reading and validation, handler,
    // and the same 404/405 handling. Useful for tests, internal calls and gateways.
    // The body is taken from the request content (set_content); chunked requests are not
    // supported. WebSocket, SSE and connection takeover answer 501.
    awaitable<std::shared_ptr<http_response>> dispatch(std::shared_ptr<http_request> request,
                                                       dispatch_options options = {});

    // Same, calling `callback` with the response; runs on `executor`
    void dispatch(const boost::asio::any_io_executor& executor,
                  std::shared_ptr<http_request> request,
                  std::function<void(std::shared_ptr<http_response>)> callback,
                  dispatch_options options = {});

    // Middleware (register before listen(), executed in registration order)
    void use(async_middleware_function middleware);
    void use(middleware_function middleware);
    
    // Basic Auth helpers
    using auth_verify_function = std::function<bool(const std::string& username, const std::string& password)>;
    
    // Add basic auth for a specific path prefix
    void set_basic_auth(const std::string& path_prefix, 
                       const std::string& realm,
                       auth_verify_function verify);
    
    // Add basic auth with simple username/password
    void set_basic_auth(const std::string& path_prefix,
                       const std::string& realm,
                       const std::string& username,
                       const std::string& password);
    
    // Add basic auth with multiple users
    void set_basic_auth(const std::string& path_prefix,
                       const std::string& realm,
                       const std::map<std::string, std::string>& users);
    
    // Configuration
    void enable_cors(bool enabled = true);
    void enable_ssl(bool enabled = true);
    void set_connection_timeout(std::chrono::seconds timeout);
    void set_max_body_size(size_t size);
    void set_max_listening_attempts(int attempts);

    // Format of the error responses: those sent with response::error() and the ones the
    // server generates (400 invalid JSON or schema validation, 404, 405, 413, 500...).
    // By default the message is sent as text/plain, and errors with details (schema
    // validation) as JSON {"error": {"message": ..., "context": [...]}}. Set it before
    // listen(); it may be called concurrently from the worker threads.
    void set_error_formatter(error_formatter formatter);

    // Proxies (IPs or CIDR ranges: "10.0.0.1", "10.0.0.0/8", "fd00::/8") whose forwarding
    // header gives the client address reported by request::get_request_ip(). The header
    // is ignored for any other peer, so only list proxies that overwrite or append to it.
    // None by default; set before listen(). Returns false if an entry is invalid (then no
    // proxy is trusted).
    bool set_trusted_proxies(const std::vector<std::string>& proxies,
                             forwarded_header header = forwarded_header::x_forwarded_for);
    
    // Server control
    virtual bool listen(const std::string& host, uint16_t port);
    virtual bool listen_unix(const std::string& unix_path);

    // Start methods with automatic wait()
    bool start(uint16_t port);
    bool start(uint16_t port, const std::function<void()> &on_listening);
    bool start(const std::string& host, uint16_t port);
    bool start(const std::string& host, uint16_t port, const std::function<void()> &on_listening);
    
    // Unix socket start methods
    bool start_unix(const std::string& unix_path);
    bool start_unix(const std::string& unix_path, const std::function<void()> &on_listening);

    virtual bool stop();
    
    // Check if server is listening
    bool is_listening() const;

    // Get the port assigned by the OS after listen()
    uint16_t local_port() const;
    
    // Abstract methods that derived classes must implement
    virtual void wait() = 0;
    
protected:
    // Virtual method for creating socket server - can be overridden by subclasses
    virtual std::unique_ptr<asio::socket_server> create_socket_server(
        const std::string& host, const std::string& port) = 0;
    
    // Virtual method for creating Unix socket server
    virtual std::unique_ptr<asio::unix_socket_server> create_unix_socket_server(
        const std::string& unix_path) = 0;
    
private:
    void setup_connection_handler();
    virtual_host& resolve_host(request& req);
    std::shared_ptr<http_response> make_error_response(http_response::status status, const std::string& message) const;
    awaitable<void> process_request(std::shared_ptr<request> req, response& res);
    awaitable<void> handle_request(std::shared_ptr<request> req, response& res);
    awaitable<bool> run_middlewares(request& req, response& res);
    awaitable<void> discard_unread_body(request& req);
};

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_BASE_HPP
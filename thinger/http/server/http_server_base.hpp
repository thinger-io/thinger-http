#ifndef THINGER_HTTP_SERVER_BASE_HPP
#define THINGER_HTTP_SERVER_BASE_HPP

#include "routing/route_handler.hpp"
#include "routing/route.hpp"
#include "http_stream.hpp"
#include "../../asio/socket_server.hpp"
#include "../../asio/socket_server_base.hpp"
#include "../../asio/unix_socket_server.hpp"
#include "../../util/types.hpp"
#include <memory>
#include <string>
#include <functional>
#include <chrono>

namespace thinger::http {

// Forward declarations
class request;
class response;

// Middleware function type
using middleware_function = std::function<void(request&, response&, std::function<void()>)>;

class http_server_base {
protected:
    route_handler router_;
    std::unique_ptr<asio::socket_server_base> socket_server_;
    std::vector<middleware_function> middlewares_;
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
    
public:
    http_server_base() = default;
    virtual ~http_server_base() = default;
    
    // Route registration methods - all return route& for chaining
    route& get(const std::string& path, route_callback_response_only handler);
    route& get(const std::string& path, route_callback_json_response handler);
    route& get(const std::string& path, route_callback_request_response handler);
    route& get(const std::string& path, route_callback_request_json_response handler);
    
    route& post(const std::string& path, route_callback_response_only handler);
    route& post(const std::string& path, route_callback_json_response handler);
    route& post(const std::string& path, route_callback_request_response handler);
    route& post(const std::string& path, route_callback_request_json_response handler);
    
    route& put(const std::string& path, route_callback_response_only handler);
    route& put(const std::string& path, route_callback_json_response handler);
    route& put(const std::string& path, route_callback_request_response handler);
    route& put(const std::string& path, route_callback_request_json_response handler);
    
    route& del(const std::string& path, route_callback_response_only handler);  // delete is keyword
    route& del(const std::string& path, route_callback_json_response handler);
    route& del(const std::string& path, route_callback_request_response handler);
    route& del(const std::string& path, route_callback_request_json_response handler);
    
    route& patch(const std::string& path, route_callback_response_only handler);
    route& patch(const std::string& path, route_callback_json_response handler);
    route& patch(const std::string& path, route_callback_request_response handler);
    route& patch(const std::string& path, route_callback_request_json_response handler);
    
    route& head(const std::string& path, route_callback_response_only handler);
    route& head(const std::string& path, route_callback_request_response handler);
    
    route& options(const std::string& path, route_callback_response_only handler);
    route& options(const std::string& path, route_callback_request_response handler);

    // Awaitable (deferred body) route registration — auto-enables deferred_body
    // Uses template + requires to avoid ambiguity with std::function<void(...)> overloads
    template<typename F>
        requires requires(F f, request& req, response& res) {
            { f(req, res) } -> std::same_as<thinger::awaitable<void>>;
        }
    route& get(const std::string& path, F&& handler) {
        return router_[method::GET][path] = route_callback_awaitable(std::forward<F>(handler));
    }

    template<typename F>
        requires requires(F f, request& req, response& res) {
            { f(req, res) } -> std::same_as<thinger::awaitable<void>>;
        }
    route& post(const std::string& path, F&& handler) {
        return router_[method::POST][path] = route_callback_awaitable(std::forward<F>(handler));
    }

    template<typename F>
        requires requires(F f, request& req, response& res) {
            { f(req, res) } -> std::same_as<thinger::awaitable<void>>;
        }
    route& put(const std::string& path, F&& handler) {
        return router_[method::PUT][path] = route_callback_awaitable(std::forward<F>(handler));
    }

    template<typename F>
        requires requires(F f, request& req, response& res) {
            { f(req, res) } -> std::same_as<thinger::awaitable<void>>;
        }
    route& del(const std::string& path, F&& handler) {
        return router_[method::DELETE][path] = route_callback_awaitable(std::forward<F>(handler));
    }

    template<typename F>
        requires requires(F f, request& req, response& res) {
            { f(req, res) } -> std::same_as<thinger::awaitable<void>>;
        }
    route& patch(const std::string& path, F&& handler) {
        return router_[method::PATCH][path] = route_callback_awaitable(std::forward<F>(handler));
    }

    // Middleware
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
    
    // Fallback handler
    void set_not_found_handler(route_callback_response_only handler);
    void set_not_found_handler(route_callback_request_response handler);
    
    // Configuration
    void enable_cors(bool enabled = true);
    void enable_ssl(bool enabled = true);
    void set_connection_timeout(std::chrono::seconds timeout);
    void set_max_body_size(size_t size);
    void set_max_listening_attempts(int attempts);
    
    // Static file serving
    void serve_static(const std::string& url_prefix, 
                     const std::string& directory,
                     bool fallback_to_index = true);
    
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
    
    // Access to router for advanced use cases
    route_handler& router() { return router_; }
    const route_handler& router() const { return router_; }
    
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
    void execute_middlewares(request& req, std::shared_ptr<http_stream> stream, size_t index, std::function<void()> final_handler);
};

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_BASE_HPP
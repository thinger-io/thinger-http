#ifndef THINGER_HTTP_SERVER_BASE_HPP
#define THINGER_HTTP_SERVER_BASE_HPP

#include "routing/route_handler.hpp"
#include "routing/route.hpp"
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

namespace thinger::http {

// Forward declarations
class request;
class response;
class route_group;

// Middlewares run after route matching (request::get_matched_route() is set, or null
// if no route matched) and before the request body is read, so they cannot see it.
// They share the response object with the route handler.

// Asynchronous middleware: co_return true to continue the chain, or respond through
// the response and co_return false to stop it (the route handler is not called).
using async_middleware_function = std::function<thinger::awaitable<bool>(request&, response&)>;

// Synchronous middleware: call next() before returning to continue the chain, or
// respond through the response to stop it. next() must not be called asynchronously.
using middleware_function = std::function<void(request&, response&, std::function<void()>)>;

class http_server_base {
protected:
    route_handler router_;
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

    // Coroutine route registration. See coroutine_handler (route.hpp) for the signatures:
    // handlers taking a JSON body get it read, parsed and validated first; (request&, response&)
    // handlers read the body themselves unless deferred_body(false) is set on the route.
    // Templates avoid ambiguity with the std::function<void(...)> overloads above.
    template<coroutine_handler F>
    route& get(const std::string& path, F&& handler) {
        return router_[method::GET][path] = std::forward<F>(handler);
    }

    template<coroutine_handler F>
    route& post(const std::string& path, F&& handler) {
        return router_[method::POST][path] = std::forward<F>(handler);
    }

    template<coroutine_handler F>
    route& put(const std::string& path, F&& handler) {
        return router_[method::PUT][path] = std::forward<F>(handler);
    }

    template<coroutine_handler F>
    route& del(const std::string& path, F&& handler) {
        return router_[method::DELETE][path] = std::forward<F>(handler);
    }

    template<coroutine_handler F>
    route& patch(const std::string& path, F&& handler) {
        return router_[method::PATCH][path] = std::forward<F>(handler);
    }

    // Group of routes sharing a path prefix, tags and metadata (see route_group)
    route_group group(const std::string& prefix);

    // Shared JSON schema (OpenAPI component) that route schemas can reference with
    // {"$ref": "#/components/schemas/<name>"}. Register it before the routes using it.
    void schema_component(const std::string& name, nlohmann::json schema);

    // OpenAPI document generated from the routes (configure title, hooks... here)
    openapi_generator& openapi() { return openapi_; }

    // Serve the OpenAPI document as JSON at `path` (not served unless called)
    route& serve_openapi(const std::string& path = "/openapi.json");

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
                     const std::string& fallback = "index.html");
    
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
    awaitable<bool> run_middlewares(request& req, response& res);
    awaitable<void> discard_unread_body(request& req);
};

// Routes registered through a group get its path prefix, and inherit its tags and
// metadata (a route can still override a metadata key with its own meta()).
class route_group {
public:
    route_group(http_server_base& server, std::string prefix)
        : server_(server), prefix_(std::move(prefix)) {}

    route_group& tag(const std::string& name) {
        tags_.push_back(name);
        return *this;
    }

    route_group& meta(const std::string& key, nlohmann::json value) {
        metadata_[key] = std::move(value);
        return *this;
    }

    // Nested group: prefix appended, tags and metadata inherited
    route_group group(const std::string& prefix) const {
        route_group nested(*this);
        nested.prefix_ += prefix;
        return nested;
    }

    const std::string& prefix() const { return prefix_; }

    template<typename F> route& get(const std::string& path, F&& handler) {
        return apply(server_.get(prefix_ + path, std::forward<F>(handler)));
    }
    template<typename F> route& post(const std::string& path, F&& handler) {
        return apply(server_.post(prefix_ + path, std::forward<F>(handler)));
    }
    template<typename F> route& put(const std::string& path, F&& handler) {
        return apply(server_.put(prefix_ + path, std::forward<F>(handler)));
    }
    template<typename F> route& del(const std::string& path, F&& handler) {
        return apply(server_.del(prefix_ + path, std::forward<F>(handler)));
    }
    template<typename F> route& patch(const std::string& path, F&& handler) {
        return apply(server_.patch(prefix_ + path, std::forward<F>(handler)));
    }
    template<typename F> route& head(const std::string& path, F&& handler) {
        return apply(server_.head(prefix_ + path, std::forward<F>(handler)));
    }
    template<typename F> route& options(const std::string& path, F&& handler) {
        return apply(server_.options(prefix_ + path, std::forward<F>(handler)));
    }

private:
    route& apply(route& r) const {
        r.tags(tags_);
        for (const auto& [key, value] : metadata_.items()) {
            r.meta(key, value);
        }
        return r;
    }

    http_server_base& server_;
    std::string prefix_;
    std::vector<std::string> tags_;
    nlohmann::json metadata_ = nlohmann::json::object();
};

inline route_group http_server_base::group(const std::string& prefix) {
    return route_group(*this, prefix);
}

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_BASE_HPP
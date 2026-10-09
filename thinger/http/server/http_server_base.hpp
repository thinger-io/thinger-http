#ifndef THINGER_HTTP_SERVER_BASE_HPP
#define THINGER_HTTP_SERVER_BASE_HPP

#include "http_application.hpp"
#include "routing/route_registrar.hpp"
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

// A server receives requests on a listener (TCP, with or without TLS, or a Unix socket)
// and passes them to its application (see http_application), which holds the routes,
// virtual hosts, middlewares and request processing settings. The application methods
// are also available here, forwarded to it.
class http_server_base : public route_registrar<http_server_base> {
protected:
    std::shared_ptr<http_application> app_ = std::make_shared<http_application>();
    std::unique_ptr<asio::socket_server_base> socket_server_;
    std::string host_ = "0.0.0.0";
    std::string port_ = "8080";
    std::string unix_path_;
    bool ssl_enabled_{false};
    bool use_unix_socket_{false};
    
    // Connection timeout setting
    std::chrono::seconds connection_timeout_{120};

    // Listening attempts (-1 = infinite)
    int max_listening_attempts_ = -1;
    
public:
    http_server_base() = default;
    virtual ~http_server_base() = default;

    // Application served: routes, hosts, middlewares... (a new one by default)
    http_application& application() { return *app_; }
    const http_application& application() const { return *app_; }

    // Serve another application (e.g. one shared with other servers); set before listen()
    void set_application(std::shared_ptr<http_application> app) { app_ = std::move(app); }

    // --- Application methods, forwarded (see http_application and virtual_host) ---
    // Route registration: get, post, put, del, patch, head and options (see route_registrar)
    route_group group(const std::string& prefix) { return app_->group(prefix); }
    void schema_component(const std::string& name, nlohmann::json schema) {
        app_->schema_component(name, std::move(schema));
    }
    void serve_static(const std::string& url_prefix, const std::string& directory,
                      const std::string& fallback = "index.html") {
        app_->serve_static(url_prefix, directory, fallback);
    }
    template<request_handler_callable F>
    void set_not_found_handler(F&& handler) { app_->set_not_found_handler(std::forward<F>(handler)); }
    void set_not_found_handler(std::nullptr_t) { app_->set_not_found_handler(nullptr); }
    route_handler& router() { return app_->router(); }
    const route_handler& router() const { return app_->router(); }

    virtual_host& host(const std::string& name) { return app_->host(name); }
    virtual_host& host_regex(const std::string& pattern) { return app_->host_regex(pattern); }
    openapi_generator& openapi() { return app_->openapi(); }
    route& serve_openapi(const std::string& path = "/openapi.json") { return app_->serve_openapi(path); }

    awaitable<std::shared_ptr<http_response>> dispatch(std::shared_ptr<http_request> request,
                                                       dispatch_options options = {}) {
        return app_->dispatch(std::move(request), std::move(options));
    }
    void dispatch(const boost::asio::any_io_executor& executor, std::shared_ptr<http_request> request,
                  std::function<void(std::shared_ptr<http_response>)> callback, dispatch_options options = {}) {
        app_->dispatch(executor, std::move(request), std::move(callback), std::move(options));
    }

    void use(async_middleware_function middleware) { app_->use(std::move(middleware)); }
    void use(middleware_function middleware) { app_->use(std::move(middleware)); }

    using auth_verify_function = http_application::auth_verify_function;
    void set_basic_auth(const std::string& path_prefix, const std::string& realm, auth_verify_function verify) {
        app_->set_basic_auth(path_prefix, realm, std::move(verify));
    }
    void set_basic_auth(const std::string& path_prefix, const std::string& realm,
                        const std::string& username, const std::string& password) {
        app_->set_basic_auth(path_prefix, realm, username, password);
    }
    void set_basic_auth(const std::string& path_prefix, const std::string& realm,
                        const std::map<std::string, std::string>& users) {
        app_->set_basic_auth(path_prefix, realm, users);
    }

    void enable_cors(bool enabled = true) { app_->enable_cors(enabled); }
    void set_max_body_size(size_t size) { app_->set_max_body_size(size); }
    void set_error_formatter(error_formatter formatter) { app_->set_error_formatter(std::move(formatter)); }
    bool set_trusted_proxies(const std::vector<std::string>& proxies,
                             forwarded_header header = forwarded_header::x_forwarded_for) {
        return app_->set_trusted_proxies(proxies, header);
    }

    // --- Server configuration ---
    void enable_ssl(bool enabled = true);
    void set_connection_timeout(std::chrono::seconds timeout);
    void set_max_listening_attempts(int attempts);
    
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
    friend class route_registrar<http_server_base>;
    route& make_route(method http_method, const std::string& path) {
        return app_->router()[http_method][path];
    }

    void setup_connection_handler();
};

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_BASE_HPP
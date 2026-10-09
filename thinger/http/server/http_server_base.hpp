#ifndef THINGER_HTTP_SERVER_BASE_HPP
#define THINGER_HTTP_SERVER_BASE_HPP

#include "http_application.hpp"
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

// A server is an application (see http_application: routes, virtual hosts, middlewares and
// request processing settings) served on a listener: TCP, with or without TLS, or a Unix
// socket. It only adds the listener, TLS and connection settings. An http_application can
// also be used on its own, e.g. for in-memory dispatch.
class http_server_base : public http_application {
protected:
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
    ~http_server_base() override = default;

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
    void setup_connection_handler();
};

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_BASE_HPP
#ifndef THINGER_HTTP_SERVER_BASE_HPP
#define THINGER_HTTP_SERVER_BASE_HPP

#include "http_application.hpp"
#include "connection_tracker.hpp"
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
#include <mutex>
#include <optional>

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

    // The connections served, for the statistics and the shutdown
    std::shared_ptr<connection_tracker> connections_ = std::make_shared<connection_tracker>();
    
public:
    http_server_base() = default;
    ~http_server_base() override;

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

    // Stop listening right away. Connections open are left as they are (see shutdown())
    virtual bool stop();

    // Graceful shutdown: stop listening right away (new connections are refused), close the
    // idle connections, and let those handling a request finish it: its response tells the
    // client the connection closes (Connection: close), and then it closes. Connections taken
    // over (WebSockets, event streams, tunnels...) are told to finish (see on_shutdown(),
    // websocket_connection::on_shutdown(), sse_connection::on_shutdown(), on_server_shutdown()).
    // Whatever is still open once `grace` passes is closed. Then the server stops, as with
    // stop() (wait() returns).
    //
    // shutdown() waits until it ends, and returns whether every connection finished before the
    // grace period ended: it must not be called from a thread serving the server (a handler, or
    // any worker of a pool_server), where it starts the shutdown and returns false at once.
    // begin_shutdown() returns at once, calling `on_done` (if any) with the same result when it
    // ends, on a thread of the server (or the one calling stop() if that ends it first).
    //
    // Both may be called from any thread, more than once (the first call sets the grace period,
    // later ones wait for the same end), and from an asio::signal_set handler; neither is
    // async-signal-safe (they lock and allocate), so not from a raw signal handler. stop()
    // during a shutdown ends it at once (false), leaving the connections still open as they are.
    // The connections must be running: a server not running (wait() not called, or stopped)
    // ends it at once.
    bool shutdown(std::chrono::milliseconds grace = std::chrono::seconds(30));
    void begin_shutdown(std::chrono::milliseconds grace = std::chrono::seconds(30),
                        std::function<void(bool)> on_done = {});

    // Called once when a graceful shutdown starts, on the thread starting it, once the server
    // stopped listening: for the application to finish what it runs on connections taken over
    // (or anything else)
    void on_shutdown(std::function<void()> callback);

    // Connections open by what they serve, and served since the server was created. It locks
    // each list the connections are kept in (by io_context): meant to be read now and then (e.g.
    // by a monitoring endpoint), as counting costs nothing per request
    server_stats stats() const;
    
    // Check if server is listening
    bool is_listening() const;

    // Get the port assigned by the OS after listen(): 0 while not listening, never throws
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

    // Where a shutdown waits for the connections: an io_context running while the server
    // runs, or none if nothing runs the connections
    virtual boost::asio::io_context* shutdown_context() = 0;
    // Whether the calling thread serves connections of the server (shutdown() cannot wait there)
    virtual bool in_server_thread() = 0;

    // A shutdown in progress (if any) ends: stop() calls it first. Waits for one ending on
    // another thread
    void end_shutdown();

private:
    struct shutdown_state;

    void setup_connection_handler();
    bool stop_listening();
    void reset_shutdown();
    static void check_shutdown(const std::shared_ptr<shutdown_state>& state);
    static void wait_shutdown(const std::shared_ptr<shutdown_state>& state);
    static void force_shutdown(const std::shared_ptr<shutdown_state>& state);
    static void finish_shutdown(const std::shared_ptr<shutdown_state>& state, bool clean);
    static void complete_shutdown(const std::shared_ptr<shutdown_state>& state, bool clean);

    std::function<void()> on_shutdown_;
    // The last shutdown (guarded by its mutex)
    mutable std::mutex shutdown_mutex_;
    std::shared_ptr<shutdown_state> shutdown_;
};

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_BASE_HPP
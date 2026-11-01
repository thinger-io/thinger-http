#ifndef THINGER_ASIO_LISTENER_HPP
#define THINGER_ASIO_LISTENER_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>
#include <boost/asio/any_io_executor.hpp>
#include <boost/system/error_code.hpp>
#include "ssl/tls_session.hpp"
#include "../util/types.hpp"

namespace thinger::asio {

class socket;
class certificate_manager;

namespace detail {
// The internal switches of a listener (see detail/listener_access.hpp)
struct listener_access;
}

// A TCP endpoint to listen on: a host name or address (resolved synchronously by open()) and a
// port, 0 for one chosen by the system (see listener::local_port())
struct tcp_endpoint {
    std::string host = "0.0.0.0";
    uint16_t port = 0;
};

// A Unix domain socket to listen on: a file there before is replaced, and the file is removed
// when the listener stops
struct unix_endpoint {
    std::string path;
};

// How a listener listens. Plain data: the listener copies it
struct listener_options {
    std::variant<tcp_endpoint, unix_endpoint> endpoint;
    // The certificates of TLS: null for plain connections; one manager per listener, or shared
    // by several. The listener holds it until stop(), whatever open() returns: its connections
    // start with the default certificate open() found, and take the certificate and the client
    // certificate mode of their host name (SNI) from the manager. The application need not keep
    // a reference to it
    std::shared_ptr<certificate_manager> tls;
    // The client certificates asked for. A handshake that names a host (SNI) with a mode of its
    // own (see certificate_manager::set_client_verify()) gets that mode; every other handshake
    // gets this one. Ignored (logged) without tls
    client_verify_options client_verify;
    // How long a TLS client has to complete its handshake, from the connection accepted
    // (waiting for a slot included, see below): closed otherwise, without reaching the
    // handler. 0: no limit
    std::chrono::milliseconds handshake_timeout{60000};
    // TLS handshakes in progress at once per IO thread (per serve executor); a connection whose
    // thread is at the bound waits for a slot before its handshake starts, while accepting goes
    // on. 0: unbounded. With no handshake_timeout, connections waiting behind stalled
    // handshakes wait for as long as those do
    size_t max_pending_handshakes = 0;
    // Applied to the contexts of `tls`: listeners sharing a manager must agree (see open())
    session_cache_options session_cache;
    bool tcp_no_delay = true;
    // Attempts to bind before open() fails, every 5 s; -1: until it binds
    int max_listening_attempts = -1;
    // Remote addresses of TCP connections closed as they are accepted: a forbidden address is
    // always closed, and a non-empty allowed list closes every address not in it
    std::vector<std::string> allowed_remotes;
    std::vector<std::string> forbidden_remotes;
};

// The executor to accept on, or the next one to serve a connection on. In this release it must
// run on an io_context (an io_context executor, or a strand of one)
using executor_provider = std::function<boost::asio::any_io_executor()>;

// Accepts connections on a TCP endpoint, with or without TLS, or on a Unix domain socket, and
// hands each one to a handler: TLS connections once their handshake succeeded.
//
// open() binds and listens, start() accepts, stop() ends it: a listener is used once. It
// accepts on the executor of `accept_on` and serves each connection on the next executor of
// `serve_on`, where its handshake runs and its handler is called (a plain connection is handed
// over on the executor it was accepted on).
//
// stop() may be called from any thread, the handler included, and more than once. Once it
// returns, no handler starts anymore and none runs on another thread: called outside the
// handler, it waits for the handlers running on other threads to return, so a handler must
// never wait for a thread calling stop(). The destructor stops the listener.
class listener {
public:
    listener(listener_options options, executor_provider accept_on, executor_provider serve_on);
    ~listener();

    listener(const listener&) = delete;
    listener& operator=(const listener&) = delete;
    listener(listener&&) = delete;
    listener& operator=(listener&&) = delete;

    // Bind and listen. An error if it cannot: `invalid_argument` for options that cannot be
    // used (an executor not on an io_context, unreadable trusted CAs, a manager without a
    // context, a session cache that differs from the one of another listener of the same
    // manager), `already_open` if it was opened (or stopped) before, or the error resolving
    // the host or binding (after max_listening_attempts). Resolving is synchronous
    boost::system::error_code open();
    // Accept connections, handing each one to `on_connection` (see above). Nothing (logged) if
    // the listener is not open, or started already
    void start(sync_callback<void(std::shared_ptr<socket>)> on_connection);
    // Stop accepting: closes the listening socket and the connections whose handshake is
    // pending (see above)
    void stop();

    // The port listened on: 0 before open(), after stop(), and for Unix domain sockets
    uint16_t local_port() const;
    // Where it listens, once open: "http://0.0.0.0:8080", "https://[::1]:443", "unix:/run/x.sock"
    std::string url() const;

private:
    friend struct detail::listener_access;
    struct state;
    std::shared_ptr<state> state_;
};

} // namespace thinger::asio

#endif // THINGER_ASIO_LISTENER_HPP

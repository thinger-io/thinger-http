#ifndef THINGER_HTTP_SERVER_BASE_HPP
#define THINGER_HTTP_SERVER_BASE_HPP

#include "http_application.hpp"
#include "connection_tracker.hpp"
#include "../common/body_logging.hpp"
#include "../../asio/listener.hpp"
#include "../../util/types.hpp"
#include <boost/system/error_code.hpp>
#include <memory>
#include <string>
#include <functional>
#include <chrono>
#include <vector>
#include <mutex>
#include <optional>

namespace thinger::http {

// How the connections of a server are served (see server_base::set_connection_options())
struct connection_options {
    // A connection idle for this long (no request arriving, nor a response being written, nor
    // the handler of a request received whole running) is closed
    std::chrono::seconds idle_timeout{120};

    // Deadlines of a request, whatever the pace of its bytes (Node's headersTimeout and
    // requestTimeout), from its start: its first byte, or the start of serving the connection
    // for its first request. A head not complete within `header_timeout` (capped by
    // `request_timeout`) is answered 408 Request Timeout and the connection closed, at once;
    // a connection that sent no byte of it is closed without a response. A request whose body
    // was not read to its end within `request_timeout` (which a route, a group or a virtual
    // host may change, see route::request_timeout()) is answered 408, or the connection closed
    // if its response already started; its handler is cancelled (cancel_reason::timeout). Zero
    // disables either
    std::chrono::milliseconds header_timeout{60000};
    std::chrono::milliseconds request_timeout{300000};

    // A response write of which the socket accepts no 64 KiB slice (the whole write, if
    // smaller) for this long closes the connection, as a client that stopped reading, whether
    // it was to be kept alive or not (nginx's send_timeout); `idle_timeout` never cuts a
    // response being written. The client taking bytes from the socket is not checked, as SSE
    // and WebSocket writes do: a slow client with a large send buffer may be cut while it
    // still reads, if the socket accepts no slice for this long. Zero disables it
    std::chrono::milliseconds write_timeout{60000};

    // Lingering close (RFC 9112 section 9.6): a connection closed after a response while the
    // client may still be sending its request (e.g. a body over the limit, answered 413 before
    // it was read) stops sending, then reads and drops what arrives, until the client closes,
    // `linger_max_bytes` were dropped or `linger_timeout` passed, and closes. Closed at once
    // instead, the unread data makes the system reset the connection, and a client still
    // sending may never read the response. Zero for either closes at once (also on sockets that
    // cannot half-close, such as custom ones). A graceful shutdown of the server closes the
    // connections lingering after at most 500 ms
    size_t linger_max_bytes = 8 << 20;
    std::chrono::milliseconds linger_timeout{5000};

    // Log the header values and the start of the bodies of the requests and responses: an
    // opt-in for debugging, off by default (see body_logging)
    body_logging logging;
};

// A server is an application (see http_application: routes, virtual hosts, middlewares and
// request processing settings) served on listeners: TCP, with or without TLS, or Unix sockets.
// It only adds the listeners and the connection settings. Every listener serves the same
// application: a route (or middleware, virtual host, hook) answers on all of them. Add them
// before any request arrives (before listen(), or after it but before a request comes): every
// request reads them without locks, so changing them while serving is a data race. An
// http_application can also be used on its own, e.g. for in-memory dispatch.
class server_base : public http_application {
public:
    server_base() = default;
    // Stops the server as server_base::stop() does (the subclasses in the library call their
    // own stop() first; one whose listeners use something it owns, e.g. an io_context, must
    // call stop() in its destructor too): once it returns, its connections are closing and its
    // handlers cancelled, none waited for, and no code of the library touches the server. A handler still running keeps the routes, hosts,
    // middlewares, hooks and settings it reaches (shared with its connection), so it may
    // finish its current step against them; what it captured itself (the server included) is
    // the application's to keep alive. It may be destroyed from one of its own handlers, on
    // any worker thread: that handler goes on, and must not touch the server afterwards
    ~server_base() override;

    // --- Server configuration ---
    // How its connections are served: copied once, and read by every connection. Set it before
    // listen(): a later call applies to the connections accepted afterwards only, never to
    // those open
    void set_connection_options(connection_options options);

    // --- Server control ---
    // Listen with `options` (a TCP endpoint, with TLS if `options.tls` holds a certificate
    // manager, or a Unix socket; see asio::listener_options), and serve the connections it
    // accepts. May be called several times, each call opening a listener of its own: all serve
    // the application. An error, with the listeners open left as they are, if:
    //  - the listener cannot open (see asio::listener::open());
    //  - the server listens on that endpoint already, or another listen() is opening it
    //    (`address_in_use`; the same host and port, or the same Unix socket path);
    //  - a shutdown is in progress (`shut_down`): it works again once the shutdown ended;
    //  - stop_listening(), stop() or a shutdown stopped the listener while it opened
    //    (`operation_aborted`), e.g. while it waited to bind again.
    // The listener is in listening_on() from the moment it accepts connections, so whatever
    // stops the listeners, on any thread, stops it too.
    boost::system::error_code listen(asio::listener_options options);
    // Listen on plain TCP: the host and the port (0 for one chosen by the system, see
    // listening_on())
    boost::system::error_code listen(std::string host, uint16_t port);

    // Listen on plain TCP and wait() until the server stops: false if it cannot listen.
    // `on_listening` (if any) is called once it listens, before waiting
    bool start(uint16_t port);
    bool start(uint16_t port, const sync_callback<void()> &on_listening);
    bool start(const std::string& host, uint16_t port);
    bool start(const std::string& host, uint16_t port, const sync_callback<void()> &on_listening);

    // The three steps of stopping a server: stop_listening() (quiesce), shutdown() (drain, until
    // a deadline) and stop() (release). Each may be called from any thread, more than once.

    // Stop listening right away, on every listener: new connections are refused. Those open are
    // left as they are: they go on serving, and count in stats(). Later calls do nothing
    void stop_listening();

    // Graceful shutdown, with `by` as its whole budget (nothing waits past it):
    //  1. it stops listening, if that was not done already (see stop_listening());
    //  2. it calls on_shutdown(), tells the connections taken over (WebSockets, event streams,
    //     tunnels...) to finish (see websocket_connection::on_shutdown(),
    //     sse_connection::on_shutdown(), server_scope::on_shutdown()), closes the idle connections, and
    //     lets those handling a request finish it: its response tells the client the connection
    //     closes (Connection: close), and then it closes;
    //  3. at `by`, it closes whatever is still open.
    // Then the server stops (wait() returns). The result is true if every
    // connection finished before `by`; false if some were closed, or stop() ended the shutdown
    // first. The handlers still running at `by` are not waited for: they go on on their threads.
    //
    // It may be awaited from any executor, on a thread of the server too (a handler, a worker of
    // a pool_server), which it does not block. Every listener drains at once, under the same
    // `by`. Several calls, also concurrent ones, share the same end and result: the first one
    // sets `by`. From an asio::signal_set handler too, but not from a raw signal handler: it is
    // not async-signal-safe (it locks and allocates). A server that is not running (wait() not
    // called, or stopped) ends it at once: true if no connection is open.
    awaitable<bool> shutdown(std::chrono::steady_clock::time_point by);
    // The same, with `by` `grace` from now, blocking until it ends: for a thread that does not
    // serve the server (e.g. main()). On one that does (a handler, a worker of a pool_server) it
    // would block the drain it waits for, so it does not wait there: it logs an error, starts
    // the shutdown, and returns false at once (co_await shutdown(by) there instead)
    bool shutdown(std::chrono::milliseconds grace = std::chrono::seconds(30));

    // Stop the server right away: stop listening, end a shutdown in progress (its result is
    // false), close the connections still open (idle keep-alive, handling a request, or taken
    // over: WebSockets, event streams, tunnels...), and release wait(). The handlers still
    // running are cancelled with cancel_reason::shutdown, whatever their route declared (their
    // next cancellable await ends with operation_aborted), and are not waited for: each
    // connection is closed on its own thread, after its return, and what a handler sends
    // afterwards is dropped. A handler still running keeps what the server serves with (routes,
    // hosts, middlewares, hooks, settings), so it may finish its current step against it; what
    // it captured itself is the application's (see ~server_base()). It does not block, so it may
    // be called from one of the server's handlers, on any thread. The connections it closed are
    // never served again, even if the server listens again (a server with its own io_context
    // stops running it too).
    //
    // The end of a shutdown does not call it: it may end on a thread of the server while the
    // server is being destroyed, so it stops the server without a virtual call (see
    // set_shutdown_stop()). An override (in a subclass of pool_server or server) runs only when
    // stop() is called, never at the end of a shutdown: what it releases after a shutdown, it
    // releases once shutdown() or wait() returned, or in its destructor
    virtual void stop();

    // Called once when a graceful shutdown starts, on the thread starting it, once the server
    // stopped listening: for the application to finish what it runs on connections taken over
    // (or anything else). What it throws is logged, and the shutdown goes on
    void on_shutdown(sync_callback<void()> callback);

    // Connections open by what they serve, and served since the server was created, over all
    // its listeners. It locks each list the connections are kept in (by io_context): meant to be
    // read now and then (e.g. by a monitoring endpoint), as counting costs nothing per request
    server_stats stats() const;

    // Whether it listens on any listener
    bool is_listening() const;

    // Where it listens, one entry per listener open, in the order they opened:
    // "http://0.0.0.0:8080", "https://[::1]:8443" or "unix:/run/x.sock", with the port the
    // system chose for a port 0. Empty while it does not listen
    std::vector<std::string> listening_on() const;

    // Wait until the server stops
    virtual void wait() = 0;

protected:
    // The listener of `options`, on the executors of the server: not opened yet
    virtual std::unique_ptr<asio::listener> create_listener(asio::listener_options options) = 0;

    // A listener opened and started: the server serves (e.g. its io_context may run)
    virtual void listening() {}

    // Where a shutdown waits for the connections: an io_context running while the server
    // runs, or none if nothing runs the connections
    virtual boost::asio::io_context* shutdown_context() = 0;
    // Whether the calling thread serves connections of the server (shutdown(grace) cannot block
    // there)
    virtual bool in_server_thread() = 0;

    // A shutdown in progress (if any) ends: stop() calls it first. Waits for one ending on
    // another thread
    void end_shutdown();

    // What the end of a shutdown runs, once it stopped listening, to stop the server as stop()
    // does (e.g. releasing wait()), instead of calling stop(): on the thread it ends on, where
    // a subclass may be in its destructor already. pool_server and server set it in their
    // constructors, and call stop() in their destructors, which waits for a shutdown ending on
    // another thread (so it never runs once their members are gone). None: the end of a
    // shutdown only stops listening
    void set_shutdown_stop(sync_callback<void()> stop);

private:
    struct shutdown_state;
    // A listener of listen(), there from before it opens, so that stopping the listeners stops
    // it too, and another listen() on its endpoint is refused
    struct open_listener {
        std::shared_ptr<asio::listener> listener;   // (opened by listen() out of the lock)
        decltype(asio::listener_options::endpoint) endpoint;   // as asked for
        uint16_t port = 0;   // the TCP port asked for, then the one it listens on
        std::string url;     // once it listens (not listed before)
    };

    void serve(std::shared_ptr<asio::socket> socket);
    // The server, for the logs: "server on http://0.0.0.0:8080" ("server" while not listening)
    std::string log_name() const;
    // The shutdown in progress, or the one that ended (until the server listens again); else
    // one started, until `by`
    std::shared_ptr<shutdown_state> start_shutdown(std::chrono::steady_clock::time_point by);
    // With shutdown_mutex_ held: false while a shutdown is in progress; else the server serves
    // (again, after a shutdown that ended)
    bool reset_shutdown();

    // The connections served, for the statistics and the shutdown
    std::shared_ptr<connection_tracker> connections_ = std::make_shared<connection_tracker>();

    // The listeners open and opening, and the connection options (guarded by listeners_mutex_:
    // listen() and stop() may be called from any thread, and connections are accepted on the
    // server's). Taken after shutdown_mutex_ when both are
    mutable std::mutex listeners_mutex_;
    std::vector<open_listener> listeners_;
    std::shared_ptr<const connection_options> options_ = std::make_shared<const connection_options>();

    sync_callback<void()> on_shutdown_;
    // Set once, when the server is constructed (see set_shutdown_stop())
    sync_callback<void()> shutdown_stop_;
    // The last shutdown (guarded by its mutex)
    mutable std::mutex shutdown_mutex_;
    std::shared_ptr<shutdown_state> shutdown_;
};

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_BASE_HPP

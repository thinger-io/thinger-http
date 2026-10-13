#ifndef THINGER_HTTP_SERVER_CONNECTION_TRACKER_HPP
#define THINGER_HTTP_SERVER_CONNECTION_TRACKER_HPP

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <boost/asio/io_context.hpp>
#include "../../asio/sockets/socket.hpp"
#include "../../util/types.hpp"

namespace thinger::http {

// What a connection of a server serves
enum class connection_type : uint8_t {
    http,        // HTTP requests (keep-alive connections waiting for the next one included)
    websocket,   // a WebSocket (upgrade_websocket(), upgrade_websocket_stream())
    sse,         // a Server-Sent Events stream (start_sse())
    tunnel,      // a reverse proxy upgrade (e.g. a WebSocket proxied to its target)
    taken_over   // any other connection taken over (response::take_over())
};

// Connections of a server, as of the moment they are read
struct server_stats {
    // Open connections, by what they serve
    uint64_t http_connections = 0;
    uint64_t websocket_connections = 0;
    uint64_t sse_connections = 0;
    uint64_t tunnel_connections = 0;
    uint64_t taken_over_connections = 0;
    // Connections served since the server was created (as HTTP connections, the way every
    // connection starts)
    uint64_t total_connections = 0;

    uint64_t active_connections() const {
        return http_connections + websocket_connections + sse_connections + tunnel_connections +
               taken_over_connections;
    }
};

class connection_tracker;

// A connection of a server, known to its tracker while open (see track()). Nothing is done per
// request: the tracker only takes a lock when the connection joins or leaves it, or changes
// what it serves, and on the shard of its io_context (statistics and shutdown are rare, and
// read the shards instead).
class tracked_connection {
public:
    tracked_connection(const tracked_connection&) = delete;
    tracked_connection& operator=(const tracked_connection&) = delete;

protected:
    tracked_connection() = default;
    // Leaves the tracker. Derived classes are gone by then, which is fine: the tracker reaches
    // them only through `self` (expired by then), and reads nothing else but this base
    ~tracked_connection();

    // Join the tracker of a server. `self` owns this object (the tracker locks it to act on
    // the connection), which runs on `io_context`. From any thread, once
    void track(std::shared_ptr<connection_tracker> tracker, std::weak_ptr<void> self,
               boost::asio::io_context& io_context, connection_type type);
    // Leave the tracker (the connection is closed, or belongs to someone else now). From the
    // thread of the connection, and again from the destructor
    void untrack();
    void set_type(connection_type type);

    const std::shared_ptr<connection_tracker>& tracker() const { return tracker_; }
    bool tracked() const { return tracker_ != nullptr; }

    // On the io_context of the connection, while the server shuts down: finish what the
    // connection is doing and close (or tell its owner to), or close it right away
    virtual void shutdown_drain() = 0;
    virtual void shutdown_close() = 0;

private:
    friend class connection_tracker;

    std::shared_ptr<connection_tracker> tracker_;
    std::weak_ptr<void> self_;
    boost::asio::io_context* io_context_ = nullptr;
    // In the list of its shard (guarded by the mutex of the shard)
    tracked_connection* prev_ = nullptr;
    tracked_connection* next_ = nullptr;
    bool linked_ = false;
    uint8_t shard_ = 0;
    connection_type type_ = connection_type::http;
};

// The connections of a server: for its statistics, and to drain and close them on shutdown.
// Thread-safe.
class connection_tracker {
public:
    enum class phase : uint8_t {
        serving,
        draining,  // connections finish what they do, and close
        closing    // connections are closed right away
    };

    phase current_phase() const { return phase_.load(std::memory_order_acquire); }
    void set_phase(phase value) { phase_.store(value, std::memory_order_release); }

    server_stats stats() const;
    // Connections open
    size_t active() const;

    // Call shutdown_drain() on every connection, on its io_context (does not wait)
    void drain_all();
    // Call shutdown_close() on every connection, on its io_context; then `done` (on the thread
    // of the last one, or right away if there is none)
    void close_all(std::function<void()> done);

private:
    friend class tracked_connection;
    friend class taken_over_connection;

    static constexpr size_t shard_count = 16;

    struct alignas(64) shard {
        mutable std::mutex mutex;
        tracked_connection* head = nullptr;
        std::array<uint64_t, 5> active{};
        uint64_t total_connections = 0;
    };

    static size_t shard_of(const boost::asio::io_context& io_context);

    void add(tracked_connection& connection);
    void remove(tracked_connection& connection);
    void retype(tracked_connection& connection, connection_type type);

    // The connections open (owned while they are acted on)
    std::vector<std::pair<std::shared_ptr<void>, tracked_connection*>> snapshot() const;

    std::array<shard, shard_count> shards_;
    std::atomic<phase> phase_{phase::serving};
};

// A connection taken over from the server (a WebSocket, an event stream, a tunnel...), tracked
// while its socket lives (it is attached to it), or until untracked by its new owner once
// closed. On shutdown it calls the callback set with on_server_shutdown() (if any), and once
// the grace period ends its socket is closed.
class taken_over_connection : public tracked_connection,
                              public asio::socket_attachment,
                              public std::enable_shared_from_this<taken_over_connection> {
public:
    // Track `socket`, taken over from a connection of `tracker`, and attach it there
    static std::shared_ptr<taken_over_connection> track(std::shared_ptr<connection_tracker> tracker,
                                                        const std::shared_ptr<asio::socket>& socket);
    // The tracking attached to `socket`, if any
    static std::shared_ptr<taken_over_connection> of(const asio::socket& socket);

    ~taken_over_connection() override;

    // Once the handler took the socket: drained or closed if the server shuts down meanwhile
    void handed_over();

    // On the thread of the socket
    void set_type(connection_type type) { tracked_connection::set_type(type); }
    void on_shutdown(sync_callback<void()> callback) { on_shutdown_ = std::move(callback); }
    // The new owner closed the connection: it no longer counts
    void closed() {
        on_shutdown_ = nullptr;
        untrack();
    }

private:
    taken_over_connection() = default;

    void shutdown_drain() override;
    void shutdown_close() override;

    std::weak_ptr<asio::socket> socket_;
    sync_callback<void()> on_shutdown_;
    bool notified_ = false;
};

// What the connection of `socket`, taken over from a server, now serves (nothing happens if the
// server does not track it). On the thread of the socket
void set_connection_type(const asio::socket& socket, connection_type type);

// `wrapper` (e.g. a WebSocket) now runs on the connection of `socket`, taken over from a server:
// tracked as `type`, and reachable from `wrapper` too (see on_server_shutdown()). On the thread
// of the socket
void wrap_connection(const asio::socket& socket, asio::socket& wrapper, connection_type type);

// Ask a connection taken over from the server (the socket of a take_over() handler, or the
// WebSocket of upgrade_websocket_stream()) to be told when the server shuts down gracefully:
// `callback` is called once, on the io_context of the socket, so its owner can finish and
// close it (e.g. with a WebSocket close 1001 Going Away). Whatever is still open at the end of
// the grace period is closed. Nothing happens for a socket that does not come from a server.
// On the thread of the socket.
void on_server_shutdown(asio::socket& socket, sync_callback<void()> callback);

}

#endif

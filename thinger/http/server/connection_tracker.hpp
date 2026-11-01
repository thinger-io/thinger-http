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
#include "upgrade.hpp"

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

    // The tracker, while tracked (it is held weakly: it is not kept alive by its connections)
    std::shared_ptr<connection_tracker> tracker() const { return tracker_.lock(); }
    bool tracked() const { return !tracker_.expired(); }

    // On the io_context of the connection, while the server shuts down: finish what the
    // connection is doing and close (or tell its owner to), or close it right away
    virtual void shutdown_drain() = 0;
    virtual void shutdown_close() = 0;
    // On the io_context of the connection, at the deadline of the shutdown, before it is
    // closed: cancel the requests whose handlers still run (nothing by default)
    virtual void shutdown_cancel() {}

private:
    friend class connection_tracker;

    std::weak_ptr<connection_tracker> tracker_;
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

    // Call `empty` once no connection is left: right away, on this thread, if none is; else
    // on the thread of the last one to leave, as it leaves. Once, replacing the one set before
    // (none with null). For a shutdown waiting for its connections: a connection leaving does
    // nothing more while none is set
    void notify_when_empty(std::function<void()> empty);

    // Call shutdown_drain() on every connection, on its io_context (does not wait)
    void drain_all();
    // Call shutdown_cancel() and then shutdown_close() on every connection, in one call on its
    // io_context, so the handlers interrupted resume on a closed connection; then `done` (on
    // the thread of the last one, or right away if there is none), once what was posted on
    // each io_context before its close ran too (the first resumption of those handlers)
    void cancel_and_close_all(std::function<void()> done);

private:
    friend class tracked_connection;

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
    // Call the callback of notify_when_empty() if no connection is left
    void check_empty();

    // The connections open (owned while they are acted on)
    std::vector<std::pair<std::shared_ptr<void>, tracked_connection*>> snapshot() const;

    std::array<shard, shard_count> shards_;
    std::atomic<phase> phase_{phase::serving};
    // What notify_when_empty() set, and whether it did (read by every connection leaving)
    std::mutex empty_mutex_;
    std::function<void()> on_empty_;
    std::atomic<bool> watching_{false};
};

namespace detail {

// A connection taken over from the server (a WebSocket, an event stream, a tunnel...). It owns
// the socket, and the socket handed out (see hand_out()) owns it: tracked while that socket
// lives (the handles copied from it, and a WebSocket built on it, included), or until its
// owner closes it (server_scope::closed()). On shutdown it calls the callback set with
// server_scope::on_shutdown() (if any), and once the grace period ends its socket is closed.
// On the thread of the socket.
class taken_over_tracking final : public tracked_connection,
                                  public std::enable_shared_from_this<taken_over_tracking> {
public:
    // Track `socket`, taken over from a connection of `tracker`
    static std::shared_ptr<taken_over_tracking> track(std::shared_ptr<connection_tracker> tracker,
                                                      std::shared_ptr<asio::socket> socket);

    ~taken_over_tracking();

    // The connection to hand to the new owner: the socket (owning this tracking) and its scope
    upgraded_connection hand_out(std::string buffered);

    // Once the handler took the socket: drained or closed if the server shuts down meanwhile
    void handed_over();

    // What server_scope does
    void set_type(connection_type type) { tracked_connection::set_type(type); }
    void on_shutdown(sync_callback<void()> callback);
    void closed();

private:
    explicit taken_over_tracking(std::shared_ptr<asio::socket> socket) : socket_(std::move(socket)) {}

    void shutdown_drain() override;
    void shutdown_close() override;
    // Call the callback of on_shutdown(), logging what it throws
    static void notify(const sync_callback<void()>& callback);

    std::shared_ptr<asio::socket> socket_;
    sync_callback<void()> on_shutdown_;
    // The drain started (shutdown_drain()), and the owner was told (or will be, posted)
    bool draining_ = false;
    bool notified_ = false;
};

}

}

#endif

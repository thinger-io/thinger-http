#ifndef THINGER_HTTP_SERVER_UPGRADE_HPP
#define THINGER_HTTP_SERVER_UPGRADE_HPP

// What a connection upgraded or taken over from the server is handed with (see
// response::upgrade_websocket(), upgrade_websocket_stream(), start_sse() and take_over()), and
// the options of those upgrades. Light: only declarations of the connection types, so that
// response.hpp does not bring them in.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "../../util/types.hpp"

namespace thinger::asio {
class socket;
class websocket;
}

namespace thinger::http {

// What a connection of a server serves (see connection_tracker.hpp)
enum class connection_type : uint8_t;

class websocket_connection;
class sse_connection;

namespace detail {
// The server's tracking of a connection taken over (connection_tracker.hpp), not public
class taken_over_tracking;
}

/**
 * The link between a connection taken over (or upgraded) and the server it came from: its
 * statistics and its shutdown. A handle: copies share it. It does not keep the server alive;
 * with no server (a socket that does not come from one, a request dispatched in memory, a
 * default-constructed scope), or once the server is gone, its methods do nothing.
 *
 * The server counts the connection while the socket handed out (or a WebSocket built on it)
 * lives, or until closed() is called; whatever is still open at the end of a shutdown's grace
 * period is closed. On the thread of the socket.
 */
class server_scope {
public:
    server_scope() = default;

    // What the connection serves now, in the server statistics
    void set_type(connection_type type) const;

    // Be told when the server shuts down gracefully: `callback` is called once, on the
    // io_context of the socket, when the drain starts, so the owner can finish and close the
    // connection (e.g. with a WebSocket close 1001 Going Away). Set after the drain started, it
    // is still called, posted. Released before it is called: it may own the socket. What it
    // throws is logged
    void on_shutdown(sync_callback<void()> callback) const;

    // The new owner closed the connection: it no longer counts, and is not told of a shutdown.
    // Once; calling it again does nothing
    void closed() const;

private:
    friend class detail::taken_over_tracking;
    explicit server_scope(std::weak_ptr<detail::taken_over_tracking> tracking)
        : tracking_(std::move(tracking)) {}

    std::weak_ptr<detail::taken_over_tracking> tracking_;
};

// A connection taken over from the server (see response::take_over()): its socket, any bytes
// the server had already read past the request that took it over, and its scope
struct upgraded_connection {
    std::shared_ptr<asio::socket> socket;
    std::string buffered;
    server_scope scope;
};

using takeover_handler = sync_callback<void(upgraded_connection)>;
// The same, as a coroutine (run on the io_context of the socket)
using takeover_coroutine = std::function<awaitable<void>(upgraded_connection)>;

// Options of a WebSocket upgrade, with a message interface (response::upgrade_websocket()) or
// as a byte stream (response::upgrade_websocket_stream())
struct websocket_accept_options {
    // Subprotocols the handler speaks, alternatives included (e.g. "mqtt", "mqttv3.1",
    // "mqttv3.1.1"). The first one the client offers (Sec-WebSocket-Protocol, in its order of
    // preference) that is in this list is accepted. With the message interface, a client
    // offering subprotocols none of which is here is rejected with 400 (also with an empty
    // list); as a byte stream, it is accepted without one, unless protocol_required
    std::vector<std::string> protocols;
    // With protocols: a client offering none of them is rejected with 400. Otherwise it is
    // accepted without a subprotocol (and the handler gets an empty one)
    bool protocol_required = true;
    // As a byte stream: messages written binary, or text (the message interface writes each
    // message as it is sent)
    bool binary = true;
    // With the message interface: the largest message received (at least 1 byte); a larger one
    // closes with 1009 (Message Too Big). A byte stream has no messages, nor this limit
    size_t max_message_size = size_t{16} * 1024 * 1024;
    // Ping the client once nothing was received from it for this long, and close the connection
    // once nothing was received for idle_timeout (pongs included); 0 turns each off. Unset, the
    // default of the upgrade: 60 s and 120 s with the message interface, off as a byte stream
    std::optional<std::chrono::milliseconds> ping_interval;
    std::optional<std::chrono::milliseconds> idle_timeout;
};

// A WebSocket upgraded as a byte stream (see response::upgrade_websocket_stream()): the
// WebSocket (holding any frames the client sent right after its request), the subprotocol
// negotiated (empty if none), and its scope
struct upgraded_websocket {
    std::shared_ptr<asio::websocket> stream;
    std::string protocol;
    server_scope scope;
};

using websocket_stream_handler = sync_callback<void(upgraded_websocket)>;
using websocket_stream_coroutine = std::function<awaitable<void>(upgraded_websocket)>;

// A WebSocket with a message interface, and an event stream
using websocket_handler = sync_callback<void(std::shared_ptr<websocket_connection>)>;
using websocket_coroutine = std::function<awaitable<void>(std::shared_ptr<websocket_connection>)>;
using sse_handler = sync_callback<void(std::shared_ptr<sse_connection>)>;
using sse_coroutine = std::function<awaitable<void>(std::shared_ptr<sse_connection>)>;

// Options of an event stream (response::start_sse())
struct sse_options {
    // Interval of the keep-alive comments written while nothing else is (0: none); see
    // sse_connection::set_keepalive()
    std::chrono::milliseconds keepalive{15000};
};

}

#endif

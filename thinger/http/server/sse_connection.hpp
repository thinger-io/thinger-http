#ifndef SSE_CONNECTION_HPP
#define SSE_CONNECTION_HPP

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <boost/asio/any_io_executor.hpp>
#include "../common/http_request.hpp"
#include "../common/http_response.hpp"
#include "../../asio/sockets/socket.hpp"
#include "../../asio/watchdog.hpp"
#include "../data/out_string.hpp"
#include "../../util/types.hpp"
#include "upgrade.hpp"
#include "flow_control.hpp"
#include "close_notifier.hpp"

namespace thinger::http{

// The queue of the connection (detail/push_queue.hpp), only declared here: not public
namespace detail {
    template<typename Item>
    class push_queue;
    enum class push_failure;
}

/**
 * An event sent whole (see sse_connection::send()): its fields present, in this order, then
 * its data, each line a data field of its own, and the empty line that dispatches it. The same
 * on the wire as send_event(), send_id(), send_retry() and send_data(), and refused as they
 * are: an event type or ID with a CR or LF, an ID with NUL, or a negative retry
 */
struct sse_event {
    std::optional<std::string> event;
    std::optional<std::string> id;
    std::optional<std::chrono::milliseconds> retry;
    std::string data;
};

/**
 * A Server-Sent Events stream. What is sent is queued, with the flow control of
 * websocket_connection (see write_limits), counted in what was sent: each field (send_event(),
 * send_id(), send_retry(), send_data()), each sse_event and each comment counts one.
 * drop_oldest drops whole events, never one being written nor one sent with send_async();
 * close closes, and EventSource reconnects.
 *
 * The connection reads what the client sends, only to learn when it leaves (it sends nothing
 * after its request; anything it sends is discarded): on_close() tells it.
 *
 * Threads. It runs on its executor (see get_executor()), which may be a strand. From any
 * thread: start(), send*(), stop(), execute(), set_keepalive(), wait_writable(),
 * set_write_limits(), on_high_watermark(), on_low_watermark(), queued_bytes() and
 * queued_messages(). On its executor only (e.g. from execute()): get_write_limits().
 * on_shutdown() and on_close() are set in the handler that accepts it.
 */
class sse_connection : public std::enable_shared_from_this<sse_connection>, public boost::noncopyable{

public:

    /// Default limit of the queue, in what is sent (max_messages is one more)
    static const int MAX_OUTPUT_MESSAGES = 100;

    /// Default interval of the keep-alive comments (see set_keepalive())
    static constexpr std::chrono::milliseconds DEFAULT_KEEPALIVE_INTERVAL{15000};

    /// On `executor` (e.g. a strand), or by default on the io_context of the socket; taken over
    /// from a server through `scope`, if any: told when the server shuts down (see
    /// on_shutdown()), counted as an event stream until closed
    sse_connection(std::shared_ptr<asio::socket> socket, boost::asio::any_io_executor executor = {},
                   server_scope scope = {});

    virtual ~sse_connection();

    /// The executor of the connection: where it runs, and its callbacks
    const boost::asio::any_io_executor& get_executor() const { return executor_; }

    /// Run `callback` on the executor of the connection: at once if already there
    void execute(sync_callback<void()> callback);

    /// Start reading, to learn when the client leaves, and the keep-alive
    void start();

    void stop();

    /// What to do when the server shuts down gracefully (see server_base::shutdown()),
    /// e.g. send a last event and stop(). By default it closes (clients reconnect); whatever
    /// is open at the end of the grace period is closed anyway
    void on_shutdown(sync_callback<void()> callback){
        on_shutdown_callback_ = std::move(callback);
    }

    /// Once the connection closed, whatever closed it: the client left (or the connection
    /// failed), stop(), the server shutdown, the overflow policy or a write timeout (a quiet
    /// stream is not closed, see set_keepalive()). Called once; nothing can be sent anymore.
    /// Always posted to the executor, never called from within the call closing it (e.g. a
    /// send from a loop over the connections, which may then remove this one); what it
    /// throws is logged. Set once the connection already closed (e.g. by a coroutine handler
    /// that ran late), it is posted at once. Not called if the io_context is stopped and
    /// destroyed with it open
    void on_close(sync_callback<void()> callback){
        on_close_.set(std::move(callback));
    }

    /// Send an event whole, without waiting: queued, or handled by the overflow policy.
    /// Refused (false, an error logged) as its fields are (see sse_event)
    bool send(const sse_event& event);

    /// Send an event and wait until written: true, or false if refused, or if the connection
    /// closed or failed first. Never dropped by the overflow policy; a cancelled wait leaves
    /// it queued
    awaitable<bool> send_async(sse_event event);

    /// The reconnection time of the client, in milliseconds
    void send_retry(unsigned long millis);

    /// The type of the event whose data follows. Refused (false, an error logged) if it holds
    /// a CR or LF, which would end the field and inject others
    bool send_event(const std::string& event_name);

    /// The last event ID of the event whose data follows. Refused (false, an error logged) if
    /// it holds a CR, LF or NUL (a browser ignores an ID with NUL)
    bool send_id(const std::string& id);

    /// The data of an event, which ends it: each line (split on CRLF, CR or LF) is sent as a
    /// data field of its own, so the client gets the value as it was sent, line breaks as LF
    /// (WHATWG HTML, Server-sent events)
    void send_data(const std::string& data);

    /// A comment (": comment", a line the client ignores), sent with the event that follows:
    /// drop_oldest drops them together. Refused (false, an error logged) if it holds a CR or LF
    bool send_comment(std::string_view comment);

    /// Send a comment (": keep-alive", a line the client ignores) once `interval` passed
    /// without anything else written, so proxies and clients do not take a quiet stream as
    /// dead (DEFAULT_KEEPALIVE_INTERVAL, 15 seconds, by default); 0 disables it. Never queued
    /// nor counted. A client gone is noticed by reading the connection, whatever this is set to
    void set_keepalive(std::chrono::milliseconds interval);

    void set_write_limits(const write_limits& limits);
    write_limits get_write_limits() const;

    /// Bytes, and what was sent (see the class), not written yet (the one being written included)
    size_t queued_bytes() const;
    size_t queued_messages() const;

    /// The watermarks crossed (see write_limits); not told once the connection is closed
    void on_high_watermark(sync_callback<void()> callback);
    void on_low_watermark(sync_callback<void()> callback);

    /// Wait while above the high watermark, until drained to the low one: true, or false if
    /// the connection closed
    awaitable<bool> wait_writable();

private:

    /// What the queue writes: one field, an event, or a comment, as sent
    struct out_field {
        std::string data;
        /// An event, or its data field: it ends the event (drop_oldest drops whole events)
        bool ends_event = false;
        /// A keep-alive comment: written when nothing is queued, neither counted nor dropped
        bool keepalive = false;
    };

    /// Send a field: `data` as send_data(), any other type with a value without CR or LF.
    /// False if refused (a value that would break the framing), an error logged
    bool handle_write(std::string_view type, std::string_view value);

    /// Queue what was sent, on the executor, or apply the overflow policy
    void push(out_field field);

    /// Write a field, moving the keep-alive later (see push_queue)
    awaitable<boost::system::error_code> write_field(out_field& field);

    /// The queue failed (overflow, write timeout or error): closed
    void queue_failed(detail::push_failure failure, boost::system::error_code ec);

    /// The keep-alive expires `keepalive_` after the last write (nothing if 0)
    void arm_keepalive();
    /// It expired: a keep-alive comment if nothing is being written nor queued
    void keepalive_expired();

    /// Close the connection (on its executor), once
    void close();

    /// Read until the client leaves (`self` keeps the connection alive)
    awaitable<void> read_loop(std::shared_ptr<sse_connection> self);

    /// Taken over from a server: told when it shuts down
    void track_shutdown();

    std::shared_ptr<asio::socket> socket_;

    boost::asio::any_io_executor executor_;

    /// What was sent and is not written yet, against the write limits, and its writer
    std::unique_ptr<detail::push_queue<out_field>> queue_;

    /// Keep-alive comments: once keepalive_ passed without anything else written
    asio::watchdog keepalive_timer_;
    std::chrono::milliseconds keepalive_ = DEFAULT_KEEPALIVE_INTERVAL;

    sync_callback<void()> on_shutdown_callback_;
    close_notifier<> on_close_;

    bool closed_ = false;

    /// The server this connection was taken over from, if any
    server_scope scope_;
};

}

#endif

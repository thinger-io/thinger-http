#ifndef SSE_CONNECTION_HPP
#define SSE_CONNECTION_HPP

#include <chrono>
#include <deque>
#include <memory>
#include "../common/http_request.hpp"
#include "../common/http_response.hpp"
#include "../../asio/sockets/socket.hpp"
#include "../data/out_string.hpp"
#include "../../util/logger.hpp"
#include "../../util/types.hpp"
#include "connection_tracker.hpp"
#include "flow_control.hpp"
#include "close_notifier.hpp"

namespace thinger::http{

/**
 * A Server-Sent Events stream. Each field sent (send_event(), send_data(), send_retry()) is
 * queued without waiting, with the flow control of websocket_connection (see write_limits),
 * counted in fields. drop_oldest drops whole events; close closes, and EventSource reconnects.
 *
 * The connection reads what the client sends, only to learn when it leaves (it sends nothing
 * after its request; anything it sends is discarded): on_close() tells it.
 *
 * Threads. From any thread: send_*(), stop(), wait_writable(), set_write_limits(),
 * on_high_watermark(), on_low_watermark(), queued_bytes() and queued_messages(). On the
 * connection thread only: get_write_limits(). on_shutdown() and on_close() are set in the
 * handler that accepts it.
 */
class sse_connection : public std::enable_shared_from_this<sse_connection>, public boost::noncopyable{

public:

    /// Default limit of the queue, in fields (max_messages is one more)
    static const int MAX_OUTPUT_MESSAGES = 100;

    sse_connection(std::shared_ptr<asio::socket> socket) :
            socket_(socket),
            timer_(socket->get_io_context()),
            flow_(socket->get_io_context().get_executor(), write_limits{.max_messages = MAX_OUTPUT_MESSAGES + 1}),
            on_close_(socket->get_io_context().get_executor())
    {
        LOG_DEBUG("created sse connection");
    }

    virtual ~sse_connection()
    {
        LOG_DEBUG("releasing sse connection");
    }

    /// Default interval of the keep-alive comments (see set_keepalive())
    static constexpr std::chrono::milliseconds DEFAULT_KEEPALIVE_INTERVAL{15000};

private:

    /// Arm the keep-alive timer for the interval set (nothing if 0)
    void arm_keepalive();

    struct out_field {
        /// The field as sent: "name: value\n", and the empty line ending an event after data
        std::string data;
        /// A data field, which ends its event
        bool ends_event = false;
        /// A keep-alive comment: never queued, outside the events and the write limits
        bool comment = false;
    };

    void process_out_queue();

    /// Write the fields queued, one at a time (`self` keeps the connection alive)
    awaitable<void> write_loop(std::shared_ptr<sse_connection> self);

    /// Queue a field, or apply the overflow policy
    void push_field(out_field field);

    /// Drop the oldest whole events, not the one being written, until a field of `size` bytes
    /// fits: false if it does not fit even so
    bool drop_oldest(size_t size);

    /// Past the end of the event `it` is in: its data field (or the end of the queue)
    std::deque<out_field>::iterator next_event(std::deque<out_field>::iterator it);

    /// Close the connection (on its thread), once
    void close();

    /// Read until the client leaves (`self` keeps the connection alive)
    awaitable<void> read_loop(std::shared_ptr<sse_connection> self);

    /// Taken over from a server: told when it shuts down
    void track_shutdown();

public:

    void start(){
        track_shutdown();
        arm_keepalive();
        co_spawn(socket_->get_io_context(), read_loop(shared_from_this()), detached);
    }

    void stop(){
        boost::asio::dispatch(socket_->get_io_context(), [this, self = shared_from_this()](){
            close();
        });
    }

    /// What to do when the server shuts down gracefully (see http_server_base::shutdown()),
    /// e.g. send a last event and stop(). By default it closes (clients reconnect); whatever
    /// is open at the end of the grace period is closed anyway
    void on_shutdown(sync_callback<void()> callback){
        on_shutdown_callback_ = std::move(callback);
    }

    /// Once the connection closed, whatever closed it: the client left (or the connection
    /// failed), stop(), the server shutdown, the overflow policy or a write timeout (a quiet
    /// stream is not closed, see set_keepalive()). Called once; nothing can be sent anymore.
    /// Always posted to the connection thread, never called from within the call closing it
    /// (e.g. a send from a loop over the connections, which may then remove this one); what it
    /// throws is logged. Set once the connection already closed (e.g. by a coroutine handler
    /// that ran late), it is posted at once. Not called if the io_context is stopped and
    /// destroyed with it open
    void on_close(sync_callback<void()> callback){
        on_close_.set(std::move(callback));
    }

    void send_retry(unsigned long millis){
        handle_write("retry", boost::lexical_cast<std::string>(millis));
    }

    /// The type of the event whose data follows. Refused (false, an error logged) if it holds
    /// a CR or LF, which would end the field and inject others
    bool send_event(const std::string& event_name){
        return handle_write("event", event_name);
    }

    /// The last event ID of the event whose data follows. Refused (false, an error logged) if
    /// it holds a CR, LF or NUL (a browser ignores an ID with NUL)
    bool send_id(const std::string& id){
        return handle_write("id", id);
    }

    /// The data of an event, which ends it: each line (split on CRLF, CR or LF) is sent as a
    /// data field of its own, so the client gets the value as it was sent, line breaks as LF
    /// (WHATWG HTML, Server-sent events)
    void send_data(const std::string& data){
        handle_write("data", data);
    }

    /// Send a field: `data` as send_data(), any other type with a value without CR or LF.
    /// False if refused (a type or value that would break the framing), an error logged
    bool handle_write(const std::string& type, const std::string& value);

    /// Send a comment (": keep-alive", a line the client ignores) every `interval` nothing
    /// else was sent, so proxies and clients do not take a quiet stream as dead
    /// (DEFAULT_KEEPALIVE_INTERVAL, 15 seconds, by default); 0 disables it. A client gone is
    /// noticed by reading the connection, whatever this is set to. From any thread
    void set_keepalive(std::chrono::milliseconds interval);

    void set_write_limits(const write_limits& limits);
    write_limits get_write_limits() const { return flow_.limits(); }

    /// Bytes, and fields, not written yet (the one being written included)
    size_t queued_bytes() const { return flow_.queued_bytes(); }
    size_t queued_messages() const { return flow_.queued_messages(); }

    /// The watermarks crossed (see write_limits); not told once the connection is closed
    void on_high_watermark(sync_callback<void()> callback);
    void on_low_watermark(sync_callback<void()> callback);

    /// Wait while above the high watermark, until drained to the low one: true, or false if
    /// the connection closed
    awaitable<bool> wait_writable();

private:
    std::shared_ptr<asio::socket> socket_;

    /// Fields not written yet (the one being written is out of it)
    std::deque<out_field> out_queue_;

    bool writing_ = false;

    /// The last field taken for writing left its event open: the fields up to its data
    /// belong to it
    bool event_open_ = false;

    /// Keep-alive comments: every keepalive_ without anything else written
    boost::asio::steady_timer timer_;
    std::chrono::milliseconds keepalive_ = DEFAULT_KEEPALIVE_INTERVAL;
    uint64_t keepalive_generation_ = 0;
    /// Nothing but comments written since the last tick of the keep-alive timer
    bool idle_ = true;
    /// A keep-alive comment to write, before the fields queued
    bool keepalive_pending_ = false;

    /// Bytes queued against the write limits
    write_flow flow_;

    sync_callback<void()> on_shutdown_callback_;
    close_notifier<> on_close_;

    bool closed_ = false;

    /// The server tracking this connection, if any (see connection_tracker.hpp)
    std::shared_ptr<taken_over_connection> tracking_;
};

}

#endif

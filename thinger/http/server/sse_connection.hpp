#ifndef SSE_CONNECTION_HPP
#define SSE_CONNECTION_HPP

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

namespace thinger::http{

/**
 * A Server-Sent Events stream. Each field sent (send_event(), send_data(), send_retry()) is
 * queued without waiting, and the queue has the flow control of websocket_connection (see
 * flow_control.hpp): limits counted in fields, an overflow policy (drop_newest by default;
 * drop_oldest drops whole events, never one partly written; close closes the connection, and
 * EventSource reconnects), a write timeout, queued_bytes(), watermarks and wait_writable().
 *
 * Threads. From any thread: the send_*() calls, stop(), wait_writable(), set_write_limits(),
 * on_high_watermark(), on_low_watermark(), and the queued_bytes() and queued_messages()
 * getters (relaxed atomics). On the thread of the connection only: get_write_limits();
 * on_shutdown() is set in the handler that accepts it.
 */
class sse_connection : public std::enable_shared_from_this<sse_connection>, public boost::noncopyable{

public:

    /**
     * Parameter for controlling the maximum number of pending messages stored
     * in the output queue: by default, up to one more field is queued (see set_write_limits())
     */
    static const int MAX_OUTPUT_MESSAGES = 100;

    sse_connection(std::shared_ptr<asio::socket> socket) :
            socket_(socket),
            timer_(socket->get_io_context()),
            idle_(false),
            flow_(socket->get_io_context(), write_limits{.max_messages = MAX_OUTPUT_MESSAGES + 1})
    {
        LOG_DEBUG("created sse connection");
    }

    virtual ~sse_connection()
    {
        LOG_DEBUG("releasing sse connection");
    }

private:

    void handle_timeout()
    {
        idle_ = true;
        timer_.expires_after(std::chrono::seconds(60));
        timer_.async_wait(
            [this, self = shared_from_this()](const boost::system::error_code& e){
                // if timer was not cancelled (just expired by itself)
                if(e != boost::asio::error::operation_aborted && !idle_){
                    handle_timeout();
                }else if(idle_){
                    // will terminate any pending async reads or writes
                    close();
                }
            }
        );
    }

    struct out_field {
        /// The field as sent: "name: value\n", and the empty line ending an event after data
        std::string data;
        /// A data field, which ends its event
        bool ends_event = false;
    };

    void process_out_queue();

    // Write the fields queued, one at a time (`self` keeps the connection alive)
    awaitable<void> write_loop(std::shared_ptr<sse_connection> self);

    // Queue a field, applying the overflow policy if the queue is full
    void push_field(out_field field);

    // Drop the oldest whole events queued until a field of `size` bytes fits: false if it does
    // not fit even so
    bool drop_oldest(size_t size);

    // Past the end of the event `it` is in: its data field (or the end of the queue)
    std::deque<out_field>::iterator next_event(std::deque<out_field>::iterator it);

    // Run `callback` on the thread of the connection
    void execute(sync_callback<void()> callback);

    // Close the connection (on its thread)
    void close();

    // Taken over from a server: told when it shuts down
    void track_shutdown();

public:

    void start(){
        track_shutdown();
        handle_timeout();
    }

    void stop(){
        boost::asio::dispatch(socket_->get_io_context(), [this, self = shared_from_this()](){
            close();
        });
    }

    /**
     * Set what to do when the server shuts down gracefully (see http_server_base::shutdown()),
     * on the connection thread: e.g. send a last event and stop(). By default the connection
     * is closed (clients reconnect). Whatever is open at the end of the grace period is closed
     * anyway
     */
    void on_shutdown(sync_callback<void()> callback){
        on_shutdown_callback_ = std::move(callback);
    }

    void send_retry(unsigned long millis){
        handle_write("retry", boost::lexical_cast<std::string>(millis));
    }

    void send_event(const std::string& event_name){
        handle_write("event", event_name);
    }

    void send_data(const std::string& data){
        handle_write("data", data);
    }

    void handle_write(const std::string& type, const std::string& value);

    /**
     * Limits of the queue (in fields), watermarks and overflow policy (see write_limits)
     */
    void set_write_limits(const write_limits& limits);
    write_limits get_write_limits() const { return flow_.limits(); }

    /**
     * Bytes, and fields, queued and not written yet (the one being written included)
     */
    size_t queued_bytes() const { return flow_.queued_bytes(); }
    size_t queued_messages() const { return flow_.queued_messages(); }

    /**
     * Told when the bytes queued reach the high watermark, and when they drain to the low
     * watermark after that: posted to the thread of the connection (never from within a
     * send), and not once it is closed
     */
    void on_high_watermark(sync_callback<void()> callback);
    void on_low_watermark(sync_callback<void()> callback);

    /**
     * Wait while the queue is above its high watermark, until it drains to the low one: true,
     * or false if the connection closed. Right away if it is not congested
     */
    awaitable<bool> wait_writable();

private:
    /// Socket being used HTTP connection
    std::shared_ptr<asio::socket> socket_;

    /// Out queue: fields not written yet (the one being written is out of it)
    std::deque<out_field> out_queue_;

    bool writing_ = false;

    /// The last field taken for writing left its event open: the fields up to its data
    /// belong to it
    bool event_open_ = false;

    /// Timer used for controlling HTTP timeout
    boost::asio::steady_timer timer_;

    bool idle_;

    /// Bytes queued against the write limits
    write_flow flow_;

    /// Server shutdown callback
    sync_callback<void()> on_shutdown_callback_;

    /// The server tracking this connection, if any (see connection_tracker.hpp)
    std::shared_ptr<taken_over_connection> tracking_;
};

}

#endif

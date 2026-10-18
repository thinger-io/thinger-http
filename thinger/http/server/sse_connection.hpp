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
            idle_(false),
            flow_(socket->get_io_context().get_executor(), write_limits{.max_messages = MAX_OUTPUT_MESSAGES + 1})
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
        handle_timeout();
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
    /// failed), stop(), the server shutdown, the overflow policy, a write timeout, or 60 seconds
    /// without sending anything. Called once, on the connection thread; nothing can be sent
    /// anymore. Set once the connection already closed (e.g. by a coroutine handler that ran
    /// late), it is called at once
    void on_close(sync_callback<void()> callback){
        if(closed_){
            if(callback) callback();
            return;
        }
        on_close_callback_ = std::move(callback);
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

    /// Closes the connection when nothing is written for 60 seconds
    boost::asio::steady_timer timer_;

    bool idle_;

    /// Bytes queued against the write limits
    write_flow flow_;

    sync_callback<void()> on_shutdown_callback_;
    sync_callback<void()> on_close_callback_;

    bool closed_ = false;

    /// The server tracking this connection, if any (see connection_tracker.hpp)
    std::shared_ptr<taken_over_connection> tracking_;
};

}

#endif

#ifndef THINGER_WEBSOCKET_CONNECTION_HPP
#define THINGER_WEBSOCKET_CONNECTION_HPP

#include <chrono>
#include <memory>
#include <boost/asio/streambuf.hpp>
#include "../../asio/async_event.hpp"
#include "../../asio/io_context_of.hpp"
#include "../../asio/sockets/websocket.hpp"
#include "../../asio/sockets/socket.hpp"
#include "../data/out_data.hpp"
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

/// How a server WebSocket closed (see websocket_connection::on_close() and
/// asio::websocket_close). The status code (RFC 6455, 7.4) is the one of the peer's close frame
/// (1005 if it had none, 1002 if it was not valid); else the one this end closed with, as 1000
/// (stop()), 1001 (server shutdown, or idle), 1007 or 1009 (a message not valid, or too large),
/// 1011 (on_message() threw) or 1013 (it did not keep up, see write_limits); 1006 if no close
/// frame was exchanged: the connection was lost, or closed by the server at the end of a
/// shutdown's grace period. When this end closed first and the peer answered while the
/// websocket was still reading (stop(), the shutdown), it is the code of the peer's answer, its
/// echo, as Node's ws reports it
using websocket_close = asio::websocket_close;

/**
 * A server WebSocket exchanging whole messages: received with on_message(), sent with
 * send_text()/send_binary() (queued, without waiting) or send_text_async()/send_binary_async()
 * (waiting until written, so a producer goes at the pace of the peer).
 *
 * Flow control: the queue holds 100 messages and drops the newest sent to a full queue, unless
 * set_write_limits() says otherwise (see write_limits in flow_control.hpp). pause_reading()
 * lets TCP backpressure reach the peer.
 *
 * Threads. Callbacks run on the websocket thread. From any thread: send_*(), wait_writable(),
 * set_write_limits(), on_high_watermark(), on_low_watermark(), pause_reading(),
 * resume_reading(), stop(), execute(), queued_bytes() and queued_messages(). On the websocket
 * thread only (e.g. from its callbacks, or execute()): get_write_limits(), queue_full(),
 * is_reading_paused() and release_socket(). on_message() and on_shutdown() are set before
 * start(), in the handler that accepts it; on_close() too, or later on the websocket thread.
 */
class websocket_connection : public std::enable_shared_from_this<websocket_connection>{

public:

    /// Size of a read, and the default largest message received (a larger one closes with
    /// 1009, see websocket_accept_options::max_message_size)
    static constexpr size_t DEFAULT_BUFFER_SIZE = 1024;        // 1KB
    static constexpr size_t MAX_BUFFER_SIZE = 16*1024*1024;   // 16MB

    /// Default max_messages of the queue
    static constexpr int MAX_OUTPUT_MESSAGES = 100;

    /// On `socket`, with the largest message and the keepalive of `options` (its subprotocols
    /// and binary are those of an upgrade, not used here), and taken over from a server through
    /// `scope`, if any: told when the server shuts down (see on_shutdown()), counted as a
    /// websocket until closed
    websocket_connection(std::shared_ptr<asio::websocket> socket, const websocket_accept_options& options = {},
                         server_scope scope = {});

    virtual ~websocket_connection();

    /// Each message received, whole, and whether it is binary
    void on_message(sync_callback<void(std::string, bool binary)> callback);

    /// What to do when the server shuts down gracefully (see server_base::shutdown()),
    /// e.g. send a last message and stop(). By default it closes with 1001 (Going Away);
    /// whatever is open at the end of the grace period is closed anyway
    void on_shutdown(sync_callback<void()> callback);

    /// Once the WebSocket closed, whatever closed it (the peer, an error, stop(), the server
    /// shutdown, the overflow policy...), and how: called once, after its last on_message();
    /// nothing can be sent anymore. Always posted to the websocket thread, never called from
    /// within the call closing it (e.g. a send from a loop over the connections, which may then
    /// remove this one); what it throws is logged. Set once the WebSocket closed, it is posted
    /// at once. Not called once released (release_socket()), nor if the io_context is stopped
    /// and destroyed with it open
    void on_close(sync_callback<void(const websocket_close&)> callback);

    /// The executor of the websocket (its thread, or strand): the one of its socket
    const boost::asio::any_io_executor& get_executor() const { return ws_->get_executor(); }

    /// Run `callback` on the websocket thread
    inline void execute(sync_callback<void()> callback){
        // Already there (e.g. from on_message()): at once, as dispatch() does, without its cost
        // through a type-erased executor on every message
        if(asio::running_in_this_thread(ws_->get_executor())){
            callback();
            return;
        }
        boost::asio::dispatch(ws_->get_executor(), [self = shared_from_this(), callback = std::move(callback)](){
            callback();
        });
    }

    /// Start reading, and the keepalive: the connection lives while it is open
    void start();

    /// Close with 1000 (Normal Closure)
    void stop();

    /// Whether the next message sent without waiting overflows the queue (not the watermarks)
    bool queue_full() const;

    /// Send without waiting: queued, or handled by the overflow policy if the queue is full
    void send_text(std::string text);
    void send_binary(std::string data);

    /// Send and wait until written to the socket: true, or false if the connection closed or
    /// failed first. Never dropped by the overflow policy; a cancelled wait leaves it queued
    awaitable<bool> send_text_async(std::string text);
    awaitable<bool> send_binary_async(std::string data);

    void set_write_limits(const write_limits& limits);
    write_limits get_write_limits() const;

    /// Payload bytes, and messages, not written yet (the one being written included)
    size_t queued_bytes() const;
    size_t queued_messages() const;

    /// The watermarks crossed (see write_limits); not told once the websocket is closing
    void on_high_watermark(sync_callback<void()> callback);
    void on_low_watermark(sync_callback<void()> callback);

    /// Wait while above the high watermark, until drained to the low one: true, or false if
    /// the connection closed
    awaitable<bool> wait_writable();

    /**
     * Stop reading the socket, so the peer's writes block once the TCP buffers fill; writes go
     * on. Messages already read are delivered: no other one if called on the websocket thread
     * (e.g. from on_message()), one more may be from another thread. While paused, the pings
     * and close frame of the peer are not read and the keepalive is suspended, so a dead peer
     * is not noticed: bound the pause, or set a write_timeout for a peer that stops reading
     * too. stop() still closes, discarding the messages before the peer's close frame
     */
    void pause_reading();

    /// Read again, in order and from where reading stopped; the keepalive is restarted
    void resume_reading();

    bool is_reading_paused() const;

    /// Release the socket for another handler: no other call may be made to this instance
    std::shared_ptr<asio::socket> release_socket();

private:

    void start_read_loop();
    awaitable<void> read_loop();

    /// The read loop, closing the websocket if on_message() throws
    awaitable<void> read_messages();

    /// How the websocket closed (see on_close())
    websocket_close close_status() const;

    /// Deliver the message read whole: false if it is not valid
    bool deliver_message();

    /// Wait while reading is paused: false if the websocket closed, or was released
    awaitable<bool> wait_resumed();
    void wake_reader();

    /// The keepalive is suspended while paused
    void suspend_keepalive();
    void restore_keepalive();

    /// Start the closing handshake with `code`, and end the read loop once closed if paused
    void start_close(uint16_t code);

    /// A message to write
    struct out_message {
        std::string data;
        bool binary = false;
    };

    /// Write a message, closing the connection if the queue fails (see push_queue)
    awaitable<boost::system::error_code> write_message(out_message& message);
    void queue_failed(detail::push_failure failure, boost::system::error_code ec);

    /// Queue a message sent without waiting, or apply the overflow policy
    void push_message(std::string data, bool binary);

    /// Close a websocket that does not keep up: 1013 (Try Again Later)
    void close_congested(const char* reason);

    awaitable<bool> send_async(std::string data, bool binary);

    /// The server shuts down (on the websocket thread)
    void server_shutdown();

private:

    std::shared_ptr<asio::websocket> ws_;

    /// Messages not written yet, against the write limits, and their writer
    std::unique_ptr<detail::push_queue<out_message>> queue_;

    /// Incoming data
    boost::asio::streambuf buffer_{MAX_BUFFER_SIZE};

    sync_callback<void(std::string, bool)> on_frame_callback_;
    sync_callback<void()> on_shutdown_callback_;
    close_notifier<websocket_close> on_close_;

    /// The server this connection was taken over from, if any
    server_scope scope_;

    /// Reading paused (see pause_reading()), and the event the read loop waits on meanwhile
    bool paused_ = false;
    asio::async_event resumed_;

    /// Started (start()), and released (release_socket())
    bool started_ = false;
    bool released_ = false;

    /// The connection was lost: a read failed while the websocket was open
    bool lost_ = false;

    /// Keepalive started by start(), and its settings while suspended
    std::chrono::milliseconds ping_interval_;
    std::chrono::milliseconds idle_timeout_;
    std::chrono::milliseconds keepalive_ping_{0};
    std::chrono::milliseconds keepalive_idle_{0};
};

}

#endif

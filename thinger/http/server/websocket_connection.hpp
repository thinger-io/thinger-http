#ifndef THINGER_WEBSOCKET_CONNECTION_HPP
#define THINGER_WEBSOCKET_CONNECTION_HPP

#include <chrono>
#include <deque>
#include <memory>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/streambuf.hpp>
#include "../../asio/sockets/websocket.hpp"
#include "../../asio/sockets/socket.hpp"
#include "../data/out_data.hpp"
#include "../../util/types.hpp"
#include "connection_tracker.hpp"
#include "flow_control.hpp"

namespace thinger::http{

/**
 * A server WebSocket exchanging whole messages: received with on_message(), sent with
 * send_text()/send_binary() (queued, without waiting) or send_text_async()/send_binary_async()
 * (waiting until written).
 *
 * Flow control, for streams between a fast and a slow end (see flow_control.hpp):
 * - Sending: the queue holds up to 100 messages by default, and drops the newest message sent
 *   to a full queue (set_write_limits() changes the limits, the overflow policy and the write
 *   timeout). queued_bytes() tells what is not written yet; the watermarks notify
 *   on_high_watermark() and on_low_watermark(), and wait_writable() waits for the queue to
 *   drain. A producer awaiting send_*_async() has one message queued at a time, and goes at
 *   the pace of the peer.
 * - Receiving: pause_reading() stops reading the socket, so the TCP window of the peer fills
 *   and its writes block, until resume_reading().
 *
 * Threads. Callbacks run on the thread of the websocket. From any thread: send_text(),
 * send_binary(), send_*_async(), wait_writable(), set_write_limits(), on_high_watermark(),
 * on_low_watermark(), pause_reading(), resume_reading(), stop(), execute(), and the
 * queued_bytes() and queued_messages() getters (relaxed atomics, as Node's bufferedAmount).
 * On the thread of the websocket only (e.g. from its callbacks, or execute()):
 * get_write_limits(), queue_full(), is_reading_paused() and
 * release_socket(); on_message() and on_shutdown() are set before it starts (in the handler
 * that accepts it).
 */
class websocket_connection : public std::enable_shared_from_this<websocket_connection>{

public:

    /**
    * Parameter for controlling the maximum number of bytes used in the incoming
    * buffer
    */
    static constexpr size_t DEFAULT_BUFFER_SIZE = 1024;        // 1KB
    static constexpr size_t MAX_BUFFER_SIZE = 16*1024*1024;   // 16MB

    /**
    * Parameter for controlling the maximum number of pending messages stored
    * in the output queue, by default (see set_write_limits()).
    */
    static constexpr int MAX_OUTPUT_MESSAGES = 100;

    /**
     * Constructor that requires a socket
     * @param socket
     */
    websocket_connection(std::shared_ptr<asio::websocket> socket);

    /**
     * Destructor
     */
    virtual ~websocket_connection();

    /**
     * Set a callbback to listen for text frames received
     * @param callback
     */
    void on_message(sync_callback<void(std::string, bool binary)> callback);

    /**
     * Set what to do when the server shuts down gracefully (see http_server_base::shutdown()),
     * on the websocket thread: e.g. send a last message and stop(). By default the websocket
     * is closed with 1001 (Going Away). Whatever is open at the end of the grace period is
     * closed anyway
     * @param callback
     */
    void on_shutdown(sync_callback<void()> callback);

    /**
     * Set a message handler
     * @param handler
     */
    //void set_stream_handler(std::shared_ptr<messages::stream_handler> handler);


    /**
     * Execute actions on the websocket thread
     * @param callback
     */
    inline void execute(sync_callback<void()> callback){
        boost::asio::dispatch(ws_->get_io_context(), [self = shared_from_this(), callback = std::move(callback)](){
            callback();
        });
    }

    /**
     * Start the websocket. Required for control lifetime and reading messages
     */
    void start();

    /**
     * Stop the webbsocket
     */
    void stop();

    /**
     * Whether the queue is full: the overflow policy applies to the next message sent without
     * waiting. Not the watermark state (see wait_writable())
     */
    bool queue_full() const;

    /**
     * Send a text frame over connection, without waiting: queued, or handled by the overflow
     * policy if the queue is full
     * @param text
     */
    void send_text(std::string text);

    /**
       * Send a binary frame over connection, without waiting (see send_text())
       * @param text
       */
    void send_binary(std::string data);

    /**
     * Send a text frame and wait until it is written to the socket (taken by the kernel): true,
     * or false if the connection closed or failed first. Never dropped by the overflow
     * policy; a cancelled wait leaves the message queued
     */
    awaitable<bool> send_text_async(std::string text);

    /**
     * Send a binary frame and wait until it is written (see send_text_async())
     */
    awaitable<bool> send_binary_async(std::string data);

    /**
     * Limits of the queue, watermarks and overflow policy (see write_limits)
     */
    void set_write_limits(const write_limits& limits);
    write_limits get_write_limits() const;

    /**
     * Payload bytes, and messages, queued and not written yet (the one being written included)
     */
    size_t queued_bytes() const;
    size_t queued_messages() const;

    /**
     * Told when the bytes queued reach the high watermark, and when they drain to the low
     * watermark after that: posted to the thread of the websocket (never from within a send),
     * and not once it is closing
     */
    void on_high_watermark(sync_callback<void()> callback);
    void on_low_watermark(sync_callback<void()> callback);

    /**
     * Wait while the queue is above its high watermark, until it drains to the low one: true,
     * or false if the connection closed. Right away if it is not congested
     */
    awaitable<bool> wait_writable();

    /**
     * Stop reading from the socket, from the next read on: messages already received are
     * delivered, nothing else is read, and the peer's writes block once the TCP buffers fill.
     * Called on the thread of the websocket (e.g. from on_message()) no other message is
     * delivered; from another thread, one more may be. Writes go on. While paused, pings and
     * the close frame of the peer are not seen (as in any WebSocket not read: the peer may
     * time out), and the keepalive is suspended, as its pongs could not be read: a dead peer
     * is not noticed until reading resumes, so bound the pause, or set a write_timeout (see
     * write_limits) for a peer that stops reading too. stop() still closes it: the closing
     * handshake reads up to the close frame of the peer, discarding the messages before it
     */
    void pause_reading();

    /**
     * Read again, in order and from where reading stopped; the keepalive is restarted
     */
    void resume_reading();

    bool is_reading_paused() const;

    /**
    * Return the base socket used in this client connection and release it for being used by another connection handler.
    * No further calls mut be done to this instance.
    */
    std::shared_ptr<asio::socket> release_socket();

private:

    /**
     * Start the read loop coroutine
     */
    void start_read_loop();

    /**
     * Main read loop coroutine
     */
    awaitable<void> read_loop();

    /**
     * Deliver the message read whole: false if it is not valid
     */
    bool deliver_message();

    /**
     * Wait while reading is paused: false if the websocket closed, or was released
     */
    awaitable<bool> wait_resumed();

    /**
     * Wake the read loop paused
     */
    void wake_reader();

    /**
     * Keepalive suspended while paused, and restarted on resume
     */
    void suspend_keepalive();
    void restore_keepalive();

    /**
     * Close the connection (waiting for the handshake), and end the read loop if paused
     */
    awaitable<void> close_connection(uint16_t code);

    struct send_waiter;

    struct out_message {
        std::string data;
        bool binary = false;
        /// The sender waiting for it to be written (send_*_async()), if any
        std::shared_ptr<send_waiter> waiter;
    };

    /**
     * Queue a message sent without waiting, applying the overflow policy if the queue is full
     */
    void push_message(std::string data, bool binary);

    /**
     * Drop the oldest messages that may be dropped until one of `size` bytes fits: false if
     * it does not fit even so
     */
    bool drop_oldest(size_t size);

    /**
     * Watch the write starting, with a write timeout
     */
    void watch_write();

    /**
     * Close a websocket that does not keep up: 1013 (Try Again Later)
     */
    void close_congested(const char* reason);

    /**
     * Queue a message, and start writing if needed
     */
    void enqueue(out_message message);

    awaitable<bool> send_async(std::string data, bool binary);

    /**
     * Process the output queue
     */
    void process_out_queue();

    /**
     * Write the messages queued, one at a time (`self` keeps the connection alive)
     */
    awaitable<void> write_loop(std::shared_ptr<websocket_connection> self);

    /**
     * Writing ended for good: the messages queued are discarded (their senders told)
     */
    void discard_queue();

    /**
     * The server shuts down (on the websocket thread)
     */
    void server_shutdown();

private:

    /// Socket being used for the websocket connection
    std::shared_ptr<asio::websocket> ws_;

    /// Shared keeper to keep the connection alive
    //std::shared_ptr<base::shared_keeper<websocket_connection>> shared_keeper_;

    /// Out queue: messages not written yet (the one being written is out of it)
    std::deque<out_message> out_queue_;

    /// Bytes queued against the write limits
    write_flow flow_;

    /// Buffer for incoming data.
    boost::asio::streambuf buffer_{MAX_BUFFER_SIZE};

    /// Message listener
    //std::shared_ptr<messages::stream_handler> stream_handler_;

    /// Text frame callback
    sync_callback<void(std::string, bool)> on_frame_callback_;

    /// Server shutdown callback
    sync_callback<void()> on_shutdown_callback_;

    /// The server tracking this connection, if any (see connection_tracker.hpp)
    std::shared_ptr<taken_over_connection> tracking_;

    bool writing_ = false;

    /// Reading paused (see pause_reading()), and the timer the read loop waits on meanwhile
    bool paused_ = false;
    boost::asio::steady_timer resume_timer_;

    /// Started (start()), and released (release_socket())
    bool started_ = false;
    bool released_ = false;

    /// Keepalive settings while suspended
    std::chrono::milliseconds keepalive_ping_{0};
    std::chrono::milliseconds keepalive_idle_{0};
};

}

#endif
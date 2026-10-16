#ifndef THINGER_HTTP_SERVER_FLOW_CONTROL_HPP
#define THINGER_HTTP_SERVER_FLOW_CONTROL_HPP

#include <cstddef>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include "../../util/types.hpp"

namespace thinger::http {

    // What a connection does with a message sent without waiting (WebSocket send_text() and
    // send_binary(), SSE events) when its queue is full
    enum class overflow_policy {
        drop_newest,    // discard the message sent (the default)
        drop_oldest,    // discard the oldest messages queued to make room for it
        close           // close the connection (a WebSocket with 1013, Try Again Later)
    };

    /**
     * Limits of the queue of a server connection that pushes messages (WebSocket, SSE).
     *
     * The queue holds what is not written yet, the message being written included. It is full
     * when another message would go over `max_messages`, or over `max_bytes` of payload (an
     * empty queue takes a message of any size); 0 disables each limit. Messages sent without
     * waiting to a full queue are handled by `policy`: drop_oldest never drops the message
     * being written, nor those a sender is waiting for (send_text_async()), nor the fields of
     * an SSE event already being written.
     *
     * The watermarks tell a producer when to stop and when to go on, as the highWaterMark and
     * the 'drain' event of Node streams: once the bytes queued reach `high_watermark`, the
     * connection is congested until they drain to `low_watermark` (on_high_watermark() and
     * on_low_watermark() are called once per cycle, and wait_writable() waits meanwhile). A
     * `high_watermark` of 0 disables them.
     */
    struct write_limits {
        size_t max_messages = 100;
        size_t max_bytes = 0;
        size_t high_watermark = 0;
        size_t low_watermark = 0;
        overflow_policy policy = overflow_policy::drop_newest;
    };

    /**
     * Accounting of the bytes queued by a connection against its write_limits: watermark
     * callbacks, and producers waiting for the queue to drain. On the thread of the connection.
     */
    class write_flow {
    public:
        explicit write_flow(boost::asio::io_context& io_context, write_limits limits = {});

        void set_limits(const write_limits& limits);
        const write_limits& limits() const { return limits_; }

        /// Whether a message of `size` bytes goes over the limits of a queue holding `messages`
        bool overflows(size_t messages, size_t size) const;

        /// A message of `size` bytes is queued (may reach the high watermark)
        void add(size_t size);

        /// A message of `size` bytes is written or dropped (may drain to the low watermark)
        void remove(size_t size);

        size_t queued_bytes() const { return queued_bytes_; }

        /// Above the high watermark, not drained to the low one yet
        bool congested() const { return congested_; }

        void on_high_watermark(sync_callback<void()> callback) { on_high_ = std::move(callback); }
        void on_low_watermark(sync_callback<void()> callback) { on_low_ = std::move(callback); }

        /// Wait while congested: true once drained, false if the connection closed (or the
        /// wait was cancelled)
        awaitable<bool> wait_writable();

        /// The connection closed: waits end, and the callbacks are released
        void close();
        bool closed() const { return closed_; }

    private:
        static void notify(const sync_callback<void()>& callback);

        write_limits limits_;
        size_t queued_bytes_ = 0;
        bool congested_ = false;
        bool closed_ = false;
        /// Never expires: cancelled to wake the producers waiting
        boost::asio::steady_timer drained_;
        sync_callback<void()> on_high_;
        sync_callback<void()> on_low_;
    };

    /// Run `operation` on the thread of `io_context`, resuming the caller where it was
    template<typename T>
    awaitable<T> on_io_context(boost::asio::io_context& io_context, awaitable<T> operation) {
        if (io_context.get_executor().running_in_this_thread()) co_return co_await std::move(operation);
        co_return co_await co_spawn(io_context, std::move(operation), use_awaitable);
    }

}

#endif

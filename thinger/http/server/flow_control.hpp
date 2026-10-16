#ifndef THINGER_HTTP_SERVER_FLOW_CONTROL_HPP
#define THINGER_HTTP_SERVER_FLOW_CONTROL_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
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
     * waiting to a full queue are handled by `policy`. drop_oldest never drops the message
     * being written, nor those a sender is waiting for (send_text_async()), nor the fields of
     * an SSE event already being written; when that is not enough, the new message is dropped
     * instead, so the limits hold.
     *
     * The watermarks tell a producer when to stop and when to go on, as Netty's
     * WriteBufferWaterMark and isWritable(): once the bytes queued reach `high_watermark`, the
     * connection is not writable until they drain to `low_watermark`. on_high_watermark() and
     * on_low_watermark() are told once per cycle, asynchronously (posted to the connection's
     * thread, never from within a send), and wait_writable() waits meanwhile. A
     * `high_watermark` of 0 disables them.
     *
     * `write_timeout` (0, off, by default): a message whose write to the socket does not
     * complete within it closes the connection (a WebSocket with 1013), as a client that
     * stopped reading; awaited sends and wait_writable() then complete with false.
     */
    struct write_limits {
        size_t max_messages = 100;
        size_t max_bytes = 0;
        size_t high_watermark = 0;
        size_t low_watermark = 0;
        overflow_policy policy = overflow_policy::drop_newest;
        std::chrono::milliseconds write_timeout{0};
    };

    /**
     * Accounting of the messages queued by a connection against its write_limits: watermark
     * notifications, producers waiting for the queue to drain, and the write timeout. On the
     * thread of the connection, except queued_bytes() and queued_messages(), which may be read
     * from any thread.
     */
    class write_flow {
    public:
        explicit write_flow(boost::asio::io_context& io_context, write_limits limits = {});
        ~write_flow();

        write_flow(const write_flow&) = delete;
        write_flow& operator=(const write_flow&) = delete;

        /// New limits, the watermark state re-evaluated with them
        void set_limits(const write_limits& limits);
        const write_limits& limits() const { return limits_; }

        /// Whether a message of `size` bytes goes over the limits, once `dropped_bytes` and
        /// `dropped_messages` are out of the queue
        bool overflows(size_t size, size_t dropped_bytes = 0, size_t dropped_messages = 0) const;

        /// What to do with a message sent without waiting
        enum class admission { queue, drop, close };

        /// The overflow policy, for a message of `size` bytes sent without waiting: queued if it
        /// fits. drop_oldest calls `drop_oldest(size)`, which drops what the connection may and
        /// tells whether the message fits then; if not, it is dropped instead
        template<typename DropOldest>
        admission admit(size_t size, DropOldest&& drop_oldest) {
            if(!overflows(size)) return admission::queue;
            switch(limits_.policy){
                case overflow_policy::drop_newest: return admission::drop;
                case overflow_policy::close: return admission::close;
                case overflow_policy::drop_oldest: break;
            }
            return drop_oldest(size) ? admission::queue : admission::drop;
        }

        /// A message of `size` bytes is queued (may reach the high watermark)
        void add(size_t size);

        /// `messages` of `bytes` in all are written or dropped (may drain to the low watermark)
        void remove(size_t bytes, size_t messages = 1);

        /// Nothing is queued anymore (no notification): once closed
        void clear();

        size_t queued_bytes() const { return queued_bytes_.load(std::memory_order_relaxed); }
        size_t queued_messages() const { return queued_messages_.load(std::memory_order_relaxed); }

        /// Above the high watermark, not drained to the low one yet
        bool congested() const { return congested_; }

        /// Wait while congested: true once drained, false if the connection closed (or the
        /// wait was cancelled)
        awaitable<bool> wait_writable();

        /// From any thread: run on the thread of the connection, keeping `owner` (the
        /// connection holding this flow) alive until then
        void set_limits(std::shared_ptr<void> owner, const write_limits& limits);
        void on_high_watermark(std::shared_ptr<void> owner, sync_callback<void()> callback);
        void on_low_watermark(std::shared_ptr<void> owner, sync_callback<void()> callback);
        awaitable<bool> wait_writable(std::shared_ptr<void> owner);

        /// The write of a message starts, and finishes: with a write timeout, a write that does
        /// not finish in time calls `on_timeout(connection)`, if the connection still lives
        template<typename Connection, typename OnTimeout>
        void write_started(Connection& connection, OnTimeout on_timeout) {
            if(closed_ || limits_.write_timeout.count() <= 0) return;
            watch_write([weak = connection.weak_from_this(), on_timeout = std::move(on_timeout)]{
                if(auto self = weak.lock()) on_timeout(*self);
            });
        }
        void write_finished();

        /// The connection closed: waits end, notifications pending are dropped, and the
        /// callbacks are released
        void close();
        bool closed() const { return closed_; }

    private:
        /// Shared with the notifications posted and the write timer, which may outlive this
        struct shared_state {
            bool closed = false;
            uint64_t write_generation = 0;
            sync_callback<void()> on_high;
            sync_callback<void()> on_low;
        };

        void dispatch(std::shared_ptr<void> owner, sync_callback<void()> operation);
        void watch_write(sync_callback<void()> on_timeout);
        void update_watermarks();
        void notify(bool high);

        boost::asio::io_context& io_context_;
        std::shared_ptr<shared_state> state_;
        write_limits limits_;
        std::atomic<size_t> queued_bytes_{0};
        std::atomic<size_t> queued_messages_{0};
        bool congested_ = false;
        bool closed_ = false;
        bool write_watched_ = false;
        /// Never expires: cancelled to wake the producers waiting
        boost::asio::steady_timer drained_;
        boost::asio::steady_timer write_timer_;
    };

}

#endif

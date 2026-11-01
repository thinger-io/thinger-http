#ifndef THINGER_HTTP_SERVER_FLOW_CONTROL_HPP
#define THINGER_HTTP_SERVER_FLOW_CONTROL_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <boost/asio/any_io_executor.hpp>
#include "../../asio/async_event.hpp"
#include "../../asio/watchdog.hpp"
#include "../../util/types.hpp"

namespace thinger::http {

    // What a connection does with a message sent without waiting to a full queue
    enum class overflow_policy {
        drop_newest,    // discard the message sent (the default)
        drop_oldest,    // discard the oldest messages queued to make room for it
        close           // close the connection (a WebSocket with 1013, Try Again Later)
    };

    /**
     * Limits of the queue of a server connection (WebSocket, SSE): what is not written yet, the
     * message being written included. 0 disables a limit.
     */
    struct write_limits {
        /// Full when another message would go over either (an empty queue takes any size)
        size_t max_messages = 100;
        size_t max_bytes = 0;
        /// Once the bytes queued reach the high watermark, the connection is not writable until
        /// they drain to the low one (as Netty's WriteBufferWaterMark): on_high_watermark() and
        /// on_low_watermark() are told once per cycle, posted to the connection's thread (never
        /// from within a send), and wait_writable() waits meanwhile
        size_t high_watermark = 0;
        size_t low_watermark = 0;
        /// For messages sent without waiting. drop_oldest never drops the message being
        /// written, one a sender waits for, nor the rest of an SSE event being written; when
        /// that is not enough, the new message is dropped, so the limits hold
        overflow_policy policy = overflow_policy::drop_newest;
        /// A write that makes no progress for this long (no byte taken by the socket, nor by the
        /// peer from it, watched for a quarter of it more: see set_send_backlog()) closes
        /// the connection (a WebSocket with 1013), as a client that stopped reading; awaited sends and wait_writable() then return false. A stall, not the time to
        /// write a message: a slow client that keeps reading is never cut. 60 s, nginx's
        /// send_timeout; 0 disables it
        std::chrono::milliseconds write_timeout{60000};
    };

    /**
     * Accounting of the queue of a connection against its write_limits. On the executor of the
     * connection, except queued_bytes(), queued_messages() and the calls taking an `owner`.
     */
    class write_flow {
    public:
        explicit write_flow(boost::asio::any_io_executor executor, write_limits limits = {});
        ~write_flow();

        write_flow(const write_flow&) = delete;
        write_flow& operator=(const write_flow&) = delete;

        /// The watermark state is re-evaluated with the new limits
        void set_limits(const write_limits& limits);
        const write_limits& limits() const { return limits_; }

        /// Whether a message of `size` bytes goes over the limits, once `dropped_bytes` and
        /// `dropped_messages` are out of the queue
        bool overflows(size_t size, size_t dropped_bytes = 0, size_t dropped_messages = 0) const;

        /// What to do with a message sent without waiting
        enum class admission { queue, drop, close };

        /// The overflow policy for a message of `size` bytes sent without waiting.
        /// `drop_oldest(size)` drops what the connection may and tells whether it fits then
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

        /// Nothing is queued anymore, without notifications: once closed
        void clear();

        size_t queued_bytes() const { return queued_bytes_.load(std::memory_order_relaxed); }
        size_t queued_messages() const { return queued_messages_.load(std::memory_order_relaxed); }

        /// Above the high watermark, not drained to the low one yet
        bool congested() const { return congested_; }

        /// Wait while congested: true once drained, false if closed (or cancelled)
        awaitable<bool> wait_writable();

        /// From any thread: run on the connection thread, `owner` (the connection) kept alive
        void set_limits(std::shared_ptr<void> owner, const write_limits& limits);
        void on_high_watermark(std::shared_ptr<void> owner, sync_callback<void()> callback);
        void on_low_watermark(std::shared_ptr<void> owner, sync_callback<void()> callback);
        awaitable<bool> wait_writable(std::shared_ptr<void> owner);

        /// A write starts, makes progress (some bytes taken by the socket), and finishes: one
        /// without progress for the write timeout calls `on_timeout`, by a writer that keeps
        /// its connection alive until the write finishes. Progress moves the deadline lazily
        /// (see asio::watchdog): no timer operation
        void write_started(sync_callback<void()> on_timeout);
        void write_progressed();
        void write_finished();

        /// The bytes the socket holds that the peer has not taken (asio::socket::send_backlog()),
        /// checked once a write made no progress for the write timeout: their fall is progress
        /// too. A backlog that does not fall within 1/BACKLOG_WATCH of the timeout after it times
        /// the write out; one that does is checked every 1/BACKLOG_CHECKS of it from then. The
        /// system may wake a writer only once much of its buffer is free (a third of it, over TCP
        /// on Linux), long after a slow reader took the first byte. A write that progresses
        /// within its timeout is never checked: no timer operation per message
        void set_send_backlog(sync_callback<size_t()> backlog);
        static constexpr int BACKLOG_CHECKS = 16;
        static constexpr int BACKLOG_WATCH = 4;

        /// Waits end, pending notifications are dropped and the callbacks released
        void close();
        bool closed() const { return closed_; }

    private:
        /// Shared with the notifications posted, which may outlive this
        struct shared_state {
            bool closed = false;
            sync_callback<void()> on_high;
            sync_callback<void()> on_low;
        };

        void dispatch(std::shared_ptr<void> owner, sync_callback<void()> operation);
        /// The write deadline from the last progress, with the limits set now
        void arm_write_timer();
        /// The timer expired: a backlog check, or the write timed out
        void write_expired();
        void update_watermarks();
        void notify(bool high);

        boost::asio::any_io_executor executor_;
        std::shared_ptr<shared_state> state_;
        write_limits limits_;
        std::atomic<size_t> queued_bytes_{0};
        std::atomic<size_t> queued_messages_{0};
        bool congested_ = false;
        bool closed_ = false;
        bool write_watched_ = false;
        /// Set once the queue drains (or closes), waking the producers waiting; reset while congested
        asio::async_event drained_;
        /// The write timeout, and what it calls: released once the write finishes
        asio::watchdog write_timer_;
        std::chrono::steady_clock::time_point write_progress_{};
        sync_callback<void()> on_write_timeout_;
        sync_callback<size_t()> send_backlog_;
        /// The backlog at the last check since the last progress, if checked
        size_t backlog_seen_ = 0;
        bool backlog_checked_ = false;
    };

}

#endif

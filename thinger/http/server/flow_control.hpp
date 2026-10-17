#ifndef THINGER_HTTP_SERVER_FLOW_CONTROL_HPP
#define THINGER_HTTP_SERVER_FLOW_CONTROL_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>
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
        /// A write to the socket not done in time closes the connection (a WebSocket with 1013),
        /// as a client that stopped reading; awaited sends and wait_writable() then return false
        std::chrono::milliseconds write_timeout{0};
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

        /// A write starts, and finishes: one not done within the write timeout calls
        /// `on_timeout(connection)`, if the connection still lives
        template<typename Connection, typename OnTimeout>
        void write_started(Connection& connection, OnTimeout on_timeout) {
            if(closed_ || limits_.write_timeout.count() <= 0) return;
            watch_write([weak = connection.weak_from_this(), on_timeout = std::move(on_timeout)]{
                if(auto self = weak.lock()) on_timeout(*self);
            });
        }
        void write_finished();

        /// Waits end, pending notifications are dropped and the callbacks released
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

        boost::asio::any_io_executor executor_;
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

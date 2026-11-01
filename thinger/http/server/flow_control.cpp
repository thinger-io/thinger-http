#include "flow_control.hpp"
#include <algorithm>

namespace thinger::http {

    write_flow::write_flow(boost::asio::any_io_executor executor, write_limits limits) :
        executor_(std::move(executor)),
        state_(std::make_shared<shared_state>()),
        drained_(executor_),
        // a member: never called once this is gone
        write_timer_(executor_, [this]{ write_expired(); })
    {
        set_limits(limits);
    }

    write_flow::~write_flow(){
        // notifications still posted find it closed
        state_->closed = true;
    }

    void write_flow::set_limits(const write_limits& limits){
        auto timed = limits_.write_timeout.count() > 0;
        limits_ = limits;
        // the low watermark is never above the high one
        limits_.low_watermark = std::min(limits_.low_watermark, limits_.high_watermark);
        update_watermarks();
        // a pending write gets the new deadline at once: from its last progress, or from now
        // if it had none (0 disarms it)
        if(write_watched_ && !closed_){
            if(!timed) write_progress_ = std::chrono::steady_clock::now();
            arm_write_timer();
        }
    }

    bool write_flow::overflows(size_t size, size_t dropped_bytes, size_t dropped_messages) const{
        auto messages = queued_messages() - std::min(dropped_messages, queued_messages());
        auto bytes = queued_bytes() - std::min(dropped_bytes, queued_bytes());
        if(limits_.max_messages && messages >= limits_.max_messages) return true;
        return limits_.max_bytes && bytes > 0 && bytes + size > limits_.max_bytes;
    }

    void write_flow::add(size_t size){
        // single writer (the connection thread): relaxed stores, for readers on other threads
        queued_bytes_.store(queued_bytes() + size, std::memory_order_relaxed);
        queued_messages_.store(queued_messages() + 1, std::memory_order_relaxed);
        update_watermarks();
    }

    void write_flow::remove(size_t bytes, size_t messages){
        queued_bytes_.store(queued_bytes() - std::min(bytes, queued_bytes()), std::memory_order_relaxed);
        queued_messages_.store(queued_messages() - std::min(messages, queued_messages()), std::memory_order_relaxed);
        update_watermarks();
    }

    void write_flow::clear(){
        queued_bytes_.store(0, std::memory_order_relaxed);
        queued_messages_.store(0, std::memory_order_relaxed);
    }

    void write_flow::update_watermarks(){
        if(closed_) return;
        auto queued = queued_bytes();
        if(!congested_ && limits_.high_watermark && queued >= limits_.high_watermark){
            congested_ = true;
            drained_.reset();
            notify(true);
        }else if(congested_ && (!limits_.high_watermark || queued <= limits_.low_watermark)){
            congested_ = false;
            drained_.set();
            notify(false);
        }
    }

    void write_flow::notify(bool high){
        if(!(high ? state_->on_high : state_->on_low)) return;
        // never from within a send or a drop: whatever the callback does (sending, closing...)
        // finds the queue consistent. Dropped if the connection closes meanwhile
        boost::asio::post(executor_, [state = state_, high]{
            if(state->closed) return;
            auto callback = high ? state->on_high : state->on_low;
            if(callback) callback();
        });
    }

    awaitable<bool> write_flow::wait_writable(){
        while(congested_ && !closed_){
            co_await drained_.wait();
            auto state = co_await boost::asio::this_coro::cancellation_state;
            if(state.cancelled() != boost::asio::cancellation_type::none) co_return false;
        }
        co_return !closed_;
    }

    void write_flow::dispatch(std::shared_ptr<void> owner, sync_callback<void()> operation){
        boost::asio::dispatch(executor_, [owner = std::move(owner), operation = std::move(operation)]{
            operation();
        });
    }

    void write_flow::set_limits(std::shared_ptr<void> owner, const write_limits& limits){
        dispatch(std::move(owner), [this, limits]{ set_limits(limits); });
    }

    void write_flow::on_high_watermark(std::shared_ptr<void> owner, sync_callback<void()> callback){
        dispatch(std::move(owner), [this, callback = std::move(callback)]() mutable {
            if(!closed_) state_->on_high = std::move(callback);
        });
    }

    void write_flow::on_low_watermark(std::shared_ptr<void> owner, sync_callback<void()> callback){
        dispatch(std::move(owner), [this, callback = std::move(callback)]() mutable {
            if(!closed_) state_->on_low = std::move(callback);
        });
    }

    awaitable<bool> write_flow::wait_writable(std::shared_ptr<void> owner){
        return on_executor(executor_, std::move(owner), wait_writable());
    }

    void write_flow::write_started(sync_callback<void()> on_timeout){
        if(closed_) return;
        // watched also without a deadline, which set_limits() may give it meanwhile
        write_watched_ = true;
        on_write_timeout_ = std::move(on_timeout);
        write_progressed();
    }

    void write_flow::write_progressed(){
        if(!write_watched_ || limits_.write_timeout.count() <= 0) return;
        write_progress_ = std::chrono::steady_clock::now();
        backlog_checked_ = false;
        arm_write_timer();
    }

    void write_flow::set_send_backlog(sync_callback<size_t()> backlog){
        send_backlog_ = std::move(backlog);
    }

    namespace {
        std::chrono::milliseconds backlog_interval(std::chrono::milliseconds timeout){
            return std::max(timeout / write_flow::BACKLOG_CHECKS, std::chrono::milliseconds(1));
        }
    }

    void write_flow::arm_write_timer(){
        if(limits_.write_timeout.count() <= 0){
            write_timer_.cancel();
            return;
        }
        // a later deadline does not touch the timer (see watchdog): the backlog is checked only
        // once it passes, so a write that progresses within it costs no timer operation
        write_timer_.expires_at(write_progress_ + limits_.write_timeout);
    }

    void write_flow::write_expired(){
        // the deadline of a write already finished (see write_finished())
        if(!write_watched_) return;
        if(send_backlog_ && limits_.write_timeout.count() > 0){
            auto now = std::chrono::steady_clock::now();
            auto backlog = send_backlog_();
            // the first check, at the deadline, watches the backlog for BACKLOG_WATCH more (a peer
            // may take bytes in bursts, e.g. a window of one segment, 64 KiB over loopback); then
            // the peer taking bytes since the last check is progress, as the socket taking some
            auto deadline = now + limits_.write_timeout / BACKLOG_WATCH;
            if(backlog_checked_){
                if(backlog < backlog_seen_) write_progress_ = now;
                deadline = std::min(now + backlog_interval(limits_.write_timeout), write_progress_ + limits_.write_timeout);
            }
            backlog_seen_ = backlog;
            backlog_checked_ = true;
            if(now < deadline){
                write_timer_.expires_at(deadline);
                return;
            }
        }
        // on_timeout may destroy this, so nothing of it is touched after the call
        auto on_timeout = std::move(on_write_timeout_);
        on_write_timeout_ = nullptr;
        if(on_timeout) on_timeout();
    }

    void write_flow::write_finished(){
        if(!write_watched_) return;
        write_watched_ = false;
        // the timer is left armed: the next write's deadline, later, does not touch it (see
        // watchdog), so a stream of writes costs no timer operation each; an expiry with no
        // write pending does nothing
        on_write_timeout_ = nullptr;
    }

    void write_flow::close(){
        closed_ = true;
        congested_ = false;
        state_->closed = true;
        state_->on_high = nullptr;
        state_->on_low = nullptr;
        drained_.set();
        write_timer_.cancel();
        on_write_timeout_ = nullptr;
    }

}

#include "flow_control.hpp"
#include <algorithm>

namespace thinger::http {

    write_flow::write_flow(boost::asio::io_context& io_context, write_limits limits) :
        io_context_(io_context),
        state_(std::make_shared<shared_state>()),
        drained_(io_context, boost::asio::steady_timer::time_point::max()),
        write_timer_(io_context)
    {
        set_limits(limits);
    }

    write_flow::~write_flow(){
        // notifications still posted find it closed
        state_->closed = true;
    }

    void write_flow::set_limits(const write_limits& limits){
        limits_ = limits;
        // the low watermark is never above the high one
        limits_.low_watermark = std::min(limits_.low_watermark, limits_.high_watermark);
        update_watermarks();
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
            notify(true);
        }else if(congested_ && (!limits_.high_watermark || queued <= limits_.low_watermark)){
            congested_ = false;
            drained_.cancel();
            notify(false);
        }
    }

    void write_flow::notify(bool high){
        if(!(high ? state_->on_high : state_->on_low)) return;
        // never from within a send or a drop: whatever the callback does (sending, closing...)
        // finds the queue consistent. Dropped if the connection closes meanwhile
        boost::asio::post(io_context_, [state = state_, high]{
            if(state->closed) return;
            auto callback = high ? state->on_high : state->on_low;
            if(callback) callback();
        });
    }

    void write_flow::on_high_watermark(sync_callback<void()> callback){
        if(!closed_) state_->on_high = std::move(callback);
    }

    void write_flow::on_low_watermark(sync_callback<void()> callback){
        if(!closed_) state_->on_low = std::move(callback);
    }

    awaitable<bool> write_flow::wait_writable(){
        while(congested_ && !closed_){
            co_await drained_.async_wait(use_nothrow_awaitable);
            auto state = co_await boost::asio::this_coro::cancellation_state;
            if(state.cancelled() != boost::asio::cancellation_type::none) co_return false;
        }
        co_return !closed_;
    }

    void write_flow::watch_write(sync_callback<void()> on_timeout){
        write_watched_ = true;
        auto generation = ++state_->write_generation;
        write_timer_.expires_after(limits_.write_timeout);
        write_timer_.async_wait([state = state_, generation, on_timeout = std::move(on_timeout)](const boost::system::error_code& ec){
            if(ec || state->closed || state->write_generation != generation) return;
            on_timeout();
        });
    }

    void write_flow::write_finished(){
        if(!write_watched_) return;
        write_watched_ = false;
        ++state_->write_generation;
        write_timer_.cancel();
    }

    void write_flow::close(){
        closed_ = true;
        congested_ = false;
        state_->closed = true;
        state_->on_high = nullptr;
        state_->on_low = nullptr;
        drained_.cancel();
        write_timer_.cancel();
    }

}

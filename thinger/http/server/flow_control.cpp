#include "flow_control.hpp"
#include <algorithm>

namespace thinger::http {

    write_flow::write_flow(boost::asio::io_context& io_context, write_limits limits) :
        drained_(io_context, boost::asio::steady_timer::time_point::max())
    {
        set_limits(limits);
    }

    void write_flow::set_limits(const write_limits& limits){
        limits_ = limits;
        // the low watermark is never above the high one
        limits_.low_watermark = std::min(limits_.low_watermark, limits_.high_watermark);
    }

    bool write_flow::overflows(size_t messages, size_t size) const{
        if(limits_.max_messages && messages >= limits_.max_messages) return true;
        return limits_.max_bytes && queued_bytes_ > 0 && queued_bytes_ + size > limits_.max_bytes;
    }

    void write_flow::add(size_t size){
        queued_bytes_ += size;
        if(congested_ || closed_ || !limits_.high_watermark || queued_bytes_ < limits_.high_watermark) return;
        congested_ = true;
        notify(on_high_);
    }

    void write_flow::remove(size_t size){
        queued_bytes_ -= std::min(size, queued_bytes_);
        if(!congested_ || queued_bytes_ > limits_.low_watermark) return;
        congested_ = false;
        drained_.cancel();
        notify(on_low_);
    }

    void write_flow::notify(const sync_callback<void()>& callback){
        // a copy: the callback may replace itself, or close the connection
        if(!callback) return;
        auto call = callback;
        call();
    }

    awaitable<bool> write_flow::wait_writable(){
        while(congested_ && !closed_){
            co_await drained_.async_wait(use_nothrow_awaitable);
            auto state = co_await boost::asio::this_coro::cancellation_state;
            if(state.cancelled() != boost::asio::cancellation_type::none) co_return false;
        }
        co_return !closed_;
    }

    void write_flow::close(){
        closed_ = true;
        congested_ = false;
        drained_.cancel();
        on_high_ = nullptr;
        on_low_ = nullptr;
    }

}

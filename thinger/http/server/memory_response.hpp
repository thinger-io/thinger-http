#ifndef THINGER_HTTP_SERVER_MEMORY_RESPONSE_HPP
#define THINGER_HTTP_SERVER_MEMORY_RESPONSE_HPP

#include "../common/http_response.hpp"
#include "response_sink.hpp"
#include "../../util/types.hpp"
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <memory>
#include <mutex>

namespace thinger::http {

// Collects the response of a request dispatched in memory (see http_application::dispatch).
// It is shared by every copy of the response object, so a handler may keep a copy and
// answer later, from any thread.
class memory_response : public response_sink {
public:
    explicit memory_response(const boost::asio::any_io_executor& executor) : done_(executor, 1) {}

    // Whole response
    void send(std::shared_ptr<http_response> response) override {
        {
            std::lock_guard lock(mutex_);
            if (complete_) return;
            response_ = std::move(response);
            if (observer_) observer_->head(response_->get_status(), false);
            finish_locked();
        }
        observed_end();
    }

    // Chunked response, collected whole: headers first, then the data, then the end
    bool begin(std::shared_ptr<http_response> headers) override {
        std::lock_guard lock(mutex_);
        if (complete_ || response_) return false;
        response_ = std::move(headers);
        if (observer_) observer_->head(response_->get_status(), true);
        return true;
    }

    bool append(const std::string& data) override {
        std::lock_guard lock(mutex_);
        if (complete_ || !response_) return false;
        response_->get_content().append(data);
        return true;
    }

    bool finish() override {
        {
            std::lock_guard lock(mutex_);
            if (complete_ || !response_) return false;
            response_->set_content_length(response_->get_content().size());
            response_->set_last_frame(true);
            finish_locked();
        }
        observed_end();
        return true;
    }

    // A streamed response that could not be completed: its body is incomplete, so the
    // result is a 502 instead
    void abort_stream() override {
        if (observer_) observer_->abort();
        send(http_response::stock_http_reply(http_response::status::bad_gateway));
    }

    void observe(std::shared_ptr<response_observer> observer) override {
        std::lock_guard lock(mutex_);
        observer_ = std::move(observer);
    }

    bool is_complete() const {
        std::lock_guard lock(mutex_);
        return complete_;
    }

    // Wait until the response is complete; null if it does not complete within `timeout`
    awaitable<std::shared_ptr<http_response>> wait(std::chrono::milliseconds timeout) {
        if (!is_complete()) {
            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, timeout);
            co_await (done_.async_receive(use_nothrow_awaitable) || timer.async_wait(use_nothrow_awaitable));
        }
        std::lock_guard lock(mutex_);
        co_return complete_ ? response_ : nullptr;
    }

private:
    void finish_locked() {
        complete_ = true;
        done_.try_send(boost::system::error_code{});
    }

    // Tell the observer the response is complete (once, by the call completing it), without
    // the lock: it may call the response hook
    void observed_end() {
        if (!observer_) return;
        observer_->add_bytes(response_->get_content().size());
        observer_->ended(response_outcome::completed);
    }

    mutable std::mutex mutex_;
    std::shared_ptr<http_response> response_;
    bool complete_ = false;
    // Told about the response, while a response hook is set (see http_application::on_response)
    std::shared_ptr<response_observer> observer_;
    boost::asio::experimental::concurrent_channel<void(boost::system::error_code)> done_;
};

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_MEMORY_RESPONSE_HPP

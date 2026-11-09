#ifndef THINGER_HTTP_SERVER_MEMORY_RESPONSE_HPP
#define THINGER_HTTP_SERVER_MEMORY_RESPONSE_HPP

#include "../common/http_response.hpp"
#include "response_sink.hpp"
#include "../../util/types.hpp"
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/steady_timer.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace thinger::http {

// Collects the response of a request dispatched in memory (see http_application::dispatch).
// It is shared by every copy of the response object, so a handler may keep a copy and
// answer later, from any thread.
class memory_response : public response_sink, public std::enable_shared_from_this<memory_response> {
public:
    explicit memory_response(const boost::asio::any_io_executor& executor) : done_(executor, 1) {}

    // Whole response
    void send(std::shared_ptr<http_response> response) override {
        size_t bytes;
        {
            std::lock_guard lock(mutex_);
            if (complete_) return;
            response_ = std::move(response);
            if (observer_) observer_->head(response_->get_status(), false);
            bytes = response_->get_content().size();
            finish_locked();
        }
        observed_end(bytes);
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
        size_t bytes;
        {
            std::lock_guard lock(mutex_);
            if (complete_ || !response_) return false;
            bytes = response_->get_content().size();
            response_->set_content_length(bytes);
            response_->set_last_frame(true);
            finish_locked();
        }
        observed_end(bytes);
        return true;
    }

    // A streamed response that could not be completed: its body is incomplete, so the
    // result is a 502 instead
    void abort_stream() override {
        if (observer_) observer_->abort();
        send(http_response::stock_http_reply(http_response::status::bad_gateway));
    }

    void observe(std::shared_ptr<response_observer> observer) override {
        // Weak: the observer is released with this object, and may report from its destructor
        observer->on_reported([self = weak_from_this()] {
            if (auto response = self.lock()) response->settle_report();
        });
        std::lock_guard lock(mutex_);
        observer_ = std::move(observer);
    }

    bool is_complete() const {
        std::lock_guard lock(mutex_);
        return complete_;
    }

    // The application is gone before the response completed (a response in parts included):
    // there will be none. Wakes wait(), and whatever is sent later is dropped. The observer is
    // not told: it reports the response as aborted when released, once the request ends too
    void abandon() {
        std::lock_guard lock(mutex_);
        // Nothing waits for the response hook any longer (see wait_reported())
        report_settled_ = true;
        if (complete_) {
            done_.try_send(boost::system::error_code{});
            return;
        }
        response_.reset();
        finish_locked();
    }

    bool abandoned() const {
        std::lock_guard lock(mutex_);
        return complete_ && !response_;
    }

    // Wait until the response is complete; null if it does not complete within `timeout`, or it
    // was abandoned. The timer wakes the same wait: the caller may run on an executor of several
    // threads, where the two branches of an `||` finishing at once would race on the cancellation
    // signal of each other
    awaitable<std::shared_ptr<http_response>> wait(std::chrono::milliseconds timeout) {
        if (!is_complete()) {
            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, timeout);
            timer.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
                if (!ec) self->done_.try_send(boost::system::error_code{});
            });
            co_await done_.async_receive(use_nothrow_awaitable);
        }
        std::lock_guard lock(mutex_);
        co_return complete_ ? response_ : nullptr;
    }

    // With a response hook, once the response is complete: wait until the hook has run, for at
    // most `timeout`, or the application is gone (see abandon()). False if the wait was cancelled
    awaitable<bool> wait_reported(std::chrono::milliseconds timeout) {
        if (report_settled()) co_return true;
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, timeout);
        timer.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
            if (!ec) self->done_.try_send(boost::system::error_code{});
        });
        auto cancellation = co_await boost::asio::this_coro::cancellation_state;
        // A wake-up may be left from the response: each one is checked again
        while (!report_settled() && timer.expiry() > boost::asio::steady_timer::clock_type::now()) {
            if (cancellation.cancelled() != boost::asio::cancellation_type::none) co_return false;
            auto [ec] = co_await done_.async_receive(use_nothrow_awaitable);
            if (ec) co_return false;
        }
        co_return true;
    }

private:
    void settle_report() {
        std::lock_guard lock(mutex_);
        report_settled_ = true;
        done_.try_send(boost::system::error_code{});
    }

    bool report_settled() const {
        std::lock_guard lock(mutex_);
        return report_settled_;
    }

    void finish_locked() {
        complete_ = true;
        done_.try_send(boost::system::error_code{});
    }

    // Tell the observer the response is complete, with the size of its body (once, by the call
    // completing it), without the lock: it may call the response hook. Not reading the response,
    // which the dispatch may have handed to its caller by then
    void observed_end(size_t bytes) {
        if (!observer_) return;
        observer_->add_bytes(bytes);
        observer_->ended(response_outcome::completed);
    }

    mutable std::mutex mutex_;
    std::shared_ptr<http_response> response_;
    bool complete_ = false;
    // Told about the response, while a response hook is set (see http_application::on_response)
    std::shared_ptr<response_observer> observer_;
    // The response hook has run, or the application is gone and it is not waited for
    bool report_settled_ = false;
    boost::asio::experimental::concurrent_channel<void(boost::system::error_code)> done_;
};

namespace detail {

// Source of a request dispatched in memory (see http_application::dispatch): its body, as if
// just received from a client, then the end. Its peer is the address the dispatch gives, without
// TLS, and no socket is ever read
class memory_source : public request_source {
public:
    memory_source(std::string body, std::string peer_ip)
        : body_(std::move(body)), peer_ip_(std::move(peer_ip)) {}

    awaitable<io_result> read_body_some(uint8_t* buffer, size_t size) override {
        size_t bytes = std::min(size, body_.size() - read_);
        if (bytes == 0) co_return io_result{boost::asio::error::eof, 0};
        std::memcpy(buffer, body_.data() + read_, bytes);
        read_ += bytes;
        co_return io_result{boost::system::error_code{}, bytes};
    }

    connection_info info() const override { return {.peer_ip = peer_ip_}; }

private:
    std::string body_;
    size_t read_ = 0;
    std::string peer_ip_;
};

} // namespace detail

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_MEMORY_RESPONSE_HPP

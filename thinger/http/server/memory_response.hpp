#ifndef THINGER_HTTP_SERVER_MEMORY_RESPONSE_HPP
#define THINGER_HTTP_SERVER_MEMORY_RESPONSE_HPP

#include "../common/http_response.hpp"
#include "../../util/types.hpp"
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <memory>
#include <mutex>

namespace thinger::http {

// Collects the response of a request dispatched in memory (see http_server_base::dispatch).
// It is shared by every copy of the response object, so a handler may keep a copy and
// answer later, from any thread.
class memory_response {
public:
    explicit memory_response(const boost::asio::any_io_executor& executor) : done_(executor, 1) {}

    // Whole response
    void complete(std::shared_ptr<http_response> response) {
        std::lock_guard lock(mutex_);
        if (complete_) return;
        response_ = std::move(response);
        finish_locked();
    }

    // Chunked response: headers first, then the data, then the end
    void begin(std::shared_ptr<http_response> headers) {
        std::lock_guard lock(mutex_);
        if (complete_ || response_) return;
        response_ = std::move(headers);
        response_->remove_header("Transfer-Encoding");
        response_->get_content().clear();
    }

    void append(const std::string& data) {
        std::lock_guard lock(mutex_);
        if (!complete_ && response_) response_->get_content().append(data);
    }

    void finish() {
        std::lock_guard lock(mutex_);
        if (complete_ || !response_) return;
        response_->set_content_length(response_->get_content().size());
        response_->set_last_frame(true);
        finish_locked();
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

    mutable std::mutex mutex_;
    std::shared_ptr<http_response> response_;
    bool complete_ = false;
    boost::asio::experimental::concurrent_channel<void(boost::system::error_code)> done_;
};

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_MEMORY_RESPONSE_HPP

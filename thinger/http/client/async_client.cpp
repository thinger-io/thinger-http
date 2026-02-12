#include "async_client.hpp"
#include "../../util/logger.hpp"

namespace thinger::http {

async_client::async_client()
    : worker_client("http_async_client") {
    LOG_DEBUG("Created HTTP async client");
}

async_client::~async_client() {
    LOG_DEBUG("Destroying HTTP async client");
    if (is_running()) {
        stop();
    }
}

void async_client::track_request_start() {
    ++active_requests_;
    LOG_DEBUG("Async client: request started, active: {}", active_requests_.load());
}

void async_client::track_request_end() {
    size_t remaining = --active_requests_;
    LOG_DEBUG("Async client: request ended, active: {}", remaining);

    if (remaining == 0) {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        requests_cv_.notify_all();
    }
}

bool async_client::stop() {
    LOG_DEBUG("Stopping HTTP async client");

    // Call base implementation
    bool result = worker_client::stop();

    // Notify any waiters
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        requests_cv_.notify_all();
    }

    return result;
}

void async_client::wait() {
    std::unique_lock<std::mutex> lock(requests_mutex_);

    // Wait until no active requests or client stopped
    requests_cv_.wait(lock, [this] {
        return active_requests_ == 0 || !running_;
    });
}

bool async_client::wait_for(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(requests_mutex_);

    // Wait with timeout
    bool completed = requests_cv_.wait_for(lock, timeout, [this] {
        return active_requests_ == 0 || !running_;
    });

    return completed && active_requests_ == 0;
}

} // namespace thinger::http

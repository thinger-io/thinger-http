#ifndef THINGER_ASIO_WORKER_CLIENT_HPP
#define THINGER_ASIO_WORKER_CLIENT_HPP

#include <string>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace thinger::asio {

/**
 * Base class for clients that use ASIO workers.
 * This allows automatic management of worker threads based on active clients.
 * Provides common implementation for wait() and state management.
 */
class worker_client {
protected:
    mutable std::mutex wait_mutex_;
    mutable std::condition_variable wait_cv_;
    std::atomic<bool> running_{false};
    std::string service_name_;

public:
    explicit worker_client(const std::string& service_name);
    virtual ~worker_client();
    
    /**
     * Start the client service
     * Default implementation sets running_ to true
     * Derived classes should call this base implementation
     * @return true if started successfully
     */
    virtual bool start() {
        running_ = true;
        return true;
    }
    
    /**
     * Stop the client service
     * Default implementation sets running_ to false and notifies waiters
     * Derived classes should call this base implementation
     * @return true if stopped successfully
     */
    virtual bool stop() {
        running_ = false;
        notify_stopped();
        return true;
    }
    
    /**
     * Check if the client service is running
     * @return true if running
     */
    virtual bool is_running() const { return running_; }
    
    /**
     * Wait until the service stops
     * Blocks the calling thread until stop() is called
     */
    virtual void wait() {
        std::unique_lock<std::mutex> lock(wait_mutex_);
        if (running_) {
            wait_cv_.wait(lock, [this] { return !running_.load(); });
        }
    }
    
    /**
     * Get a descriptive name for this client (for debugging/logging)
     * @return service name
     */
    const std::string& get_service_name() const { return service_name_; }

protected:
    /**
     * Notify that the service has stopped
     * Should be called by derived classes in their stop() implementation
     */
    void notify_stopped() {
        wait_cv_.notify_all();
    }
};

} // namespace thinger::asio

#endif // THINGER_ASIO_WORKER_CLIENT_HPP
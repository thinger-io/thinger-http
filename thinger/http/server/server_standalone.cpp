#include "server_standalone.hpp"
#include "../../util/logger.hpp"
#include <future>

namespace thinger::http {

server::server() {
    LOG_DEBUG("Created standalone HTTP server (single-threaded)");
    // The end of a shutdown stops the io_context, without calling stop() (see
    // set_shutdown_stop())
    set_shutdown_stop([this] { stop_running(); });
}

server::~server() {
    LOG_DEBUG("Destroying standalone HTTP server");
    stop();
}

boost::asio::io_context* server::shutdown_context() {
    return running_ && serving_ ? &io_context_ : nullptr;
}

bool server::in_server_thread() {
    return io_context_.get_executor().running_in_this_thread();
}


std::unique_ptr<asio::listener> server::create_listener(asio::listener_options options) {
    // Both accepting and the connections on our io_context
    auto executor = [this]() -> boost::asio::any_io_executor { return io_context_.get_executor(); };
    return std::make_unique<asio::listener>(std::move(options), executor, executor);
}

void server::listening() {
    if (running_) return;
    // Restart io_context if it was previously stopped
    io_context_.restart();
    // Create work guard to keep io_context running
    work_guard_ = std::make_unique<work_guard_type>(io_context_.get_executor());
    // Note: io_context_.run() will be called in wait() method
    running_ = true;
}

void server::wait() {
    if (!running_) {
        return;
    }
    
    // Run io_context in the current thread
    // This blocks until the server is stopped
    LOG_DEBUG("Running io_context in current thread");
    serving_ = true;
    io_context_.run();
    serving_ = false;
    LOG_DEBUG("io_context stopped");
}

void server::stop() {
    // (a shutdown in progress ends first)
    end_shutdown();
    stop_running();
}

void server::stop_running() {
    // One exchange, so of the stop() calls made at once only one goes on
    if (!running_.exchange(false)) {
        return;
    }

    LOG_DEBUG("Stopping standalone HTTP server");

    // Stop accepting new connections first (this is always safe to call)
    server_base::stop();
    
    // Reset work guard to allow io_context to stop when it's running
    work_guard_.reset();
    
    // Stop the io_context if it's running
    // Note: io_context_.stop() is safe to call even if not running
    io_context_.stop();
    
    LOG_DEBUG("Standalone HTTP server stopped");
}

} // namespace thinger::http
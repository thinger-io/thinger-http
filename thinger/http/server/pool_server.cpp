#include "pool_server.hpp"
#include "../../asio/detail/listener_access.hpp"
#include "../../asio/io_context_of.hpp"
#include "../../util/logger.hpp"

namespace thinger::http {

pool_server::pool_server(asio::workers& workers)
    : workers_(workers) {
    LOG_DEBUG("Created HTTP pool server");
    // The end of a shutdown releases wait(), without calling stop() (see set_shutdown_stop())
    set_shutdown_stop([this] { release_wait(); });
}

pool_server::~pool_server() {
    LOG_DEBUG("Destroying HTTP pool server");
    stop();
}

std::unique_ptr<asio::listener> pool_server::create_listener(asio::listener_options options) {
    auto listener = std::make_unique<asio::listener>(
        std::move(options),
        [this]() -> boost::asio::any_io_executor { return workers_.get_thread_io_context().get_executor(); },
        workers_.provider());
    // Accepting does not wait behind the connections a worker serves
    asio::detail::listener_access::use_acceptor_thread(*listener);
    return listener;
}

boost::asio::io_context* pool_server::shutdown_context() {
    // One read of the workers, which may stop meanwhile on another thread: none once stopped
    return asio::io_context_of(workers_.next_executor());
}

bool pool_server::in_server_thread() {
    return workers_.in_worker_thread();
}

void pool_server::stop() {
    server_base::stop();
    release_wait();
}

void pool_server::release_wait() {
    {
        std::lock_guard<std::mutex> lock(wait_mutex_);
        stopped_ = true;
    }
    wait_cv_.notify_all();
}

void pool_server::wait() {
    std::unique_lock<std::mutex> lock(wait_mutex_);
    wait_cv_.wait(lock, [this] { return stopped_; });
}

} // namespace thinger::http

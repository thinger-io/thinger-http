#include "executor_client.hpp"
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <stdexcept>

namespace thinger::http {

executor_client::executor_client(boost::asio::io_context& io_context) : io_context_(io_context) {
    // Nothing runs the io_context for the client: a pending idle timer would hold the
    // connection (and the io_context) until it expired
    pool_.use_idle_timers(false);
    max_idle_connections_per_host(0);
}

executor_client::~executor_client() {
    clear_connections();
}

awaitable<std::unique_ptr<executor_client>> executor_client::for_current_coroutine() {
    auto* io_context = io_context_of(co_await boost::asio::this_coro::executor);
    if (!io_context) {
        throw std::logic_error("executor_client: the coroutine does not run on an io_context, "
                               "nor on a strand of one");
    }
    co_return std::make_unique<executor_client>(*io_context);
}

boost::asio::io_context* io_context_of(const boost::asio::any_io_executor& executor) {
    using io_executor = boost::asio::io_context::executor_type;
    if (auto* io = executor.target<io_executor>()) return &io->context();
    if (auto* strand = executor.target<boost::asio::strand<io_executor>>()) {
        return &strand->get_inner_executor().context();
    }
    if (auto* strand = executor.target<boost::asio::strand<boost::asio::any_io_executor>>()) {
        return io_context_of(strand->get_inner_executor());
    }
    return nullptr;
}

} // namespace thinger::http

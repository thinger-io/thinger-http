#include "io_context_of.hpp"
#include <boost/asio/strand.hpp>

namespace thinger::asio {

namespace {

    using io_executor = boost::asio::io_context::executor_type;
    // The executor of an io_context that keeps it running (as the work guards and the
    // executors of the handlers hold)
    using tracked_io_executor = boost::asio::io_context::basic_executor_type<std::allocator<void>, 4>;

}

boost::asio::io_context* io_context_of(const boost::asio::any_io_executor& executor) {
    if (auto* io = executor.target<io_executor>()) return &io->context();
    if (auto* strand = executor.target<boost::asio::strand<io_executor>>()) {
        return &strand->get_inner_executor().context();
    }
    if (auto* strand = executor.target<boost::asio::strand<boost::asio::any_io_executor>>()) {
        return io_context_of(strand->get_inner_executor());
    }
    return nullptr;
}

bool running_in_this_thread(const boost::asio::any_io_executor& executor) {
    if (auto* io = executor.target<io_executor>()) return io->running_in_this_thread();
    if (auto* io = executor.target<tracked_io_executor>()) return io->running_in_this_thread();
    if (auto* strand = executor.target<boost::asio::strand<io_executor>>()) return strand->running_in_this_thread();
    if (auto* strand = executor.target<boost::asio::strand<boost::asio::any_io_executor>>()) {
        return strand->running_in_this_thread();
    }
    return false;
}

} // namespace thinger::asio

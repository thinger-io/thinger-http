#include "io_context_of.hpp"
#include <boost/asio/strand.hpp>

namespace thinger::asio {

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

} // namespace thinger::asio

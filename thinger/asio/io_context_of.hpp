#ifndef THINGER_ASIO_IO_CONTEXT_OF_HPP
#define THINGER_ASIO_IO_CONTEXT_OF_HPP

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>

namespace thinger::asio {

// The io_context of an executor: an io_context executor, or a strand of one (of any depth);
// null for any other executor (e.g. of a thread_pool)
boost::asio::io_context* io_context_of(const boost::asio::any_io_executor& executor);

// Whether the caller runs on `executor`: on a thread running the io_context of an io_context
// executor, or within a strand (of any executor); false for any other executor
bool running_in_this_thread(const boost::asio::any_io_executor& executor);

} // namespace thinger::asio

#endif // THINGER_ASIO_IO_CONTEXT_OF_HPP

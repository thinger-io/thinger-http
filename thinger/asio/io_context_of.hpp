#ifndef THINGER_ASIO_IO_CONTEXT_OF_HPP
#define THINGER_ASIO_IO_CONTEXT_OF_HPP

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <functional>

namespace thinger::asio {

// The io_context of an executor: an io_context executor, or a strand of one (of any depth);
// null for any other executor (e.g. of a thread_pool)
boost::asio::io_context* io_context_of(const boost::asio::any_io_executor& executor);

// Whether the caller runs on `executor`: on a thread running the io_context of an io_context
// executor, or within a strand (of any executor); false for any other executor
bool running_in_this_thread(const boost::asio::any_io_executor& executor);

// Run `io_context` on this thread until `done()` holds: a synchronous call waiting for its own
// operation. It does not wait for the io_context to run out of work (a WebSocket keepalive
// keeps it busy for ever), and it sees an operation completed by another thread running the
// io_context too (it waits for handlers in slices of a few milliseconds). Then it runs what is
// ready already, without waiting (e.g. a keepalive ping queued meanwhile goes out), and leaves
// the io_context ready to run again. Called within a handler of the io_context, it polls it
void run_until(boost::asio::io_context& io_context, const std::function<bool()>& done);

} // namespace thinger::asio

#endif // THINGER_ASIO_IO_CONTEXT_OF_HPP

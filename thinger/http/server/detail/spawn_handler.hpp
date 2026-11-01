#ifndef THINGER_HTTP_SERVER_DETAIL_SPAWN_HANDLER_HPP
#define THINGER_HTTP_SERVER_DETAIL_SPAWN_HANDLER_HPP

#include <exception>
#include <utility>
#include <boost/asio/co_spawn.hpp>

namespace thinger::http::detail {

// Log what a coroutine run by spawn_handler() threw, as thrown by the `what` handler
void log_handler_exception(const char* what, const std::exception_ptr& error);

// Run a coroutine (or a function returning one) on `executor` (or an io_context), detached:
// what it throws is logged. Not public: the response and the file server use it, from their
// .cpp files, for the handlers and bodies that go on once the request handler returned.
template<typename Executor, typename F>
void spawn_handler(Executor&& executor, const char* what, F&& run) {
    boost::asio::co_spawn(std::forward<Executor>(executor), std::forward<F>(run),
                          [what](const std::exception_ptr& error) {
                              if (error) log_handler_exception(what, error);
                          });
}

} // namespace thinger::http::detail

#endif // THINGER_HTTP_SERVER_DETAIL_SPAWN_HANDLER_HPP

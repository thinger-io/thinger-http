#ifndef THINGER_HTTP_TEST_AWAITED_SHUTDOWN_HPP
#define THINGER_HTTP_TEST_AWAITED_SHUTDOWN_HPP

#include <thinger/http/server/server_base.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <future>
#include <thread>

namespace thinger::http::test {

// co_await server.shutdown(by), on an io_context of its own: as an application awaits it apart
// from the threads of the server. It starts on the constructing thread, so the server stopped
// listening and was told once the constructor returns, and it goes on on a thread of its own.
// The destructor waits for it to end, which it does by `by`
class awaited_shutdown {
public:
    awaited_shutdown(server_base& server, std::chrono::steady_clock::time_point by)
        : by_(by), result_(co_spawn(io_context_, server.shutdown(by), boost::asio::use_future)) {
        // Up to its first wait (or its end), here
        io_context_.poll();
        thread_ = std::thread([this] { io_context_.run(); });
    }

    awaited_shutdown(server_base& server, std::chrono::milliseconds grace)
        : awaited_shutdown(server, std::chrono::steady_clock::now() + grace) {}

    ~awaited_shutdown() { thread_.join(); }

    awaited_shutdown(const awaited_shutdown&) = delete;
    awaited_shutdown& operator=(const awaited_shutdown&) = delete;

    std::chrono::steady_clock::time_point by() const { return by_; }

    bool ended_within(std::chrono::milliseconds timeout) {
        return result_.wait_for(timeout) == std::future_status::ready;
    }

    // Its result, once it ends
    bool get() { return result_.get(); }

private:
    boost::asio::io_context io_context_;
    std::chrono::steady_clock::time_point by_;
    std::future<bool> result_;
    std::thread thread_;
};

} // namespace thinger::http::test

#endif // THINGER_HTTP_TEST_AWAITED_SHUTDOWN_HPP

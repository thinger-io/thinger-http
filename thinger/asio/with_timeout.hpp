#ifndef THINGER_ASIO_WITH_TIMEOUT_HPP
#define THINGER_ASIO_WITH_TIMEOUT_HPP

#include <chrono>
#include <optional>
#include <utility>
#include <variant>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include "../util/types.hpp"

namespace thinger::asio {

    // The result of `operation`, or nothing if it did not complete within `timeout`: it is then
    // cancelled (per-operation cancellation, as the awaitable operators do, see socket.hpp).
    // Both the operation and the timer end before this returns, so nothing outlives the call
    // (unlike a timer detached from the caller, which may still run once the caller is gone).
    // The timer runs on the executor of the caller
    template<typename T>
    awaitable<std::optional<T>> with_timeout(awaitable<T> operation, std::chrono::steady_clock::duration timeout) {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, timeout);
        auto result = co_await (std::move(operation) || timer.async_wait(use_nothrow_awaitable));
        if (result.index() == 0) co_return std::optional<T>{std::get<0>(std::move(result))};
        co_return std::nullopt;
    }

}

#endif

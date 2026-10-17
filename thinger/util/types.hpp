#ifndef THINGER_TYPES
#define THINGER_TYPES

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

namespace thinger {

    // Awaitable type alias
    template<typename T = void>
    using awaitable = boost::asio::awaitable<T>;

    // Import commonly used awaitable utilities
    using boost::asio::use_awaitable;
    using boost::asio::co_spawn;
    using boost::asio::detached;

    // For error handling without exceptions (returns tuple<error_code, result>)
    constexpr auto use_nothrow_awaitable =
        boost::asio::as_tuple(boost::asio::use_awaitable);

    // Result type for socket read/write operations (error_code + bytes transferred)
    using io_result = std::tuple<boost::system::error_code, size_t>;

    // Awaitable operators for combining coroutines (||, &&)
    using namespace boost::asio::experimental::awaitable_operators;

    // Whether T is what a coroutine returns
    template<typename T>
    inline constexpr bool is_awaitable_v = false;

    template<typename T, typename Executor>
    inline constexpr bool is_awaitable_v<boost::asio::awaitable<T, Executor>> = true;

    // A callable that is not a coroutine when called with Args. A std::function<void(...)>
    // takes any callable, coroutines too: it would call a coroutine lambda only to drop the
    // awaitable it returns, so the coroutine would never run
    template<typename F, typename... Args>
    concept not_a_coroutine = !is_awaitable_v<std::invoke_result_t<F&, Args...>>;

    // A coroutine taking Args (returning awaitable<void>)
    template<typename F, typename... Args>
    concept coroutine_callable = std::same_as<std::invoke_result_t<F&, Args...>, awaitable<void>>;

    // Call a callback that may be a coroutine, awaiting it if it is
    template<typename F, typename... Args>
    awaitable<void> call_and_await(F& callback, Args&&... args) {
        if constexpr (is_awaitable_v<std::invoke_result_t<F&, Args...>>) {
            co_await std::invoke(callback, std::forward<Args>(args)...);
        } else {
            std::invoke(callback, std::forward<Args>(args)...);
        }
    }

    // `operation`, keeping `keeper` alive until it ends
    template<typename T>
    awaitable<T> kept_alive(std::shared_ptr<void> keeper, awaitable<T> operation) {
        co_return co_await std::move(operation);
    }

    // Run `operation` on `executor` (e.g. the strand of the object it uses), resuming the
    // caller where it was: at once if the caller already runs there. The operation keeps
    // `keeper` (the object it uses) alive, also if the caller goes away
    template<typename T>
    awaitable<T> on_executor(boost::asio::any_io_executor executor, std::shared_ptr<void> keeper,
                             awaitable<T> operation) {
        if (executor == co_await boost::asio::this_coro::executor) co_return co_await std::move(operation);
        co_return co_await co_spawn(executor, kept_alive(std::move(keeper), std::move(operation)), use_awaitable);
    }

    // A std::function for callbacks that are called, never awaited: it does not take a
    // coroutine (see not_a_coroutine), which then fails to compile instead of never running
    template<typename Signature>
    class sync_callback;

    template<typename R, typename... Args>
    class sync_callback<R(Args...)> : public std::function<R(Args...)> {
    public:
        sync_callback() noexcept = default;
        sync_callback(std::nullptr_t) noexcept {}

        template<typename F>
            requires (!std::same_as<std::remove_cvref_t<F>, sync_callback>) &&
                     std::constructible_from<std::function<R(Args...)>, F> &&
                     not_a_coroutine<std::remove_cvref_t<F>, Args...>
        sync_callback(F&& callback) : std::function<R(Args...)>(std::forward<F>(callback)) {}
    };

}

#endif
#ifndef THINGER_TYPES
#define THINGER_TYPES

#include <functional>
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

    // Awaitable operators for combining coroutines (||, &&)
    using namespace boost::asio::experimental::awaitable_operators;

}

#endif
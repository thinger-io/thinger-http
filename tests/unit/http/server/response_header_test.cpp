// response.hpp alone: it brings in none of the connection types (it declares them), and its 8
// upgrade overloads compile with what it and upgrade.hpp declare. Its include comes first, so
// nothing else brings those headers in before the checks
#include <thinger/http/server/response.hpp>

#if defined(THINGER_SERVER_HTTP_SERVER_CONNECTION_HPP)
#error "response.hpp includes server_connection.hpp"
#endif
#if defined(HTTP_STREAM_HPP)
#error "response.hpp includes http_stream.hpp"
#endif
#if defined(THINGER_HTTP_SERVER_CONNECTION_TRACKER_HPP)
#error "response.hpp includes connection_tracker.hpp"
#endif
#if defined(THINGER_HTTP_SERVER_RESPONSE_SINK_HPP)
#error "response.hpp includes response_sink.hpp"
#endif
#if defined(THINGER_WEBSOCKET_CONNECTION_HPP)
#error "response.hpp includes websocket_connection.hpp"
#endif
#if defined(SSE_CONNECTION_HPP)
#error "response.hpp includes sse_connection.hpp"
#endif
#if defined(THINGER_ASIO_WEBSOCKET_HPP)
#error "response.hpp includes asio/sockets/websocket.hpp"
#endif

#include <catch2/catch_test_macros.hpp>
#include <memory>

using namespace thinger;

namespace {

    // Never run: it only has to compile
    [[maybe_unused]] void upgrade_overloads(http::response& res) {
        res.upgrade_websocket({}, [](std::shared_ptr<http::websocket_connection>) {});
        res.upgrade_websocket({.protocols = {"a", "b"}, .protocol_required = false},
                              [](std::shared_ptr<http::websocket_connection>) -> awaitable<void> { co_return; });
        res.upgrade_websocket_stream({}, [](http::upgraded_websocket upgraded) { upgraded.scope.closed(); });
        res.upgrade_websocket_stream({.binary = false, .ping_interval = std::chrono::seconds(30)},
                                     [](http::upgraded_websocket) -> awaitable<void> { co_return; });
        res.start_sse([](std::shared_ptr<http::sse_connection>) {});
        res.start_sse([](std::shared_ptr<http::sse_connection>) -> awaitable<void> { co_return; },
                      {.keepalive = std::chrono::seconds(5)});
        res.take_over([](http::upgraded_connection conn) { conn.scope.on_shutdown([] {}); });
        res.take_over([](http::upgraded_connection) -> awaitable<void> { co_return; });
    }

}

TEST_CASE("response.hpp declares its upgrades without the connection headers", "[response][upgrade][unit]") {
    // What the overloads above are handed, complete with response.hpp alone
    http::upgraded_connection conn;
    CHECK(conn.buffered.empty());
    CHECK_FALSE(conn.socket);
    http::upgraded_websocket upgraded;
    CHECK(upgraded.protocol.empty());
    CHECK_FALSE(upgraded.stream);
    http::websocket_accept_options options;
    CHECK(options.max_message_size == size_t{16} * 1024 * 1024);
    CHECK(http::sse_options{}.keepalive == std::chrono::seconds(15));
}

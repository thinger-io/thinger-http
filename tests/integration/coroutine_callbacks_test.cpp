#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/async_client.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/client/websocket_client.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/reverse_proxy.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/sse_connection.hpp>
#include <thinger/http/server/websocket_connection.hpp>
#include <thinger/asio/socket_pipe.hpp>
#include <boost/asio.hpp>
#include <atomic>
#include <future>

#include "server_test_helpers.hpp"

// Callbacks given as coroutines: run where the library can await them (middlewares, the
// handlers of upgrades, event streams and takeovers, the async client callbacks), and
// rejected at compile time where they would be called and their coroutine dropped

using namespace thinger;
using namespace std::chrono_literals;
using server_test::running_server;

namespace {

    // A coroutine that waits a little, so its work happens after a suspension
    awaitable<void> short_pause() {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 10ms);
        co_await timer.async_wait(use_nothrow_awaitable);
    }

    auto coroutine_with_no_args = []() -> awaitable<void> { co_return; };
    auto sync_with_no_args = []() {};

    template<typename F>
    concept websocket_on_message = requires(http::websocket_connection& ws, F f) { ws.on_message(f); };
    template<typename F>
    concept websocket_on_shutdown = requires(http::websocket_connection& ws, F f) { ws.on_shutdown(f); };
    template<typename F>
    concept sse_on_shutdown = requires(http::sse_connection& sse, F f) { sse.on_shutdown(f); };
    template<typename F>
    concept server_on_shutdown = requires(http::server& server, F f) { server.on_shutdown(f); };
    template<typename F>
    concept shutdown_done = requires(http::server& server, F f) { server.begin_shutdown(1s, f); };
    template<typename F>
    concept socket_shutdown = requires(asio::socket& socket, F f) { http::on_server_shutdown(socket, f); };
    template<typename F>
    concept pipe_end = requires(asio::socket_pipe& pipe, F f) { pipe.set_on_end(f); };
    template<typename F>
    concept formatter_callback = requires(F f) { http::error_formatter(f); };
    template<typename F>
    concept proxy_request_hook = requires(http::proxy_options& options, F f) { options.on_request = f; };
    template<typename F>
    concept proxy_response_hook = requires(http::proxy_options& options, F f) { options.on_response = f; };
    template<typename F>
    concept dispatch_callback = requires(http::server& server, boost::asio::any_io_executor executor, F f) {
        server.dispatch(executor, std::shared_ptr<http::http_request>{}, f);
    };
    template<typename F>
    concept client_message_callback = requires(http::websocket_client& ws, F f) { ws.on_message(f); };
    template<typename F>
    concept download_progress = requires(F f) { http::progress_callback(f); };
    template<typename F>
    concept sync_middleware = requires(http::server& server, F f) { server.use(f); };

}

TEST_CASE("Callbacks that are not awaited reject coroutines at compile time", "[coroutine][callbacks]") {
    auto on_message = [](std::string, bool) -> awaitable<void> { co_return; };
    auto on_message_sync = [](std::string, bool) {};
    static_assert(!websocket_on_message<decltype(on_message)>);
    static_assert(websocket_on_message<decltype(on_message_sync)>);

    static_assert(!websocket_on_shutdown<decltype(coroutine_with_no_args)>);
    static_assert(websocket_on_shutdown<decltype(sync_with_no_args)>);
    static_assert(!sse_on_shutdown<decltype(coroutine_with_no_args)>);
    static_assert(sse_on_shutdown<decltype(sync_with_no_args)>);
    static_assert(!server_on_shutdown<decltype(coroutine_with_no_args)>);
    static_assert(server_on_shutdown<decltype(sync_with_no_args)>);
    static_assert(!socket_shutdown<decltype(coroutine_with_no_args)>);
    static_assert(socket_shutdown<decltype(sync_with_no_args)>);
    static_assert(!pipe_end<decltype(coroutine_with_no_args)>);
    static_assert(pipe_end<decltype(sync_with_no_args)>);

    auto done = [](bool) -> awaitable<void> { co_return; };
    auto done_sync = [](bool) {};
    static_assert(!shutdown_done<decltype(done)>);
    static_assert(shutdown_done<decltype(done_sync)>);

    auto formatter = [](const http::http_error&, http::http_response&) -> awaitable<void> { co_return; };
    auto formatter_sync = [](const http::http_error&, http::http_response&) {};
    static_assert(!formatter_callback<decltype(formatter)>);
    static_assert(formatter_callback<decltype(formatter_sync)>);

    auto on_request = [](http::request&, http::http_request&) -> awaitable<void> { co_return; };
    auto on_request_sync = [](http::request&, http::http_request&) {};
    static_assert(!proxy_request_hook<decltype(on_request)>);
    static_assert(proxy_request_hook<decltype(on_request_sync)>);
    auto on_response = [](http::request&, http::http_response&) -> awaitable<void> { co_return; };
    auto on_response_sync = [](http::request&, http::http_response&) {};
    static_assert(!proxy_response_hook<decltype(on_response)>);
    static_assert(proxy_response_hook<decltype(on_response_sync)>);

    auto dispatched = [](std::shared_ptr<http::http_response>) -> awaitable<void> { co_return; };
    auto dispatched_sync = [](std::shared_ptr<http::http_response>) {};
    static_assert(!dispatch_callback<decltype(dispatched)>);
    static_assert(dispatch_callback<decltype(dispatched_sync)>);

    auto client_message = [](const std::string&, bool) -> awaitable<void> { co_return; };
    auto client_message_sync = [](const std::string&, bool) {};
    static_assert(!client_message_callback<decltype(client_message)>);
    static_assert(client_message_callback<decltype(client_message_sync)>);

    auto progress = [](size_t, size_t) -> awaitable<void> { co_return; };
    auto progress_sync = [](size_t, size_t) {};
    static_assert(!download_progress<decltype(progress)>);
    static_assert(download_progress<decltype(progress_sync)>);

    // Middlewares with next(): both kinds are accepted (and the coroutine one is run)
    auto middleware = [](http::request&, http::response&, std::function<void()>) -> awaitable<void> { co_return; };
    auto middleware_sync = [](http::request&, http::response&, std::function<void()>) {};
    static_assert(sync_middleware<decltype(middleware)>);
    static_assert(sync_middleware<decltype(middleware_sync)>);

    SUCCEED();
}

TEST_CASE("Coroutine middlewares with next() run", "[coroutine][middleware][integration]") {
    http::server server;
    server.use([](http::request& req, http::response& res, std::function<void()> next) -> awaitable<void> {
        co_await short_pause();
        if (req.get_http_request()->get_path() == "/blocked") {
            res.error(http::http_response::status::forbidden, "blocked");
            co_return;
        }
        res.header("X-Middleware", "coroutine");
        next();
    });
    server.get("/open", [](http::response& res) { res.send("open"); });
    server.get("/blocked", [](http::response& res) { res.send("reached"); });
    running_server running(server);

    http::client client;
    auto base = "http://127.0.0.1:" + std::to_string(running.port);
    auto open = client.get(base + "/open");
    REQUIRE(open.status() == 200);
    REQUIRE(open.body() == "open");
    REQUIRE(open.header("X-Middleware") == "coroutine");

    auto blocked = client.get(base + "/blocked");
    REQUIRE(blocked.status() == 403);
    REQUIRE(blocked.body() != "reached");
}

TEST_CASE("Coroutine WebSocket handlers run before the WebSocket starts", "[coroutine][websocket][integration]") {
    http::server server;
    server.get("/ws", [](http::response& res) {
        res.upgrade_websocket([](std::shared_ptr<http::websocket_connection> ws) -> awaitable<void> {
            co_await short_pause();
            ws->on_message([ws](std::string message, bool) { ws->send_text("echo: " + message); });
        });
    });
    server.get("/stream", [](http::response& res) {
        res.upgrade_websocket_stream([](std::shared_ptr<asio::websocket> ws, std::string) -> awaitable<void> {
            uint8_t buffer[256];
            auto [ec, bytes] = co_await ws->read_some(buffer, sizeof(buffer));
            if (ec) co_return;
            co_await short_pause();
            co_await ws->write(std::string("stream: ") + std::string(reinterpret_cast<char*>(buffer), bytes));
        });
    });
    running_server running(server);
    auto base = "ws://127.0.0.1:" + std::to_string(running.port);

    http::client client;
    SECTION("Message interface") {
        auto ws = client.websocket(base + "/ws");
        REQUIRE(ws);
        REQUIRE(ws->send_text("hello"));
        auto [message, binary] = ws->receive();
        REQUIRE(message == "echo: hello");
        ws->close();
    }
    SECTION("As a socket") {
        auto ws = client.websocket(base + "/stream");
        REQUIRE(ws);
        REQUIRE(ws->send_binary("hello"));
        auto [message, binary] = ws->receive();
        REQUIRE(message == "stream: hello");
        ws->close();
    }
}

TEST_CASE("Coroutine SSE and takeover handlers run", "[coroutine][sse][takeover][integration]") {
    http::server server;
    server.get("/events", [](http::response& res) {
        res.start_sse([](std::shared_ptr<http::sse_connection> sse) -> awaitable<void> {
            for (int i = 0; i < 3; ++i) {
                co_await short_pause();
                sse->send_data("event " + std::to_string(i));
            }
        });
    });
    server.get("/takeover", [](http::response& res) {
        res.status(http::http_response::status::switching_protocols);
        res.header("Upgrade", "echo");
        res.header("Connection", "Upgrade");
        res.take_over([](std::shared_ptr<asio::socket> socket, std::string buffered) -> awaitable<void> {
            co_await short_pause();
            co_await socket->write("taken over, buffered: " + buffered);
            socket->close();
        });
    });
    running_server running(server);

    SECTION("Event stream") {
        auto result = server_test::raw_exchange(running.port,
            "GET /events HTTP/1.1\r\nHost: x\r\nAccept: text/event-stream\r\n\r\n", 1s);
        REQUIRE(result.data.starts_with("HTTP/1.1 200"));
        REQUIRE(result.data.find("data: event 0") != std::string::npos);
        REQUIRE(result.data.find("data: event 2") != std::string::npos);
    }
    SECTION("Takeover") {
        auto result = server_test::raw_exchange(running.port,
            "GET /takeover HTTP/1.1\r\nHost: x\r\nUpgrade: echo\r\nConnection: Upgrade\r\n\r\nearly");
        REQUIRE(result.data.starts_with("HTTP/1.1 101"));
        REQUIRE(result.data.find("taken over, buffered: early") != std::string::npos);
        REQUIRE(result.closed);
    }
}

TEST_CASE("Coroutine callbacks of the async client are awaited", "[coroutine][client][integration]") {
    http::server server;
    server.get("/hello", [](http::response& res) { res.send("hello"); });
    running_server running(server);
    auto url = "http://127.0.0.1:" + std::to_string(running.port) + "/hello";

    http::async_client client;
    std::promise<std::string> body;
    client.get(url, [&body](http::client_response& res) -> awaitable<void> {
        co_await short_pause();
        body.set_value(std::string(res.body()));
    });
    auto future = body.get_future();
    REQUIRE(future.wait_for(5s) == std::future_status::ready);
    REQUIRE(future.get() == "hello");

    std::promise<int> status;
    client.request(url).get([&status](http::client_response& res) -> awaitable<void> {
        co_await short_pause();
        status.set_value(res.status());
    });
    auto status_future = status.get_future();
    REQUIRE(status_future.wait_for(5s) == std::future_status::ready);
    REQUIRE(status_future.get() == 200);
    client.wait();
}

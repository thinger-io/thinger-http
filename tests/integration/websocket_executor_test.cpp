#include <catch2/catch_test_macros.hpp>
#include <thinger/asio/sockets/tcp_socket.hpp>
#include <thinger/asio/sockets/websocket.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/client/executor_client.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/websocket_connection.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include "server_test_helpers.hpp"

#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef THINGER_HTTP_SSL_ENABLED
#include <openssl/rand.h>
#endif

// A WebSocket runs its own work (the keepalive, the control frames it queues, the closing
// handshake) on the executor it was created with: on a strand of an io_context run by several
// threads, never at the same time as its user. Run under TSan, these tests report the race of
// a websocket that spawns that work on its io_context instead

using namespace thinger;
using namespace std::chrono_literals;
using namespace boost::asio::experimental::awaitable_operators;
using server_test::running_server;
using tcp = boost::asio::ip::tcp;

namespace {

    constexpr int THREADS = 4;
    constexpr auto PING_INTERVAL = 10ms;
    constexpr auto IDLE_TIMEOUT = 5s;
    constexpr auto TRAFFIC = 300ms;
    constexpr size_t MESSAGE_SIZE = 512;

    void run_threads(boost::asio::io_context& context) {
        // The first random bytes of OpenSSL (the mask keys of the client frames) set up its
        // generator, which TSan, without OpenSSL instrumented, takes for a race with the first
        // ones taken on another thread: taken here first, before the threads
#ifdef THINGER_HTTP_SSL_ENABLED
        uint8_t random[4];
        REQUIRE(RAND_bytes(random, sizeof(random)) == 1);
#endif
        std::vector<std::thread> threads;
        for (int t = 0; t < THREADS; ++t) threads.emplace_back([&] { context.run_for(20s); });
        for (auto& thread : threads) thread.join();
    }

    awaitable<void> sleep_for(std::chrono::milliseconds duration) {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, duration);
        co_await timer.async_wait(use_nothrow_awaitable);
    }

    // What one end exchanged
    struct traffic {
        size_t written = 0;
        size_t read = 0;
        bool closed_clean = false;
    };

    // Write messages for TRAFFIC, with pauses longer than the ping interval so the keepalive
    // pings in between, while reading the echo of all of them; then close
    awaitable<void> exchange(std::shared_ptr<asio::websocket> ws, traffic& result) {
        std::string message(MESSAGE_SIZE, 'x');
        bool writing = true;
        auto writer = [&]() -> awaitable<void> {
            auto end = std::chrono::steady_clock::now() + TRAFFIC;
            for (int i = 0; std::chrono::steady_clock::now() < end; ++i) {
                auto [ec, bytes] = co_await ws->write(std::string_view(message));
                if (ec) break;
                result.written += bytes;
                if (i % 8 == 7) co_await sleep_for(PING_INTERVAL + 5ms);
            }
            writing = false;
            // Everything echoed already: the reader waits for nothing more (on the same strand)
            if (result.read == result.written) ws->cancel();
        };
        auto reader = [&]() -> awaitable<void> {
            uint8_t buffer[4096];
            while (writing || result.read < result.written) {
                auto [ec, bytes] = co_await ws->read_some(buffer, sizeof(buffer));
                if (ec) co_return;
                result.read += bytes;
            }
        };
        co_await (writer() && reader());
        // the closing handshake goes on by itself, on the executor of the websocket
        ws->close();
        result.closed_clean = result.written > 0 && result.read == result.written;
    }

    // Echo whatever is read until the peer closes
    awaitable<void> echo(std::shared_ptr<asio::websocket> ws) {
        uint8_t buffer[4096];
        for (;;) {
            auto [ec, bytes] = co_await ws->read_some(buffer, sizeof(buffer));
            if (ec) co_return;
            auto [write_ec, written] = co_await ws->write(buffer, bytes);
            if (write_ec) co_return;
        }
    }

}

TEST_CASE("Client WebSockets on strands of an io_context run by several threads",
          "[websocket][executor][threading][integration]") {
    http::server server;
    server.get("/echo", [](http::request&, http::response& res) {
        http::websocket_options options;
        options.ping_interval = PING_INTERVAL;
        options.idle_timeout = IDLE_TIMEOUT;
        res.upgrade_websocket_stream(options, [](std::shared_ptr<asio::websocket> ws, std::string) -> awaitable<void> {
            co_await echo(std::move(ws));
        });
    });
    running_server running(server);
    auto url = "ws://127.0.0.1:" + std::to_string(running.port) + "/echo";

    boost::asio::io_context context;
    constexpr int sockets = 4;
    std::vector<traffic> results(sockets);
    std::atomic<int> on_strand{0};

    for (auto& result : results) {
        auto strand = boost::asio::make_strand(context);
        co_spawn(strand, [&, strand]() -> awaitable<void> {
            auto client = co_await http::executor_client::for_current_coroutine();
            auto ws = co_await client->websocket(url);
            if (!ws) co_return;
            auto socket = ws->release_socket();
            if (socket->get_executor() == boost::asio::any_io_executor(strand)) ++on_strand;
            socket->set_binary(true);
            socket->set_keepalive(PING_INTERVAL, IDLE_TIMEOUT);
            co_await exchange(std::move(socket), result);
        }, detached);
    }
    run_threads(context);

    CHECK(on_strand == sockets);
    for (const auto& result : results) {
        CHECK(result.closed_clean);
    }
}

TEST_CASE("Server and client WebSockets on strands of an io_context run by several threads",
          "[websocket][executor][threading][integration]") {
    boost::asio::io_context context;
    constexpr int pairs = 4;
    std::vector<traffic> results(pairs);
    std::atomic<int> echoes_ended{0};

    tcp::acceptor acceptor(context, {boost::asio::ip::make_address("127.0.0.1"), 0});
    for (auto& result : results) {
        tcp::socket client_end(context);
        client_end.connect(acceptor.local_endpoint());
        auto server_end = acceptor.accept();

        auto server_strand = boost::asio::make_strand(context);
        co_spawn(server_strand, [&, server_strand, socket = std::move(server_end)]() mutable -> awaitable<void> {
            auto tcp_end = std::make_shared<asio::tcp_socket>("ws", std::move(socket));
            auto ws = std::make_shared<asio::websocket>(tcp_end, true, true, server_strand);
            ws->set_keepalive(PING_INTERVAL, IDLE_TIMEOUT);
            co_await echo(std::move(ws));
            ++echoes_ended;
        }, detached);

        auto client_strand = boost::asio::make_strand(context);
        co_spawn(client_strand, [&, client_strand, socket = std::move(client_end)]() mutable -> awaitable<void> {
            auto tcp_end = std::make_shared<asio::tcp_socket>("ws", std::move(socket));
            auto ws = std::make_shared<asio::websocket>(tcp_end, true, false, client_strand);
            ws->set_keepalive(PING_INTERVAL, IDLE_TIMEOUT);
            co_await exchange(std::move(ws), result);
        }, detached);
    }
    acceptor.close();
    run_threads(context);

    CHECK(echoes_ended == pairs);
    for (const auto& result : results) {
        CHECK(result.closed_clean);
    }
}

TEST_CASE("A websocket_connection on a strand of an io_context run by several threads",
          "[websocket][executor][threading][integration]") {
    boost::asio::io_context context;
    constexpr int pairs = 4;
    std::vector<traffic> results(pairs);

    tcp::acceptor acceptor(context, {boost::asio::ip::make_address("127.0.0.1"), 0});
    for (auto& result : results) {
        tcp::socket client_end(context);
        client_end.connect(acceptor.local_endpoint());
        auto server_end = acceptor.accept();

        // Its loops, timers and calls run on the strand of its WebSocket
        auto server_strand = boost::asio::make_strand(context);
        boost::asio::post(server_strand, [server_strand, socket = std::move(server_end)]() mutable {
            auto tcp_end = std::make_shared<asio::tcp_socket>("ws", std::move(socket));
            auto ws = std::make_shared<asio::websocket>(tcp_end, true, true, server_strand);
            auto connection = std::make_shared<http::websocket_connection>(ws);
            connection->set_write_limits({.max_messages = 0});   // the echo drops nothing
            std::weak_ptr<http::websocket_connection> weak = connection;
            connection->on_message([weak](std::string message, bool) {
                if (auto self = weak.lock()) self->send_binary(std::move(message));
            });
            connection->start();
            ws->set_keepalive(PING_INTERVAL, IDLE_TIMEOUT);
        });

        auto client_strand = boost::asio::make_strand(context);
        co_spawn(client_strand, [&, client_strand, socket = std::move(client_end)]() mutable -> awaitable<void> {
            auto tcp_end = std::make_shared<asio::tcp_socket>("ws", std::move(socket));
            auto ws = std::make_shared<asio::websocket>(tcp_end, true, false, client_strand);
            ws->set_keepalive(PING_INTERVAL, IDLE_TIMEOUT);
            co_await exchange(std::move(ws), result);
        }, detached);
    }
    acceptor.close();
    run_threads(context);

    for (const auto& result : results) {
        CHECK(result.closed_clean);
    }
}

TEST_CASE("A WebSocket runs on the executor of its creator", "[websocket][executor][integration]") {
    http::server server;
    server.get("/echo", [](http::request&, http::response& res) {
        res.upgrade_websocket_stream({}, [](std::shared_ptr<asio::websocket> ws, std::string) -> awaitable<void> {
            co_await echo(std::move(ws));
        });
    });
    running_server running(server);
    auto url = "ws://127.0.0.1:" + std::to_string(running.port) + "/echo";

    SECTION("By default, the io_context of its socket") {
        boost::asio::io_context context;
        auto tcp_end = std::make_shared<asio::tcp_socket>("ws", context);
        auto ws = std::make_shared<asio::websocket>(tcp_end);
        CHECK(ws->get_executor() == boost::asio::any_io_executor(context.get_executor()));
    }

    SECTION("The synchronous client: its io_context") {
        http::client client;
        auto ws = client.websocket(url);
        REQUIRE(ws);
        auto socket = ws->release_socket();
        CHECK(socket->get_executor() == boost::asio::any_io_executor(socket->get_io_context().get_executor()));
        socket->close();
    }
}

TEST_CASE("The synchronous calls of a client WebSocket fail on its strand", "[websocket][executor][integration]") {
    http::server server;
    server.get("/echo", [](http::request&, http::response& res) {
        res.upgrade_websocket_stream({}, [](std::shared_ptr<asio::websocket> ws, std::string) -> awaitable<void> {
            co_await echo(std::move(ws));
        });
    });
    running_server running(server);
    auto url = "ws://127.0.0.1:" + std::to_string(running.port) + "/echo";

    // They would wait running the io_context, which cannot run the strand from within it
    boost::asio::io_context context;
    bool opened = false;
    int thrown = 0;
    std::string echoed;
    co_spawn(boost::asio::make_strand(context), [&]() -> awaitable<void> {
        auto client = co_await http::executor_client::for_current_coroutine();
        auto ws = co_await client->websocket(url);
        if (!ws) co_return;
        opened = true;
        try { ws->send_text("lost"); } catch (const std::logic_error&) { ++thrown; }
        try { ws->send_binary(std::string_view("lost")); } catch (const std::logic_error&) { ++thrown; }
        try { ws->receive(); } catch (const std::logic_error&) { ++thrown; }
        try { ws->close(); } catch (const std::logic_error&) { ++thrown; }

        // The coroutine API works there, on the connection still open
        if (co_await ws->send_text_async("hello")) echoed = (co_await ws->receive_async()).first;
        co_await ws->close_async();
    }, detached);
    context.run_for(10s);

    REQUIRE(opened);
    CHECK(thrown == 4);
    CHECK(echoed == "hello");
}

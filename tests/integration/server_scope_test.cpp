#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <thinger/http/server/connection_tracker.hpp>
#include <thinger/http/server/pool_server.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/sse_connection.hpp>
#include <thinger/http/server/websocket_connection.hpp>
#include <thinger/asio/sockets/websocket.hpp>

#include "server_test_helpers.hpp"
#include "shutdown_test_helpers.hpp"
#include "../fixtures/awaited_shutdown.hpp"
#include "../fixtures/running_workers.hpp"
#include "../fixtures/server_listen.hpp"
#include "../fixtures/stream_test_helpers.hpp"

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

// The server_scope of the connections taken over from a server (take_over(), a WebSocket as a
// byte stream or with the message interface, an event stream): told when the drain starts,
// counted by type while open and no longer once closed, closed at the end of the grace period;
// and a scope with no server, or whose server is gone, that does nothing

using namespace thinger;
using namespace std::chrono_literals;
using server_test::running_server;
using http::test::awaited_shutdown;
using shutdown_test::eventually;
using shutdown_test::raw_client;

namespace {

    using shutdown_test::clock;   // here, as ::clock is the C function

    // The paths of the routes upgrading, one per kind of upgrade
    const std::string take_over_path = "/raw";
    const std::string stream_path = "/stream";
    const std::string messages_path = "/messages";
    const std::string events_path = "/events";

    bool is_websocket(const std::string& path) { return path == stream_path || path == messages_path; }

    // The count of the type `path` upgrades to
    uint64_t typed(const http::server_stats& stats, const std::string& path) {
        if (path == take_over_path) return stats.taken_over_connections;
        if (path == events_path) return stats.sse_connections;
        return stats.websocket_connections;
    }

    // What the handlers upgraded, and what the server told them. The connections hold
    // themselves: a socket and a byte stream while read (see hold_reading()), a WebSocket and an
    // event stream while open
    struct upgraded_state {
        std::mutex mutex;
        boost::asio::any_io_executor executor;    // of the connection
        http::server_scope scope;                 // of the socket or byte stream
        std::weak_ptr<asio::socket> socket;       // the socket or byte stream
        std::weak_ptr<http::websocket_connection> websocket;
        std::weak_ptr<http::sse_connection> sse;
        // Whether a handler registers on_shutdown(), and closes when told
        bool register_shutdown = true;
        bool close_when_told = true;
        std::atomic<bool> upgraded{false};
        std::atomic<int> told{0};
        clock::time_point told_at;

        clock::time_point told_time() {
            std::scoped_lock lock(mutex);
            return told_at;
        }

        // Run `callback` on the thread of the connection, and wait for it
        void on_thread(std::function<void()> callback) {
            boost::asio::any_io_executor on;
            {
                std::scoped_lock lock(mutex);
                on = executor;
            }
            REQUIRE(on);
            std::promise<void> ran;
            boost::asio::post(on, [&] {
                callback();
                ran.set_value();
            });
            REQUIRE(ran.get_future().wait_for(3s) == std::future_status::ready);
        }

        // What the server shuts down calls: closes the connection, if close_when_told
        void notified() {
            {
                std::scoped_lock lock(mutex);
                told_at = clock::now();
            }
            ++told;
            if (close_when_told) close();
        }

        // On the thread of the connection: close it, as its owner does. A byte stream is
        // closed for the server once its closing handshake ends, as websocket_connection does:
        // the server may stop as soon as nothing counts, and a server stopping its own
        // io_context leaves the rest of a handshake that close() started undone (on_close()
        // is told when it starts), the connection open while the server lives
        void close() {
            if (auto ws = websocket.lock()) return ws->stop();
            if (auto events = sse.lock()) return events->stop();
            auto open = socket.lock();
            if (auto stream = std::dynamic_pointer_cast<asio::websocket>(open)) {
                co_spawn(stream->get_executor(), [stream, scope = scope]() -> thinger::awaitable<void> {
                    co_await stream->close_graceful(asio::websocket::CLOSE_GOING_AWAY);
                    scope.closed();
                }, boost::asio::detached);
                return;
            }
            scope.closed();
            if (open) open->close();
        }

        // On the thread of the connection: the socket or byte stream is closed for the server
        // (scope.closed()), but kept open
        void closed_for_the_server() {
            if (auto ws = websocket.lock()) return ws->stop();
            if (auto events = sse.lock()) return events->stop();
            scope.closed();
        }
    };

    // The socket lives while it is read, until the peer closes it (or it is closed)
    void hold_reading(const boost::asio::any_io_executor& executor, std::shared_ptr<asio::socket> socket) {
        co_spawn(executor, [socket = std::move(socket)]() -> thinger::awaitable<void> {
            uint8_t buffer[64];
            while (true) {
                auto [ec, bytes] = co_await socket->read_some(buffer, sizeof(buffer));
                if (ec) break;
            }
        }, boost::asio::detached);
    }

    // A socket or byte stream handed out with its scope: kept while read (unless !keep)
    void keep(upgraded_state& state, const boost::asio::any_io_executor& executor,
              std::shared_ptr<asio::socket> socket, http::server_scope scope, bool keep = true) {
        {
            std::scoped_lock lock(state.mutex);
            state.executor = executor;
            state.scope = scope;
            state.socket = socket;
        }
        if (state.register_shutdown) scope.on_shutdown([&state] { state.notified(); });
        if (keep) hold_reading(executor, std::move(socket));
        state.upgraded = true;
    }

    // A route per kind of upgrade, keeping what they upgrade in `state` (and the sockets and
    // byte streams while read, unless !keep_sockets)
    void upgrade_routes(http::server& server, upgraded_state& state, bool keep_sockets = true) {
        server.get(take_over_path, [&state, keep_sockets](http::response& res) {
            res.take_over([&state, keep_sockets](http::upgraded_connection conn) {
                auto executor = boost::asio::any_io_executor(conn.socket->get_io_context().get_executor());
                keep(state, executor, std::move(conn.socket), conn.scope, keep_sockets);
            });
        });
        server.get(stream_path, [&state, keep_sockets](http::response& res) {
            res.upgrade_websocket_stream({}, [&state, keep_sockets](http::upgraded_websocket upgraded) {
                auto executor = upgraded.stream->get_executor();
                keep(state, executor, std::move(upgraded.stream), upgraded.scope, keep_sockets);
            });
        });
        server.get(messages_path, [&state](http::response& res) {
            res.upgrade_websocket({}, [&state](std::shared_ptr<http::websocket_connection> ws) {
                {
                    std::scoped_lock lock(state.mutex);
                    state.executor = ws->get_executor();
                    state.websocket = ws;
                }
                ws->on_message([](std::string, bool) {});
                if (state.register_shutdown) ws->on_shutdown([&state] { state.notified(); });
                state.upgraded = true;
            });
        });
        server.get(events_path, [&state](http::response& res) {
            res.start_sse([&state](std::shared_ptr<http::sse_connection> sse) {
                {
                    std::scoped_lock lock(state.mutex);
                    state.executor = sse->get_executor();
                    state.sse = sse;
                }
                if (state.register_shutdown) sse->on_shutdown([&state] { state.notified(); });
                sse->send_data("hello");
                state.upgraded = true;
            });
        });
    }

    // The client of an upgrade: a raw WebSocket client, or a raw HTTP one
    struct upgrade_client {
        std::optional<stream_test::raw_ws_client> ws;
        std::optional<raw_client> http;

        upgrade_client(const server_test::transport_endpoint& to, const std::string& path) {
            if (is_websocket(path)) {
                ws.emplace(to);
                REQUIRE(ws->handshake(path).starts_with("HTTP/1.1 101"));
                return;
            }
            http.emplace(to);
            http->get(path);
            REQUIRE(http->read_until(path == events_path ? "data: hello" : "\r\n\r\n"));
        }

        // Whether the server closes the connection within the timeout (a WebSocket close frame
        // received is answered, as a client does)
        bool closed_within(std::chrono::milliseconds timeout) {
            if (!ws) return http->rest(timeout).closed;
            auto deadline = clock::now() + timeout;
            while (auto frame = ws->read_frame(timeout)) {
                if (frame->opcode == stream_test::OP_CLOSE) {
                    ws->send_frame(stream_test::OP_CLOSE, frame->payload.substr(0, 2));
                    break;
                }
                if (clock::now() >= deadline) break;
            }
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now());
            return ws->closed_within(std::max(left, 1ms));
        }
    };

}

TEST_CASE("A connection taken over is told of the shutdown through its scope when the drain starts",
          "[server][shutdown][scope][websocket][sse][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    auto path = GENERATE(take_over_path, stream_path, messages_path, events_path);
    INFO("upgrade " << path);
    upgraded_state state;
    http::server server;
    upgrade_routes(server, state);
    running_server running(server, kind);

    upgrade_client client(running.endpoint(), path);
    REQUIRE(eventually([&] { return state.upgraded.load(); }));
    // Counted as what it serves while open, and no longer as HTTP
    REQUIRE(eventually([&] { return typed(server.stats(), path) == 1; }));
    auto open = server.stats();
    CHECK(open.active_connections() == 1);
    CHECK(open.http_connections == 0);

    // Told once the drain starts, well before the end of the grace period
    auto begin = clock::now();
    awaited_shutdown shutdown(server, 5s);
    REQUIRE(eventually([&] { return state.told.load() == 1; }, 2s));
    CHECK(state.told_time() - begin < 1s);

    // It closes when told: the shutdown ends clean, before its deadline
    REQUIRE(client.closed_within(3s));
    REQUIRE(shutdown.ended_within(3s));
    REQUIRE(shutdown.get());
    CHECK(clock::now() - begin < 4s);
    CHECK(state.told.load() == 1);
    CHECK(server.stats().active_connections() == 0);
}

TEST_CASE("A connection taken over no longer counts once closed, and is not told of a later shutdown",
          "[server][shutdown][scope][websocket][sse][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    auto path = GENERATE(take_over_path, stream_path, messages_path, events_path);
    INFO("upgrade " << path);
    upgraded_state state;
    http::server server;
    upgrade_routes(server, state);
    running_server running(server, kind);
    auto before = server.stats();

    upgrade_client client(running.endpoint(), path);
    REQUIRE(eventually([&] { return state.upgraded.load(); }));
    REQUIRE(eventually([&] { return typed(server.stats(), path) == 1; }));

    // Closed by its owner: a socket or byte stream through scope.closed() (twice: the second
    // does nothing), still open; a WebSocket or an event stream with stop()
    state.on_thread([&] {
        state.closed_for_the_server();
        state.scope.closed();
    });
    bool kept_open = path == take_over_path || path == stream_path;
    if (!kept_open) REQUIRE(client.closed_within(3s));
    REQUIRE(eventually([&] { return typed(server.stats(), path) == typed(before, path); }));
    auto after = server.stats();
    CHECK(after.active_connections() == before.active_connections());
    CHECK(after.total_connections == before.total_connections + 1);

    // A shutdown afterwards: nothing left to drain, nor to tell; a socket only closed for the
    // server stays open (until the server, stopped, destroys what its io_context holds)
    REQUIRE(server.shutdown(5s));
    CHECK(state.told.load() == 0);
    if (kept_open) CHECK_FALSE(client.closed_within(200ms));
}

TEST_CASE("A connection taken over that sets on_shutdown() once the drain started is told once",
          "[server][shutdown][scope][websocket][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    auto path = GENERATE(take_over_path, stream_path);
    INFO("upgrade " << path);
    upgraded_state state;
    state.register_shutdown = false;
    http::server server;
    upgrade_routes(server, state);
    running_server running(server, kind);

    upgrade_client client(running.endpoint(), path);
    REQUIRE(eventually([&] { return state.upgraded.load(); }));
    REQUIRE(eventually([&] { return typed(server.stats(), path) == 1; }));

    awaited_shutdown shutdown(server, 5s);
    // the drain was posted to the connection when the shutdown started: it ran meanwhile
    std::this_thread::sleep_for(100ms);
    REQUIRE(state.told.load() == 0);
    state.on_thread([&] { state.scope.on_shutdown([&state] { state.notified(); }); });

    // Told once, posted
    REQUIRE(eventually([&] { return state.told.load() == 1; }, 2s));
    REQUIRE(client.closed_within(3s));
    REQUIRE(shutdown.ended_within(3s));
    REQUIRE(shutdown.get());
    std::this_thread::sleep_for(50ms);
    CHECK(state.told.load() == 1);
}

TEST_CASE("A takeover while the server closes its connections closes the connection without calling the handler",
          "[server][shutdown][scope][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    std::atomic<bool> called{false};
    std::atomic<bool> taking_over{false};
    std::weak_ptr<int> handler_alive;
    {
        http::server server;
        // A handler taking over once interrupted at the deadline of the shutdown, when the server
        // closes what is left. The close of its connection was posted before the handler
        // resumes, so the takeover finds it closed (the guard of a takeover completing in the
        // closing phase covers a race no test orders)
        server.get("/late", [&](http::response& res) -> thinger::awaitable<void> {
            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 30s);
            co_await timer.async_wait(thinger::use_nothrow_awaitable);
            auto sentinel = std::make_shared<int>(0);
            handler_alive = sentinel;
            taking_over = true;
            res.take_over([&called, sentinel = std::move(sentinel)](http::upgraded_connection) { called = true; });
        });
        running_server running(server, kind);

        raw_client client(running.endpoint());
        client.get("/late");
        REQUIRE(eventually([&] { return server.stats().http_connections == 1; }));
        std::this_thread::sleep_for(50ms);

        REQUIRE_FALSE(server.shutdown(200ms));
        REQUIRE(eventually([&] { return taking_over.load(); }));
        // The connection is closed without calling the handler
        CHECK(client.rest(2s).closed);
        CHECK(eventually([&] { return server.stats().active_connections() == 0; }));
        CHECK_FALSE(called.load());
    }
    // and the handler released (by then, or with what the stopped server still held)
    CHECK(handler_alive.expired());
    CHECK_FALSE(called.load());
}

TEST_CASE("A socket taken over released without scope.closed() no longer counts",
          "[server][scope][websocket][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    auto path = GENERATE(take_over_path, stream_path);
    INFO("upgrade " << path);
    upgraded_state state;
    http::server server;
    // The handlers keep nothing: the socket handed out is released when they return
    upgrade_routes(server, state, false);
    running_server running(server, kind);

    upgrade_client client(running.endpoint(), path);
    REQUIRE(eventually([&] { return state.upgraded.load(); }));
    REQUIRE(client.closed_within(3s));
    REQUIRE(eventually([&] { return server.stats().active_connections() == 0; }));
    CHECK(typed(server.stats(), path) == 0);
    REQUIRE(server.shutdown(5s));
    CHECK(state.told.load() == 0);
}

TEST_CASE("Shutdown closes a socket taken over still open at the end of its grace period",
          "[server][shutdown][scope][websocket][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    auto path = GENERATE(take_over_path, stream_path);
    auto told = GENERATE(false, true);
    INFO("upgrade " << path << (told ? ", told and ignoring it" : ", not asking to be told"));
    upgraded_state state;
    state.register_shutdown = told;
    state.close_when_told = false;
    http::server server;
    upgrade_routes(server, state);
    running_server running(server, kind);

    upgrade_client client(running.endpoint(), path);
    REQUIRE(eventually([&] { return state.upgraded.load(); }));
    REQUIRE(eventually([&] { return typed(server.stats(), path) == 1; }));

    auto begin = clock::now();
    REQUIRE_FALSE(server.shutdown(300ms));
    auto elapsed = clock::now() - begin;
    CHECK(elapsed >= 300ms);
    CHECK(elapsed < 1s);
    CHECK(state.told.load() == (told ? 1 : 0));
    CHECK(client.closed_within(2s));
    // (the blocking shutdown returns once decided, maybe before the close ran)
    CHECK(eventually([&] { return server.stats().active_connections() == 0; }));
    INFO("taken over " << server.stats().taken_over_connections << ", websocket "
         << server.stats().websocket_connections << ", http " << server.stats().http_connections);
    CHECK(server.stats().active_connections() == 0);
}

TEST_CASE("A server_scope with no server, or whose server is gone, does nothing", "[server][scope][integration]") {
    SECTION("Default-constructed") {
        http::server_scope scope;
        bool called = false;
        scope.set_type(http::connection_type::tunnel);
        scope.on_shutdown([&] { called = true; });
        scope.closed();
        scope.closed();
        http::server_scope copy = scope;
        copy.on_shutdown([&] { called = true; });
        CHECK_FALSE(called);
    }

    SECTION("Its server destroyed") {
        http::test::running_workers workers;
        upgraded_state state;
        state.register_shutdown = false;
        auto server = std::make_unique<http::pool_server>(workers);
        server->get(take_over_path, [&state](http::response& res) {
            res.take_over([&state](http::upgraded_connection conn) {
                auto executor = boost::asio::any_io_executor(conn.socket->get_io_context().get_executor());
                keep(state, executor, std::move(conn.socket), conn.scope);
            });
        });
        REQUIRE_FALSE(server->listen("127.0.0.1", 0));
        auto port = http::test::listening_port(*server);

        upgrade_client client(server_test::transport_endpoint{server_test::transport::tcp, port, {}}, take_over_path);
        REQUIRE(eventually([&] { return state.upgraded.load(); }));
        REQUIRE(eventually([&] { return server->stats().taken_over_connections == 1; }));
        server->stop();
        server.reset();

        // The socket is the handler's: its scope does nothing now
        std::atomic<bool> called{false};
        state.on_thread([&] {
            state.scope.set_type(http::connection_type::tunnel);
            state.scope.on_shutdown([&called] { called = true; });
            state.scope.closed();
            state.scope.closed();
        });
        std::this_thread::sleep_for(50ms);
        CHECK_FALSE(called.load());
        state.on_thread([&] { state.close(); });
        CHECK(client.closed_within(3s));
    }
}

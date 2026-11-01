#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <thinger/http/server/pool_server.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>

#include "server_test_helpers.hpp"
#include "shutdown_test_helpers.hpp"
#include "../fixtures/awaited_shutdown.hpp"
#include "../fixtures/listener_helpers.hpp"
#include "../fixtures/running_workers.hpp"
#include "../fixtures/server_listen.hpp"

#include <atomic>
#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

// shutdown(by): `by` is the whole budget of a shutdown. It ends as soon as the connections
// finish (at once on an idle server, however far `by` is), and at `by` at the latest (± 50 ms,
// the sockets left closed by then), on every listener at once. It may be awaited on a thread of
// the server, which goes on serving meanwhile; the blocking shutdown(grace) does not wait on
// one. And the three steps: stop_listening() (quiesce), shutdown() (drain), stop() (release)

using namespace thinger;
using namespace std::chrono_literals;
using server_test::running_server;
using server_test::transport;
using server_test::transport_endpoint;
using http::test::awaited_shutdown;
using shutdown_test::can_connect;
using shutdown_test::eventually;
using shutdown_test::raw_client;
using shutdown_test::slow_route;

namespace {

    using shutdown_test::clock;   // here, as ::clock is the C function

    // How far from `by` a shutdown may end
    constexpr auto tolerance = 50ms;

    // How far `when` is from `by`, either way
    std::chrono::milliseconds off_by(clock::time_point when, clock::time_point by) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(when > by ? when - by : by - when);
    }

    std::chrono::milliseconds since(clock::time_point begin) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - begin);
    }

    // `server` (listening already) waited on a thread of its own: when wait() returned. Stopped
    // when destroyed, if it did not stop before
    struct waited_server {
        http::server_base& server;
        std::future<clock::time_point> returned;

        explicit waited_server(http::server_base& s)
            : server(s), returned(std::async(std::launch::async, [this] {
                  server.wait();
                  return clock::now();
              })) {}

        ~waited_server() {
            server.stop();
            if (returned.valid()) returned.wait();
        }

        waited_server(const waited_server&) = delete;
        waited_server& operator=(const waited_server&) = delete;
    };

    // A server listening on two transports at once, waited on a thread of its own
    struct two_listeners {
        http::test::transport_listener first;
        http::test::transport_listener second;
        waited_server waited;

        two_listeners(http::server_base& server, transport first_kind, transport second_kind)
            : first(server, first_kind), second(server, second_kind), waited(server) {}
    };

}

// --- An idle server, and the deadlines passed or far away (US1 1, 4) ---

TEST_CASE("Shutdown of an idle server ends at once, and the server stops accepting",
          "[server][shutdown][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    server.get("/hello", [](http::response& res) { res.send("hi"); });
    running_server running(server, kind);
    REQUIRE(server.is_listening());

    // However far its deadline
    auto grace = GENERATE(5000ms, std::chrono::milliseconds(10h));
    INFO("by " << grace.count() << " ms from now");
    auto begin = clock::now();
    awaited_shutdown shutdown(server, grace);
    REQUIRE(shutdown.ended_within(1s));
    INFO("ended in " << since(begin).count() << " ms");
    REQUIRE(clock::now() - begin < 100ms);
    REQUIRE(shutdown.get());
    REQUIRE_FALSE(server.is_listening());
    REQUIRE_FALSE(can_connect(running.endpoint()));
}

TEST_CASE("Shutdown of an idle server blocking a thread of the application ends at once",
          "[server][shutdown][deadline][integration]") {
    http::server server;
    running_server running(server);

    auto begin = clock::now();
    REQUIRE(server.shutdown(std::chrono::milliseconds(10h)));
    REQUIRE(clock::now() - begin < 100ms);
    REQUIRE_FALSE(can_connect(running.endpoint()));
}

TEST_CASE("Shutdown with its deadline passed closes what is left at once",
          "[server][shutdown][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    std::promise<void> started;
    slow_route(server, "/stuck", 30s, &started);
    running_server running(server, kind);

    SECTION("Without connections: clean") {
        auto begin = clock::now();
        awaited_shutdown shutdown(server, clock::now() - 1s);
        REQUIRE(shutdown.ended_within(1s));
        REQUIRE(clock::now() - begin < tolerance);
        REQUIRE(shutdown.get());
        REQUIRE_FALSE(can_connect(running.endpoint()));
    }

    SECTION("With a request in flight: closed, not clean") {
        raw_client client(running.endpoint());
        client.get("/stuck");
        REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

        auto begin = clock::now();
        awaited_shutdown shutdown(server, clock::now() - 1ms);
        REQUIRE(shutdown.ended_within(1s));
        REQUIRE(clock::now() - begin < tolerance);
        REQUIRE_FALSE(shutdown.get());
        auto rest = client.rest(2s);
        REQUIRE(rest.closed);
        REQUIRE(rest.data.empty());
    }
}

TEST_CASE("Shutdown of a server not running ends at once", "[server][shutdown][deadline][integration]") {
    SECTION("Listening, but wait() never called") {
        http::server server;
        REQUIRE_FALSE(server.listen("127.0.0.1", 0));
        auto port = http::test::listening_port(server);

        auto begin = clock::now();
        awaited_shutdown shutdown(server, 5s);
        REQUIRE(shutdown.ended_within(1s));
        REQUIRE(clock::now() - begin < 100ms);
        REQUIRE(shutdown.get());
        REQUIRE_FALSE(server.is_listening());
        REQUIRE_FALSE(can_connect(port));
    }

    SECTION("Never listening") {
        http::server server;
        awaited_shutdown shutdown(server, 5s);
        REQUIRE(shutdown.ended_within(100ms));
        REQUIRE(shutdown.get());
    }
}

// --- Draining until the deadline (US1 2, 3, 5) ---

TEST_CASE("Shutdown closes an idle connection at once and ends when the request in flight does",
          "[server][shutdown][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    server.get("/hello", [](http::response& res) { res.send("hi"); });
    std::promise<void> started;
    slow_route(server, "/slow", 200ms, &started);
    running_server running(server, kind);

    raw_client idle(running.endpoint());
    idle.get("/hello");
    REQUIRE(idle.read_until("hi"));
    raw_client busy(running.endpoint());
    busy.get("/slow");
    REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);
    auto request_began = clock::now();

    auto begin = clock::now();
    awaited_shutdown shutdown(server, 5s);

    // The idle one at once, well before the request ends
    auto idle_rest = idle.rest(1s);
    REQUIRE(idle_rest.closed);
    REQUIRE(idle_rest.data.empty());
    INFO("idle closed in " << since(begin).count() << " ms");
    REQUIRE(clock::now() - begin < 150ms);

    // The shutdown ends just after the request, long before its deadline
    REQUIRE(shutdown.ended_within(3s));
    INFO("ended " << since(request_began).count() << " ms after the request began");
    REQUIRE(clock::now() - request_began >= 200ms);
    REQUIRE(clock::now() - begin < 500ms);
    REQUIRE(shutdown.get());

    // and the request got its answer, telling the client the connection closes
    auto response = busy.rest();
    REQUIRE(response.closed);
    REQUIRE(response.data.starts_with("HTTP/1.1 200"));
    REQUIRE(response.data.find("Connection: Close") != std::string::npos);
    REQUIRE(response.data.ends_with("done"));
}

TEST_CASE("Shutdown ends at its deadline with a handler still running, the client closed by then",
          "[server][shutdown][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    // Its timing, again and again
    for (int round = 0; round < 20; ++round) {
        INFO("round " << round);
        http::server server;
        std::promise<void> started;
        slow_route(server, "/stuck", 30s, &started);
        running_server running(server, kind);
        raw_client client(running.endpoint());
        client.get("/stuck");
        REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

        awaited_shutdown shutdown(server, 500ms);
        REQUIRE(shutdown.ended_within(2s));
        auto ended = clock::now();
        INFO("ended " << std::chrono::duration_cast<std::chrono::milliseconds>(ended - shutdown.by()).count()
                      << " ms from its deadline");
        REQUIRE(off_by(ended, shutdown.by()) <= tolerance);
        REQUIRE_FALSE(shutdown.get());

        // The whole of it, with the sockets closed, within the deadline too
        auto rest = client.rest(2s);
        auto closed = clock::now();
        INFO("client closed " << std::chrono::duration_cast<std::chrono::milliseconds>(closed - shutdown.by()).count()
                              << " ms from the deadline");
        REQUIRE(rest.closed);
        REQUIRE(rest.data.empty());
        REQUIRE(closed - shutdown.by() <= tolerance);
    }
}

TEST_CASE("Shutdown blocking a thread of the application ends as the awaited one",
          "[server][shutdown][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    std::promise<void> started;

    SECTION("Every connection finishing: clean, once they do") {
        slow_route(server, "/slow", 200ms, &started);
        running_server running(server, kind);
        raw_client client(running.endpoint());
        client.get("/slow");
        REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

        auto begin = clock::now();
        REQUIRE(server.shutdown(500ms));
        REQUIRE(clock::now() - begin < 500ms);
        REQUIRE(client.rest().data.ends_with("done"));
    }

    SECTION("A handler going on past the deadline: not clean, at the deadline") {
        slow_route(server, "/stuck", 30s, &started);
        running_server running(server, kind);
        raw_client client(running.endpoint());
        client.get("/stuck");
        REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

        auto by = clock::now() + 500ms;
        REQUIRE_FALSE(server.shutdown(500ms));
        INFO("ended " << since(by).count() << " ms after its deadline");
        REQUIRE(off_by(clock::now(), by) <= tolerance);
        REQUIRE(client.rest(2s).closed);
    }
}

// --- Several listeners under one deadline (US2) ---

TEST_CASE("Shutdown drains every listener at once, under the same deadline", "[server][shutdown][deadline][integration]") {
    auto [first_kind, second_kind] = GENERATE(table<transport, transport>({
        {transport::tcp, transport::tls},
        {transport::tcp, transport::unix},
    }));
    INFO("listening on " << first_kind << " and " << second_kind);
    http::server server;
    std::promise<void> first_started;
    std::promise<void> second_started;

    SECTION("Handlers of 300 ms on each: clean after 300 ms, not 600") {
        slow_route(server, "/first", 300ms, &first_started);
        slow_route(server, "/second", 300ms, &second_started);
        two_listeners listeners(server, first_kind, second_kind);
        raw_client first(listeners.first.endpoint());
        raw_client second(listeners.second.endpoint());
        first.get("/first");
        second.get("/second");
        REQUIRE(first_started.get_future().wait_for(3s) == std::future_status::ready);
        REQUIRE(second_started.get_future().wait_for(3s) == std::future_status::ready);

        auto begin = clock::now();
        awaited_shutdown shutdown(server, 5s);
        REQUIRE_FALSE(can_connect(listeners.first.endpoint()));
        REQUIRE_FALSE(can_connect(listeners.second.endpoint()));

        REQUIRE(shutdown.ended_within(3s));
        INFO("ended in " << since(begin).count() << " ms");
        REQUIRE(clock::now() - begin < 300ms + tolerance);
        REQUIRE(shutdown.get());
        for (auto* client : {&first, &second}) {
            auto response = client->rest();
            REQUIRE(response.closed);
            REQUIRE(response.data.find("Connection: Close") != std::string::npos);
            REQUIRE(response.data.ends_with("done"));
        }
    }

    SECTION("Handlers that do not finish on each: closed at the deadline, not twice it") {
        slow_route(server, "/first", 30s, &first_started);
        slow_route(server, "/second", 30s, &second_started);
        two_listeners listeners(server, first_kind, second_kind);
        raw_client first(listeners.first.endpoint());
        raw_client second(listeners.second.endpoint());
        first.get("/first");
        second.get("/second");
        REQUIRE(first_started.get_future().wait_for(3s) == std::future_status::ready);
        REQUIRE(second_started.get_future().wait_for(3s) == std::future_status::ready);

        awaited_shutdown shutdown(server, 500ms);
        REQUIRE_FALSE(can_connect(listeners.first.endpoint()));
        REQUIRE_FALSE(can_connect(listeners.second.endpoint()));

        REQUIRE(shutdown.ended_within(2s));
        INFO("ended " << since(shutdown.by()).count() << " ms after its deadline");
        REQUIRE(off_by(clock::now(), shutdown.by()) <= tolerance);
        REQUIRE_FALSE(shutdown.get());
        REQUIRE(first.rest(2s).closed);
        REQUIRE(second.rest(2s).closed);
        REQUIRE(listeners.waited.returned.wait_for(1s) == std::future_status::ready);
    }
}

// --- On a thread of the server, and blocking (US3) ---

namespace {

    // A shutdown started by a handler and awaited on its thread, the only one of the server,
    // while another request is in flight there and a third one arrives on a connection open:
    // both are answered during the drain, which ends clean once they are
    struct shutdown_on_server_thread {
        std::promise<void> started;
        std::promise<bool> result;
        std::thread::id handler_thread;
        std::thread::id result_thread;

        // Its routes, before it listens
        explicit shutdown_on_server_thread(http::server_base& server) {
            slow_route(server, "/slow", 300ms, &started);
            server.get("/hello", [](http::response& res) { res.send("hi"); });
            server.get("/quit", [this, &server](http::response& res) -> thinger::awaitable<void> {
                res.send("bye");
                handler_thread = std::this_thread::get_id();
                co_spawn(co_await boost::asio::this_coro::executor, server.shutdown(clock::now() + 5s),
                         [this](std::exception_ptr, bool clean) {
                             result_thread = std::this_thread::get_id();
                             result.set_value(clean);
                         });
            });
        }

        void run(http::server_base& server, const transport_endpoint& to) {
            raw_client busy(to);
            busy.get("/slow");
            REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);
            raw_client arriving(to);
            arriving.send("GET /hello HTTP/1.1\r\n");
            REQUIRE(eventually([&] { return server.stats().http_connections == 2; }));

            raw_client quit(to);
            quit.get("/quit");
            auto bye = quit.rest();
            REQUIRE(bye.closed);
            REQUIRE(bye.data.ends_with("bye"));
            REQUIRE(eventually([&] { return !server.is_listening(); }));

            // Served on that thread while the shutdown awaits there
            arriving.send("Host: localhost\r\n\r\n");
            auto hello = arriving.rest();
            REQUIRE(hello.closed);
            REQUIRE(hello.data.starts_with("HTTP/1.1 200"));
            REQUIRE(hello.data.find("Connection: Close") != std::string::npos);
            REQUIRE(hello.data.ends_with("hi"));
            auto response = busy.rest();
            REQUIRE(response.closed);
            REQUIRE(response.data.find("Connection: Close") != std::string::npos);
            REQUIRE(response.data.ends_with("done"));

            auto ended = result.get_future();
            REQUIRE(ended.wait_for(3s) == std::future_status::ready);
            REQUIRE(ended.get());
            REQUIRE(result_thread == handler_thread);
        }
    };

}

TEST_CASE("Shutdown awaited on a thread of the server goes on serving it", "[server][shutdown][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);

    SECTION("A server") {
        http::server server;
        shutdown_on_server_thread scenario(server);
        running_server running(server, kind);
        scenario.run(server, running.endpoint());
    }

    SECTION("A worker of a pool server, its only one") {
        http::test::running_workers workers(1);
        http::pool_server server(workers);
        shutdown_on_server_thread scenario(server);
        http::test::transport_listener listener(server, kind);
        waited_server waited(server);
        scenario.run(server, listener.endpoint());
        // wait() returns once it ends
        REQUIRE(waited.returned.wait_for(2s) == std::future_status::ready);
    }
}

TEST_CASE("Shutdown blocking a thread of the server does not wait there, and closes at its deadline",
          "[server][shutdown][deadline][logging][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::test::log_capture logs;
    http::server server;
    std::promise<void> started;
    slow_route(server, "/stuck", 30s, &started);
    struct call {
        bool clean = true;
        clock::time_point began;
        clock::time_point returned;
    };
    std::promise<call> called;
    server.get("/quit", [&](http::response& res) {
        auto began = clock::now();
        bool clean = server.shutdown(500ms);
        called.set_value({clean, began, clock::now()});
        res.send("bye");
    });
    http::test::transport_listener listener(server, kind);
    waited_server waited(server);

    raw_client stuck(listener.endpoint());
    stuck.get("/stuck");
    REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);
    raw_client quit(listener.endpoint());
    quit.get("/quit");

    // It returns false at once, naming the misuse
    auto future = called.get_future();
    REQUIRE(future.wait_for(3s) == std::future_status::ready);
    auto result = future.get();
    REQUIRE_FALSE(result.clean);
    REQUIRE(result.returned - result.began < tolerance);
    REQUIRE(logs.count("error", "cannot block a thread of the server") == 1);

    // and the shutdown goes on as an awaited one: listening no more, the handler answered, and
    // what is left closed at its deadline, when wait() returns
    REQUIRE(quit.rest().data.ends_with("bye"));
    REQUIRE_FALSE(can_connect(listener.endpoint()));
    REQUIRE(waited.returned.wait_for(2s) == std::future_status::ready);
    auto by = result.began + 500ms;
    auto stopped = waited.returned.get();
    INFO("stopped " << std::chrono::duration_cast<std::chrono::milliseconds>(stopped - by).count()
                    << " ms from its deadline");
    REQUIRE(off_by(stopped, by) <= tolerance);
    REQUIRE(stuck.rest(2s).closed);
}

// --- Quiesce, drain, release; the calls sharing an end; the notice and the forced close (US4, US5) ---

TEST_CASE("stop_listening() refuses new connections and goes on serving those open",
          "[server][shutdown][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    server.get("/hello", [](http::response& res) { res.send("hi"); });
    std::promise<void> started;
    slow_route(server, "/slow", 200ms, &started);
    std::atomic<int> notified{0};
    server.on_shutdown([&] { ++notified; });
    running_server running(server, kind);

    raw_client client(running.endpoint());
    client.get("/hello");
    REQUIRE(client.read_until("hi"));
    client.received.clear();

    server.stop_listening();
    server.stop_listening();   // nothing more
    REQUIRE_FALSE(server.is_listening());
    REQUIRE_FALSE(can_connect(running.endpoint()));
    REQUIRE(notified == 0);

    // The connection open is served as before, kept alive, and counted
    client.get("/hello");
    REQUIRE(client.read_until("hi"));
    REQUIRE(client.received.find("Connection: Keep-Alive") != std::string::npos);
    REQUIRE(eventually([&] { return server.stats().http_connections == 1; }));

    // A shutdown afterwards drains it as usual
    client.get("/slow");
    REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);
    awaited_shutdown shutdown(server, 5s);
    REQUIRE(notified == 1);
    auto response = client.rest();
    REQUIRE(response.closed);
    REQUIRE(response.data.find("Connection: Close") != std::string::npos);
    REQUIRE(response.data.ends_with("done"));
    REQUIRE(shutdown.ended_within(3s));
    REQUIRE(shutdown.get());
}

TEST_CASE("stop() during a shutdown ends it with false, and wait() returns", "[server][shutdown][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    std::promise<void> started;
    slow_route(server, "/stuck", 30s, &started);
    http::test::transport_listener listener(server, kind);
    waited_server waited(server);
    raw_client client(listener.endpoint());
    client.get("/stuck");
    REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

    awaited_shutdown shutdown(server, 30s);
    REQUIRE_FALSE(shutdown.ended_within(50ms));
    auto stopped = clock::now();
    server.stop();
    REQUIRE(shutdown.ended_within(1s));
    REQUIRE_FALSE(shutdown.get());
    REQUIRE(waited.returned.wait_for(1s) == std::future_status::ready);
    REQUIRE(waited.returned.get() - stopped < 100ms);
}

TEST_CASE("Shutdowns called twice share the end of the first one", "[server][shutdown][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    std::promise<void> started;
    slow_route(server, "/stuck", 30s, &started);
    running_server running(server, kind);
    raw_client client(running.endpoint());
    client.get("/stuck");
    REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

    SECTION("A later call keeps the deadline of the first") {
        awaited_shutdown first(server, 500ms);
        awaited_shutdown later(server, 30s);
        REQUIRE(later.ended_within(2s));
        INFO("ended " << since(first.by()).count() << " ms after the first deadline");
        REQUIRE(off_by(clock::now(), first.by()) <= tolerance);
        REQUIRE_FALSE(later.get());
        REQUIRE(first.ended_within(1s));
        REQUIRE_FALSE(first.get());
    }

    SECTION("Concurrent calls from two threads") {
        auto call = [&] {
            bool clean = server.shutdown(500ms);
            return std::pair{clean, clock::now()};
        };
        auto by = clock::now() + 500ms;
        auto one = std::async(std::launch::async, call);
        auto other = std::async(std::launch::async, call);
        for (auto* future : {&one, &other}) {
            REQUIRE(future->wait_for(2s) == std::future_status::ready);
            auto [clean, ended] = future->get();
            INFO("ended " << std::chrono::duration_cast<std::chrono::milliseconds>(ended - by).count()
                          << " ms from the deadline");
            REQUIRE_FALSE(clean);
            REQUIRE(off_by(ended, by) <= tolerance);
        }
    }
    REQUIRE(client.rest(2s).closed);
}

TEST_CASE("on_shutdown() is called once, as the drain starts", "[server][shutdown][deadline][integration]") {
    http::test::log_capture logs;
    http::server server;
    std::promise<void> started;
    slow_route(server, "/slow", 200ms, &started);
    std::atomic<int> notified{0};

    SECTION("Once over every call") {
        server.on_shutdown([&] { ++notified; });
        running_server running(server);
        raw_client client(running.endpoint());
        client.get("/slow");
        REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

        awaited_shutdown first(server, 5s);
        // Before waiting: the request is still in flight
        REQUIRE(notified == 1);
        REQUIRE_FALSE(first.ended_within(0ms));
        awaited_shutdown second(server, 5s);
        REQUIRE(server.shutdown(5s));
        REQUIRE(first.ended_within(1s));
        REQUIRE(second.ended_within(1s));
        REQUIRE(first.get());
        REQUIRE(second.get());
        REQUIRE(notified == 1);
        REQUIRE(client.rest().data.ends_with("done"));
    }

    SECTION("What it throws is logged, and the shutdown goes on") {
        server.on_shutdown([&] {
            ++notified;
            throw std::runtime_error("on_shutdown failing");
        });
        running_server running(server);
        raw_client client(running.endpoint());
        client.get("/slow");
        REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

        REQUIRE(server.shutdown(5s));
        REQUIRE(notified == 1);
        REQUIRE(logs.count("error", "on_shutdown failing") == 1);
        REQUIRE(client.rest().data.ends_with("done"));
    }
}

TEST_CASE("Shutdown closes a connection taken over that does not close at its deadline",
          "[server][shutdown][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    server.get("/raw", [](http::response& res) {
        res.take_over([](http::upgraded_connection conn) {
            auto socket = std::move(conn.socket);
            // Reading until closed, deaf to the shutdown
            co_spawn(socket->get_io_context(), [socket]() -> thinger::awaitable<void> {
                uint8_t buffer[64];
                while (true) {
                    auto [ec, bytes] = co_await socket->read_some(buffer, sizeof(buffer));
                    if (ec) break;
                }
            }, boost::asio::detached);
        });
    });
    running_server running(server, kind);
    raw_client client(running.endpoint());
    client.get("/raw");
    REQUIRE(client.read_until("\r\n\r\n"));
    REQUIRE(eventually([&] { return server.stats().taken_over_connections == 1; }));

    awaited_shutdown shutdown(server, 500ms);
    REQUIRE(shutdown.ended_within(2s));
    INFO("ended " << since(shutdown.by()).count() << " ms after its deadline");
    REQUIRE(off_by(clock::now(), shutdown.by()) <= tolerance);
    REQUIRE_FALSE(shutdown.get());
    REQUIRE(client.rest(2s).closed);
}

#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/pool_server.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/sse_connection.hpp>
#include <thinger/http/server/websocket_connection.hpp>

#include "server_test_helpers.hpp"
#include "shutdown_test_helpers.hpp"
#include "../fixtures/awaited_shutdown.hpp"
#include "../fixtures/handler_probe.hpp"
#include "../fixtures/stream_test_helpers.hpp"
#include "../fixtures/running_workers.hpp"

#include <atomic>
#include <csignal>
#include <filesystem>
#include <future>
#include <optional>
#include <signal.h>
#include <unistd.h>
#include "../fixtures/server_listen.hpp"

// Graceful shutdown, awaited (co_await shutdown(by)) or blocking (shutdown(grace)): the server
// stops listening at once, closes the idle connections, lets the requests in flight finish (their
// responses tell the client the connection closes), tells the connections taken over, and closes
// whatever is left at its deadline. And the statistics of the connections it tracks for that

using namespace thinger;
using namespace std::chrono_literals;
using server_test::running_server;
using http::test::awaited_shutdown;
using shutdown_test::can_connect;
using shutdown_test::eventually;
using shutdown_test::raw_client;
using shutdown_test::slow_route;

namespace {
    using shutdown_test::clock;   // here, as ::clock is the C function
}

TEST_CASE("Shutdown lets a request in flight finish with Connection: close", "[server][shutdown][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    std::promise<void> started;
    slow_route(server, "/slow", 300ms, &started);
    running_server running(server, kind);

    raw_client client(running.endpoint());
    client.get("/slow");
    REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

    auto begin = clock::now();
    awaited_shutdown shutdown(server, 5s);

    // New connections are refused right away
    REQUIRE_FALSE(can_connect(running.endpoint()));
    REQUIRE_FALSE(server.is_listening());

    // The request in flight gets its answer, telling the client the connection closes, and
    // the connection is closed after it
    auto response = client.rest();
    REQUIRE(response.closed);
    REQUIRE(response.data.starts_with("HTTP/1.1 200"));
    REQUIRE(response.data.find("Connection: Close") != std::string::npos);
    REQUIRE(response.data.ends_with("done"));

    REQUIRE(shutdown.ended_within(3s));
    REQUIRE(shutdown.get());
    REQUIRE(clock::now() - begin < 3s);
}

TEST_CASE("Shutdown closes idle keep-alive connections right away", "[server][shutdown][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    server.get("/hello", [](http::response& res) { res.send("hi"); });
    running_server running(server, kind);

    raw_client idle(running.endpoint());
    idle.get("/hello");
    REQUIRE(idle.read_until("hi"));
    REQUIRE(idle.received.find("Connection: Keep-Alive") != std::string::npos);
    raw_client fresh(running.endpoint());  // connected, nothing sent yet
    REQUIRE(eventually([&] { return server.stats().http_connections == 2; }));

    auto begin = clock::now();
    REQUIRE(server.shutdown(5s));
    REQUIRE(clock::now() - begin < 1s);

    auto rest = idle.rest(2s);
    REQUIRE(rest.closed);
    REQUIRE(rest.data.empty());
    REQUIRE(fresh.rest(2s).closed);
    REQUIRE(server.stats().active_connections() == 0);
}

TEST_CASE("Shutdown closes what is left at its deadline", "[server][shutdown][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    std::promise<void> started;
    slow_route(server, "/stuck", 30s, &started);
    running_server running(server, kind);

    raw_client client(running.endpoint());
    client.get("/stuck");
    REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

    auto begin = clock::now();
    REQUIRE_FALSE(server.shutdown(300ms));
    auto elapsed = clock::now() - begin;
    REQUIRE(elapsed >= 300ms);
    REQUIRE(elapsed < 1s);

    auto rest = client.rest(2s);
    INFO("received: " << rest.data);
    REQUIRE(rest.closed);
    REQUIRE(rest.data.empty());
}

TEST_CASE("Shutdown interrupts the handlers still running at its deadline", "[server][shutdown][cancellation][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::test::handler_probe probe;
    http::server server;
    // A default route, and one declaring another reason: both are interrupted
    http::test::waiting_route(server, "/default", probe, 10s);
    http::test::waiting_route(server, "/declared", probe, 10s).cancel_on({http::cancel_reason::timeout});
    running_server running(server, kind);
    auto path = GENERATE(std::string("/default"), std::string("/declared"));
    INFO("route " << path);

    raw_client client(running.endpoint());
    client.get(path);
    REQUIRE(probe.wait_started());

    auto by = clock::now() + 200ms;
    awaited_shutdown shutdown(server, by);
    REQUIRE(probe.wait_ended());
    INFO("interrupted " << probe.ended_after(by).count() << " ms after the deadline");
    CHECK(probe.interrupted);
    CHECK(probe.reason == http::cancel_reason::shutdown);
    CHECK(probe.end_time() >= by);
    CHECK(probe.ended_after(by) <= 50ms);

    // Its connection is closed then, without waiting for the handler: the drain failed
    REQUIRE(shutdown.ended_within(1s));
    CHECK_FALSE(shutdown.get());
    CHECK(client.rest(2s).closed);
}

TEST_CASE("Shutdown answers a request arriving on a draining connection with Connection: close",
          "[server][shutdown][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    std::promise<void> started;
    slow_route(server, "/slow", 300ms, &started);
    running_server running(server, kind);

    // Part of a request arrived when the shutdown starts: it is in progress, not idle
    raw_client client(running.endpoint());
    client.send("GET /slow HTTP/1.1\r\n");
    REQUIRE(eventually([&] { return server.stats().http_connections == 1; }));
    awaited_shutdown shutdown(server, 5s);
    std::this_thread::sleep_for(50ms);
    client.send("Host: localhost\r\n\r\n");

    auto response = client.rest();
    REQUIRE(response.closed);
    REQUIRE(response.data.starts_with("HTTP/1.1 200"));
    REQUIRE(response.data.find("Connection: Close") != std::string::npos);
    REQUIRE(shutdown.ended_within(3s));
    REQUIRE(shutdown.get());
}

TEST_CASE("Shutdown closes WebSockets with 1001 Going Away", "[server][shutdown][websocket][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    server.get("/ws", [](http::response& res) {
        res.upgrade_websocket({}, [](std::shared_ptr<http::websocket_connection> ws) {
            ws->on_message([](std::string, bool) {});
        });
    });
    running_server running(server, kind);

    stream_test::raw_ws_client client(running.endpoint());
    REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
    REQUIRE(eventually([&] { return server.stats().websocket_connections == 1; }));
    REQUIRE(server.stats().http_connections == 0);

    awaited_shutdown shutdown(server, 5s);

    auto frame = client.read_frame(3s);
    REQUIRE(frame);
    REQUIRE(frame->opcode == stream_test::OP_CLOSE);
    REQUIRE(frame->close_code() == 1001);
    client.send_frame(stream_test::OP_CLOSE, stream_test::close_payload(1001));

    REQUIRE(shutdown.ended_within(3s));
    REQUIRE(shutdown.get());
    REQUIRE(client.closed_within(2s));
}

TEST_CASE("Shutdown closes connections taken over that ignore it at its deadline",
          "[server][shutdown][websocket][sse][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    std::atomic<int> notified{0};
    server.get("/ws", [&](http::response& res) {
        res.upgrade_websocket({}, [&](std::shared_ptr<http::websocket_connection> ws) {
            ws->on_message([](std::string, bool) {});
            ws->on_shutdown([&] { ++notified; });   // and goes on
        });
    });
    server.get("/events", [&](http::response& res) {
        res.start_sse([&](std::shared_ptr<http::sse_connection> sse) {
            sse->on_shutdown([&] { ++notified; });  // and goes on
            sse->send_data("hello");
        });
    });
    running_server running(server, kind);

    stream_test::raw_ws_client ws_client(running.endpoint());
    REQUIRE(ws_client.handshake("/ws").starts_with("HTTP/1.1 101"));
    raw_client sse_client(running.endpoint());
    sse_client.get("/events");
    REQUIRE(sse_client.read_until("data: hello"));
    REQUIRE(eventually([&] {
        auto stats = server.stats();
        return stats.websocket_connections == 1 && stats.sse_connections == 1;
    }));

    auto begin = clock::now();
    REQUIRE_FALSE(server.shutdown(300ms));
    REQUIRE(clock::now() - begin < 1s);
    REQUIRE(notified == 2);

    REQUIRE(ws_client.closed_within(2s));
    REQUIRE(sse_client.rest(2s).closed);
}

TEST_CASE("Shutdown closes event streams by default", "[server][shutdown][sse][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    server.get("/events", [](http::response& res) {
        res.start_sse([](std::shared_ptr<http::sse_connection> sse) { sse->send_data("hello"); });
    });
    running_server running(server, kind);

    raw_client client(running.endpoint());
    client.get("/events");
    REQUIRE(client.read_until("data: hello"));
    REQUIRE(eventually([&] { return server.stats().sse_connections == 1; }));

    REQUIRE(server.shutdown(5s));
    REQUIRE(client.rest(2s).closed);
}

TEST_CASE("Shutdown tells a connection taken over through its scope", "[server][shutdown][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    std::atomic<bool> notified{false};
    server.get("/raw", [&](http::response& res) {
        res.take_over([&](http::upgraded_connection conn) {
            auto socket = std::move(conn.socket);
            std::weak_ptr<asio::socket> weak = socket;
            conn.scope.on_shutdown([&notified, weak] {
                notified = true;
                if (auto socket = weak.lock()) socket->close();
            });
            // the socket lives while read
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

    REQUIRE(server.shutdown(5s));
    REQUIRE(notified);
    REQUIRE(client.rest(2s).closed);
    REQUIRE(server.stats().active_connections() == 0);
}

TEST_CASE("Shutdown ends as the last connection leaves, long before its deadline", "[server][shutdown][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    // A connection taken over that ignores the shutdown: open until its client leaves
    server.get("/raw", [](http::response& res) {
        res.take_over([](http::upgraded_connection conn) {
            auto socket = std::move(conn.socket);
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

    SECTION("Awaited") {
        awaited_shutdown shutdown(server, 10s);
        // Waits for it
        REQUIRE_FALSE(shutdown.ended_within(300ms));
        auto left = clock::now();
        client.connection.close();
        REQUIRE(shutdown.ended_within(3s));
        REQUIRE(shutdown.get());
        REQUIRE(clock::now() - left < 3s);
    }

    SECTION("Blocking") {
        auto shutdown = std::async(std::launch::async, [&] { return server.shutdown(10s); });
        REQUIRE(shutdown.wait_for(300ms) == std::future_status::timeout);
        auto left = clock::now();
        client.connection.close();
        REQUIRE(shutdown.wait_for(3s) == std::future_status::ready);
        REQUIRE(shutdown.get());
        REQUIRE(clock::now() - left < 3s);
    }

    REQUIRE(server.stats().active_connections() == 0);
}

TEST_CASE("Shutdown may be called twice, and stopped", "[server][shutdown][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    SECTION("Twice, and stop() afterwards") {
        http::server server;
        std::promise<void> started;
        slow_route(server, "/slow", 200ms, &started);
        running_server running(server, kind);
        raw_client client(running.endpoint());
        client.get("/slow");
        REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

        awaited_shutdown first(server, 5s);
        awaited_shutdown second(server, 1ms);   // waits for the same end
        auto blocking = std::async(std::launch::async, [&] { return server.shutdown(1ms); });

        REQUIRE(first.ended_within(3s));
        REQUIRE(second.ended_within(3s));
        REQUIRE(blocking.wait_for(3s) == std::future_status::ready);
        REQUIRE(first.get());
        REQUIRE(second.get());
        REQUIRE(blocking.get());

        // Once ended: the same result, at once
        REQUIRE(server.shutdown(5s));
        server.stop();
        REQUIRE_FALSE(server.is_listening());
        REQUIRE(client.rest(2s).data.ends_with("done"));
    }

    SECTION("stop() during a shutdown ends it") {
        http::server server;
        std::promise<void> started;
        slow_route(server, "/stuck", 30s, &started);
        running_server running(server, kind);
        raw_client client(running.endpoint());
        client.get("/stuck");
        REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

        awaited_shutdown shutdown(server, 30s);
        auto blocking = std::async(std::launch::async, [&] { return server.shutdown(30s); });
        std::this_thread::sleep_for(50ms);
        server.stop();
        REQUIRE(shutdown.ended_within(1s));
        REQUIRE_FALSE(shutdown.get());
        REQUIRE(blocking.wait_for(1s) == std::future_status::ready);
        REQUIRE_FALSE(blocking.get());
    }

    SECTION("From a handler, without waiting") {
        http::server server;
        std::promise<bool> result;
        server.get("/quit", [&](http::response& res) -> thinger::awaitable<void> {
            res.send("bye");
            // On the thread of the handler, which goes on serving meanwhile
            co_spawn(co_await boost::asio::this_coro::executor, server.shutdown(clock::now() + 5s),
                     [&](std::exception_ptr, bool clean) { result.set_value(clean); });
        });
        running_server running(server, kind);
        raw_client client(running.endpoint());
        client.get("/quit");
        auto response = client.rest();
        REQUIRE(response.closed);
        REQUIRE(response.data.ends_with("bye"));
        auto ended = result.get_future();
        REQUIRE(ended.wait_for(3s) == std::future_status::ready);
        REQUIRE(ended.get());
    }

    SECTION("A server not running ends it at once") {
        http::server server;
        REQUIRE(server.shutdown(5s));
        REQUIRE(server.shutdown(5s));
        server.stop();
        REQUIRE_FALSE(server.is_listening());
    }
}

TEST_CASE("Shutdown of a pool server", "[server][shutdown][pool][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::test::running_workers workers;
    http::pool_server server(workers);
    std::promise<void> started;
    slow_route(server, "/slow", 300ms, &started);
    server.get("/hello", [](http::response& res) { res.send("hi"); });
    http::test::transport_listener listener(server, kind);
    const auto& endpoint = listener.endpoint();
    auto waiting = std::async(std::launch::async, [&] { server.wait(); });

    // idle keep-alive connections, and one with a request in flight
    std::vector<std::unique_ptr<raw_client>> idle;
    for (int i = 0; i < 8; ++i) {
        idle.push_back(std::make_unique<raw_client>(endpoint));
        idle.back()->get("/hello");
        REQUIRE(idle.back()->read_until("hi"));
    }
    raw_client busy(endpoint);
    busy.get("/slow");
    REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);
    REQUIRE(server.stats().http_connections == 9);

    awaited_shutdown shutdown(server, 5s);
    REQUIRE_FALSE(can_connect(endpoint));
    for (auto& client : idle) REQUIRE(client->rest(2s).closed);
    auto response = busy.rest();
    REQUIRE(response.closed);
    REQUIRE(response.data.find("Connection: Close") != std::string::npos);
    REQUIRE(response.data.ends_with("done"));

    REQUIRE(shutdown.ended_within(3s));
    REQUIRE(shutdown.get());
    // wait() returns once the shutdown ends
    REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
    auto stats = server.stats();
    REQUIRE(stats.active_connections() == 0);
    REQUIRE(stats.total_connections == 9);
}

namespace {

    // Whether the process handles `signal` (a wait() of the workers is waiting for it)
    bool handling(int signal) {
        struct sigaction now {};
        ::sigaction(signal, nullptr, &now);
        return now.sa_handler != SIG_DFL && now.sa_handler != SIG_IGN;
    }

    bool ready(std::future<void>& future, std::chrono::milliseconds timeout) {
        return future.wait_for(timeout) == std::future_status::ready;
    }

}

// The signal cases stay on TCP: what they test is how an application stops on a signal, waiting
// for it on its workers, and a pool server on each transport is the case above

TEST_CASE("An application shuts its pool server down gracefully on a signal","[server][shutdown][pool][signal][integration]") {
    http::test::running_workers workers;
    http::pool_server server(workers);
    std::promise<void> started;
    // The request in flight ends once the shutdown began (the server stopped listening), however
    // late the signal arrives (a sanitizer defers it), so its response announces the close
    std::atomic<bool> release{false};
    server.get("/slow", [&started, &release](http::response& res) -> awaitable<void> {
        started.set_value();
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
        while (!release) {
            timer.expires_after(10ms);
            co_await timer.async_wait(use_nothrow_awaitable);
        }
        res.send("done");
    });
    REQUIRE_FALSE(server.listen("127.0.0.1", 0));
    auto port = http::test::listening_port(server);

    // wait() returns the signal and leaves the workers running: the shutdown is the application's
    int received = 0;
    bool clean = false;
    bool running_after_wait = false;
    auto waiting = std::async(std::launch::async, [&] {
        received = workers.wait({SIGUSR2});
        running_after_wait = workers.running();
        clean = server.shutdown(5s);
        workers.stop();
    });
    REQUIRE(eventually([] { return handling(SIGUSR2); }));

    raw_client busy(port);
    busy.get("/slow");
    REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);
    auto signalled = clock::now();
    ::kill(::getpid(), SIGUSR2);
    REQUIRE(eventually([&] { return !can_connect(port); }));
    release = true;

    // the request in flight finishes, and tells the client the connection closes
    auto response = busy.rest();
    REQUIRE(response.closed);
    REQUIRE(response.data.find("Connection: Close") != std::string::npos);
    REQUIRE(response.data.ends_with("done"));

    // then the application stops the workers, well before the deadline
    REQUIRE(ready(waiting, 3s));
    REQUIRE(clock::now() - signalled < 4s);
    REQUIRE(received == SIGUSR2);
    REQUIRE(running_after_wait);
    REQUIRE(clean);
    REQUIRE_FALSE(workers.running());
    // and the signal takes its default action again
    REQUIRE_FALSE(handling(SIGUSR2));
}

TEST_CASE("An application stops at once on a second signal, or without a drain",
          "[server][shutdown][pool][signal][integration]") {
    http::test::running_workers workers;
    http::pool_server server(workers);
    std::promise<void> started;
    slow_route(server, "/stuck", 60s, &started);
    REQUIRE_FALSE(server.listen("127.0.0.1", 0));
    auto port = http::test::listening_port(server);

    SECTION("A second signal during the drain") {
        // A shutdown begun on the first signal, and a stop on the second one, which ends it
        bool clean = true;
        auto waiting = std::async(std::launch::async, [&] {
            workers.wait({SIGUSR2});
            {
                awaited_shutdown shutdown(server, 60s);
                workers.wait({SIGUSR2});
                server.stop();
                clean = shutdown.get();
            }
            workers.stop();
        });
        REQUIRE(eventually([] { return handling(SIGUSR2); }));
        raw_client busy(port);
        busy.get("/stuck");
        REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

        ::kill(::getpid(), SIGUSR2);
        REQUIRE(eventually([&] { return !can_connect(port); }));
        // waiting for the request in flight, and for a second signal
        REQUIRE(eventually([] { return handling(SIGUSR2); }));
        REQUIRE_FALSE(ready(waiting, 300ms));

        ::kill(::getpid(), SIGUSR2);
        REQUIRE(ready(waiting, 3s));
        REQUIRE_FALSE(clean);
        REQUIRE_FALSE(workers.running());
    }

    SECTION("Without a drain") {
        auto waiting = std::async(std::launch::async, [&] {
            workers.wait({SIGUSR2});
            server.stop();
            workers.stop();
        });
        REQUIRE(eventually([] { return handling(SIGUSR2); }));
        raw_client busy(port);
        busy.get("/stuck");
        REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

        ::kill(::getpid(), SIGUSR2);
        REQUIRE(ready(waiting, 3s));
        REQUIRE_FALSE(workers.running());
    }
}

TEST_CASE("Shutdown of a server on a Unix socket", "[server][shutdown][unix][integration]") {
    auto path = (std::filesystem::temp_directory_path() / ("thinger_shutdown_" + std::to_string(::getpid()) + ".sock")).string();
    http::server server;
    server.get("/hello", [](http::response& res) { res.send("hi"); });
    REQUIRE_FALSE(server.listen(http::test::unix_listen_options(path)));
    std::thread thread([&] { server.wait(); });
    REQUIRE(std::filesystem::exists(path));

    REQUIRE(server.shutdown(5s));
    REQUIRE_FALSE(std::filesystem::exists(path));
    boost::asio::io_context ioc;
    boost::asio::local::stream_protocol::socket sock(ioc);
    boost::system::error_code ec;
    sock.connect(boost::asio::local::stream_protocol::endpoint(path), ec);
    REQUIRE(ec);
    thread.join();
}

TEST_CASE("Server statistics count connections", "[server][stats][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    server.get("/hello", [](http::response& res) { res.send("hi"); });
    server.get("/ws", [](http::response& res) {
        res.upgrade_websocket({}, [](std::shared_ptr<http::websocket_connection> ws) {
            ws->on_message([](std::string, bool) {});
        });
    });
    running_server running(server, kind);
    REQUIRE(server.stats().active_connections() == 0);

    {
        raw_client first(running.endpoint());
        raw_client second(running.endpoint());
        for (int i = 0; i < 3; ++i) {
            first.get("/hello");
            REQUIRE(first.read_until("hi"));
            first.received.clear();
        }
        second.get("/hello");
        REQUIRE(second.read_until("hi"));
        REQUIRE(eventually([&] { return server.stats().http_connections == 2; }));
        REQUIRE(server.stats().total_connections == 2);
    }
    // Closed by the clients: no longer counted
    REQUIRE(eventually([&] { return server.stats().http_connections == 0; }));
    REQUIRE(server.stats().total_connections == 2);

    {
        stream_test::raw_ws_client client(running.endpoint());
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        REQUIRE(eventually([&] { return server.stats().websocket_connections == 1; }));
        auto stats = server.stats();
        REQUIRE(stats.http_connections == 0);
        REQUIRE(stats.total_connections == 3);
        client.send_frame(stream_test::OP_CLOSE, stream_test::close_payload(1000));
        REQUIRE(client.closed_within(3s));
    }
    REQUIRE(eventually([&] { return server.stats().active_connections() == 0; }));
}

#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/pool_server.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>

#include "server_test_helpers.hpp"
#include "../fixtures/stream_test_helpers.hpp"

#include <atomic>
#include <csignal>
#include <filesystem>
#include <future>
#include <optional>
#include <signal.h>
#include <unistd.h>

// Graceful shutdown: the server stops listening at once, closes the idle connections, lets the
// requests in flight finish (their responses tell the client the connection closes), tells the
// connections taken over, and closes whatever is left once the grace period ends. And the
// statistics of the connections it tracks for that

using namespace thinger;
using namespace std::chrono_literals;
using server_test::running_server;
using server_test::read_until_closed;
using tcp = boost::asio::ip::tcp;

namespace {

    using clock = std::chrono::steady_clock;

    bool can_connect(uint16_t port) {
        boost::asio::io_context ioc;
        tcp::socket sock(ioc);
        boost::system::error_code ec;
        sock.connect({boost::asio::ip::make_address("127.0.0.1"), port}, ec);
        return !ec;
    }

    // Whether `condition` holds within the timeout
    bool eventually(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 3s) {
        auto deadline = clock::now() + timeout;
        while (clock::now() < deadline) {
            if (condition()) return true;
            std::this_thread::sleep_for(5ms);
        }
        return condition();
    }

    // A raw HTTP/1.1 client connection: every read waits up to a timeout
    struct raw_client {
        boost::asio::io_context ioc;
        tcp::socket sock{ioc};
        std::string received;

        explicit raw_client(uint16_t port) {
            sock.connect({boost::asio::ip::make_address("127.0.0.1"), port});
        }

        void send(const std::string& data) {
            boost::asio::write(sock, boost::asio::buffer(data));
        }

        void get(const std::string& path) {
            send("GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
        }

        // Read until `received` holds `needle`
        bool read_until(const std::string& needle, std::chrono::milliseconds timeout = 3s) {
            auto deadline = clock::now() + timeout;
            std::array<char, 4096> buffer{};
            while (received.find(needle) == std::string::npos) {
                bool done = false;
                boost::system::error_code error;
                sock.async_read_some(boost::asio::buffer(buffer), [&](boost::system::error_code ec, size_t bytes) {
                    received.append(buffer.data(), bytes);
                    error = ec;
                    done = true;
                });
                ioc.restart();
                ioc.run_until(deadline);
                if (!done) {
                    sock.cancel();
                    ioc.restart();
                    ioc.run();
                    return false;
                }
                if (error) return received.find(needle) != std::string::npos;
            }
            return true;
        }

        // What arrives until the server closes the connection (or the timeout expires)
        server_test::exchange_result rest(std::chrono::milliseconds timeout = 3s) {
            ioc.restart();
            return read_until_closed(ioc, sock, timeout);
        }
    };

    // A handler answering after `delay`
    void slow_route(http::http_application& app, const std::string& path, std::chrono::milliseconds delay,
                    std::promise<void>* started = nullptr) {
        app.get(path, [delay, started](http::response& res) -> thinger::awaitable<void> {
            if (started) started->set_value();
            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, delay);
            co_await timer.async_wait(thinger::use_nothrow_awaitable);
            res.send("done");
        });
    }

    // The result of begin_shutdown(), once it ends
    struct shutdown_result {
        std::promise<bool> promise;
        std::future<bool> future = promise.get_future();
        std::function<void(bool)> callback() {
            return [this](bool clean) { promise.set_value(clean); };
        }
        bool ended_within(std::chrono::milliseconds timeout) {
            return future.wait_for(timeout) == std::future_status::ready;
        }
    };

}

TEST_CASE("Shutdown lets a request in flight finish with Connection: close", "[server][shutdown][integration]") {
    http::server server;
    std::promise<void> started;
    slow_route(server, "/slow", 300ms, &started);
    running_server running(server);

    raw_client client(running.port);
    client.get("/slow");
    REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

    auto begin = clock::now();
    shutdown_result result;
    server.begin_shutdown(5s, result.callback());

    // New connections are refused right away
    REQUIRE_FALSE(can_connect(running.port));
    REQUIRE_FALSE(server.is_listening());

    // The request in flight gets its answer, telling the client the connection closes, and
    // the connection is closed after it
    auto response = client.rest();
    REQUIRE(response.closed);
    REQUIRE(response.data.starts_with("HTTP/1.1 200"));
    REQUIRE(response.data.find("Connection: Close") != std::string::npos);
    REQUIRE(response.data.ends_with("done"));

    REQUIRE(result.ended_within(3s));
    REQUIRE(result.future.get());
    REQUIRE(clock::now() - begin < 3s);
}

TEST_CASE("Shutdown closes idle keep-alive connections right away", "[server][shutdown][integration]") {
    http::server server;
    server.get("/hello", [](http::response& res) { res.send("hi"); });
    running_server running(server);

    raw_client idle(running.port);
    idle.get("/hello");
    REQUIRE(idle.read_until("hi"));
    REQUIRE(idle.received.find("Connection: Keep-Alive") != std::string::npos);
    raw_client fresh(running.port);  // connected, nothing sent yet
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

TEST_CASE("Shutdown closes what is left once the grace period ends", "[server][shutdown][integration]") {
    http::server server;
    std::promise<void> started;
    slow_route(server, "/stuck", 30s, &started);
    running_server running(server);

    raw_client client(running.port);
    client.get("/stuck");
    REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

    auto begin = clock::now();
    REQUIRE_FALSE(server.shutdown(300ms));
    auto elapsed = clock::now() - begin;
    REQUIRE(elapsed >= 300ms);
    REQUIRE(elapsed < 2s);

    auto rest = client.rest(2s);
    REQUIRE(rest.closed);
    REQUIRE(rest.data.empty());
}

TEST_CASE("Shutdown answers a request arriving on a draining connection with Connection: close",
          "[server][shutdown][integration]") {
    http::server server;
    std::promise<void> started;
    slow_route(server, "/slow", 300ms, &started);
    running_server running(server);

    // Part of a request arrived when the shutdown starts: it is in progress, not idle
    raw_client client(running.port);
    client.send("GET /slow HTTP/1.1\r\n");
    REQUIRE(eventually([&] { return server.stats().http_connections == 1; }));
    shutdown_result result;
    server.begin_shutdown(5s, result.callback());
    std::this_thread::sleep_for(50ms);
    client.send("Host: localhost\r\n\r\n");

    auto response = client.rest();
    REQUIRE(response.closed);
    REQUIRE(response.data.starts_with("HTTP/1.1 200"));
    REQUIRE(response.data.find("Connection: Close") != std::string::npos);
    REQUIRE(result.ended_within(3s));
    REQUIRE(result.future.get());
}

TEST_CASE("Shutdown closes WebSockets with 1001 Going Away", "[server][shutdown][websocket][integration]") {
    http::server server;
    server.get("/ws", [](http::response& res) {
        res.upgrade_websocket([](std::shared_ptr<http::websocket_connection> ws) {
            ws->on_message([](std::string, bool) {});
        });
    });
    running_server running(server);

    stream_test::raw_ws_client client(running.port);
    REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
    REQUIRE(eventually([&] { return server.stats().websocket_connections == 1; }));
    REQUIRE(server.stats().http_connections == 0);

    shutdown_result result;
    server.begin_shutdown(5s, result.callback());

    auto frame = client.read_frame(3s);
    REQUIRE(frame);
    REQUIRE(frame->opcode == stream_test::OP_CLOSE);
    REQUIRE(frame->close_code() == 1001);
    client.send_frame(stream_test::OP_CLOSE, stream_test::close_payload(1001));

    REQUIRE(result.ended_within(3s));
    REQUIRE(result.future.get());
    REQUIRE(client.closed_within(2s));
}

TEST_CASE("Shutdown closes connections taken over that ignore it after the grace period",
          "[server][shutdown][websocket][sse][integration]") {
    http::server server;
    std::atomic<int> notified{0};
    server.get("/ws", [&](http::response& res) {
        res.upgrade_websocket([&](std::shared_ptr<http::websocket_connection> ws) {
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
    running_server running(server);

    stream_test::raw_ws_client ws_client(running.port);
    REQUIRE(ws_client.handshake("/ws").starts_with("HTTP/1.1 101"));
    raw_client sse_client(running.port);
    sse_client.get("/events");
    REQUIRE(sse_client.read_until("data: hello"));
    REQUIRE(eventually([&] {
        auto stats = server.stats();
        return stats.websocket_connections == 1 && stats.sse_connections == 1;
    }));

    auto begin = clock::now();
    REQUIRE_FALSE(server.shutdown(300ms));
    REQUIRE(clock::now() - begin < 2s);
    REQUIRE(notified == 2);

    REQUIRE(ws_client.closed_within(2s));
    REQUIRE(sse_client.rest(2s).closed);
}

TEST_CASE("Shutdown closes event streams by default", "[server][shutdown][sse][integration]") {
    http::server server;
    server.get("/events", [](http::response& res) {
        res.start_sse([](std::shared_ptr<http::sse_connection> sse) { sse->send_data("hello"); });
    });
    running_server running(server);

    raw_client client(running.port);
    client.get("/events");
    REQUIRE(client.read_until("data: hello"));
    REQUIRE(eventually([&] { return server.stats().sse_connections == 1; }));

    REQUIRE(server.shutdown(5s));
    REQUIRE(client.rest(2s).closed);
}

TEST_CASE("Shutdown tells a connection taken over through on_server_shutdown()", "[server][shutdown][integration]") {
    http::server server;
    std::atomic<bool> notified{false};
    server.get("/raw", [&](http::response& res) {
        res.take_over([&](std::shared_ptr<asio::socket> socket, std::string) {
            std::weak_ptr<asio::socket> weak = socket;
            http::on_server_shutdown(*socket, [&notified, weak] {
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
    running_server running(server);

    raw_client client(running.port);
    client.get("/raw");
    REQUIRE(client.read_until("\r\n\r\n"));
    REQUIRE(eventually([&] { return server.stats().taken_over_connections == 1; }));

    REQUIRE(server.shutdown(5s));
    REQUIRE(notified);
    REQUIRE(client.rest(2s).closed);
    REQUIRE(server.stats().active_connections() == 0);
}

TEST_CASE("Shutdown may be called twice, and stopped", "[server][shutdown][integration]") {
    SECTION("Twice, and stop() afterwards") {
        http::server server;
        std::promise<void> started;
        slow_route(server, "/slow", 200ms, &started);
        running_server running(server);
        raw_client client(running.port);
        client.get("/slow");
        REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

        shutdown_result first, second;
        server.begin_shutdown(5s, first.callback());
        server.begin_shutdown(1ms, second.callback());   // waits for the same end
        auto blocking = std::async(std::launch::async, [&] { return server.shutdown(1ms); });

        REQUIRE(first.ended_within(3s));
        REQUIRE(second.ended_within(3s));
        REQUIRE(blocking.wait_for(3s) == std::future_status::ready);
        REQUIRE(first.future.get());
        REQUIRE(second.future.get());
        REQUIRE(blocking.get());

        // Once ended: the same result, at once
        REQUIRE(server.shutdown(5s));
        REQUIRE_FALSE(server.stop());
        REQUIRE(client.rest(2s).data.ends_with("done"));
    }

    SECTION("stop() during a shutdown ends it") {
        http::server server;
        std::promise<void> started;
        slow_route(server, "/stuck", 30s, &started);
        running_server running(server);
        raw_client client(running.port);
        client.get("/stuck");
        REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

        shutdown_result result;
        server.begin_shutdown(30s, result.callback());
        auto blocking = std::async(std::launch::async, [&] { return server.shutdown(30s); });
        std::this_thread::sleep_for(50ms);
        server.stop();
        REQUIRE(result.ended_within(1s));
        REQUIRE_FALSE(result.future.get());
        REQUIRE(blocking.wait_for(1s) == std::future_status::ready);
        REQUIRE_FALSE(blocking.get());
    }

    SECTION("From a handler, without waiting") {
        http::server server;
        shutdown_result result;
        server.get("/quit", [&](http::response& res) {
            res.send("bye");
            server.begin_shutdown(5s, result.callback());
        });
        running_server running(server);
        raw_client client(running.port);
        client.get("/quit");
        auto response = client.rest();
        REQUIRE(response.closed);
        REQUIRE(response.data.ends_with("bye"));
        REQUIRE(result.ended_within(3s));
        REQUIRE(result.future.get());
    }

    SECTION("A server not running ends it at once") {
        http::server server;
        REQUIRE(server.shutdown(5s));
        REQUIRE(server.shutdown(5s));
        REQUIRE_FALSE(server.stop());
    }
}

TEST_CASE("Shutdown of a pool server", "[server][shutdown][pool][integration]") {
    http::pool_server server;
    std::promise<void> started;
    slow_route(server, "/slow", 300ms, &started);
    server.get("/hello", [](http::response& res) { res.send("hi"); });
    REQUIRE(server.listen("127.0.0.1", 0));
    auto port = server.local_port();
    auto waiting = std::async(std::launch::async, [&] { server.wait(); });

    // idle keep-alive connections, and one with a request in flight
    std::vector<std::unique_ptr<raw_client>> idle;
    for (int i = 0; i < 8; ++i) {
        idle.push_back(std::make_unique<raw_client>(port));
        idle.back()->get("/hello");
        REQUIRE(idle.back()->read_until("hi"));
    }
    raw_client busy(port);
    busy.get("/slow");
    REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);
    REQUIRE(server.stats().http_connections == 9);

    shutdown_result result;
    server.begin_shutdown(5s, result.callback());
    REQUIRE_FALSE(can_connect(port));
    for (auto& client : idle) REQUIRE(client->rest(2s).closed);
    auto response = busy.rest();
    REQUIRE(response.closed);
    REQUIRE(response.data.find("Connection: Close") != std::string::npos);
    REQUIRE(response.data.ends_with("done"));

    REQUIRE(result.ended_within(3s));
    REQUIRE(result.future.get());
    // wait() returns once the shutdown ends
    REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
    auto stats = server.stats();
    REQUIRE(stats.active_connections() == 0);
    REQUIRE(stats.total_connections == 9);
}

namespace {

    // Wait for `signal` on the workers, on another thread (gracefully if `grace`), returning
    // once they handle it: their handler replaced the one before
    std::future<void> workers_waiting_for(int signal, std::optional<std::chrono::milliseconds> grace) {
        struct sigaction before {};
        ::sigaction(signal, nullptr, &before);
        auto waiting = std::async(std::launch::async, [signal, grace] {
            auto& workers = asio::get_workers();
            if (grace) {
                workers.wait_graceful(*grace, {unsigned(signal)});
            } else {
                workers.wait({unsigned(signal)});
            }
        });
        REQUIRE(eventually([&] {
            struct sigaction now {};
            ::sigaction(signal, nullptr, &now);
            return now.sa_handler != before.sa_handler;
        }));
        return waiting;
    }

    bool ready(std::future<void>& future, std::chrono::milliseconds timeout) {
        return future.wait_for(timeout) == std::future_status::ready;
    }

}

TEST_CASE("Workers shut pool servers down gracefully on a signal", "[server][shutdown][pool][signal][integration]") {
    http::pool_server server;
    std::promise<void> started;
    slow_route(server, "/slow", 300ms, &started);
    REQUIRE(server.listen("127.0.0.1", 0));
    auto port = server.local_port();
    auto waiting = workers_waiting_for(SIGUSR2, 5s);

    raw_client busy(port);
    busy.get("/slow");
    REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);
    auto signalled = clock::now();
    ::kill(::getpid(), SIGUSR2);

    // the request in flight finishes, and tells the client the connection closes
    auto response = busy.rest();
    REQUIRE(response.closed);
    REQUIRE(response.data.find("Connection: Close") != std::string::npos);
    REQUIRE(response.data.ends_with("done"));
    REQUIRE_FALSE(can_connect(port));

    // then the workers stop, well before the grace period ends
    REQUIRE(ready(waiting, 3s));
    REQUIRE(clock::now() - signalled < 4s);
    REQUIRE_FALSE(asio::get_workers().running());
}

TEST_CASE("Workers stop at once on a second signal, or without a grace period",
          "[server][shutdown][pool][signal][integration]") {
    http::pool_server server;
    std::promise<void> started;
    slow_route(server, "/stuck", 60s, &started);
    REQUIRE(server.listen("127.0.0.1", 0));
    auto port = server.local_port();

    SECTION("A second signal during the grace period") {
        auto waiting = workers_waiting_for(SIGUSR2, 60s);
        raw_client busy(port);
        busy.get("/stuck");
        REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

        ::kill(::getpid(), SIGUSR2);
        REQUIRE(eventually([&] { return !can_connect(port); }));
        // waiting for the request in flight
        REQUIRE_FALSE(ready(waiting, 300ms));

        ::kill(::getpid(), SIGUSR2);
        REQUIRE(ready(waiting, 3s));
        REQUIRE_FALSE(asio::get_workers().running());
    }

    SECTION("Without a grace period") {
        auto waiting = workers_waiting_for(SIGUSR2, std::nullopt);
        raw_client busy(port);
        busy.get("/stuck");
        REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

        ::kill(::getpid(), SIGUSR2);
        REQUIRE(ready(waiting, 3s));
        REQUIRE_FALSE(asio::get_workers().running());
    }
}

TEST_CASE("Shutdown of a server on a Unix socket", "[server][shutdown][unix][integration]") {
    auto path = (std::filesystem::temp_directory_path() / ("thinger_shutdown_" + std::to_string(::getpid()) + ".sock")).string();
    http::server server;
    server.get("/hello", [](http::response& res) { res.send("hi"); });
    REQUIRE(server.listen_unix(path));
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

TEST_CASE("Server statistics count connections", "[server][stats][integration]") {
    http::server server;
    server.get("/hello", [](http::response& res) { res.send("hi"); });
    server.get("/ws", [](http::response& res) {
        res.upgrade_websocket([](std::shared_ptr<http::websocket_connection> ws) {
            ws->on_message([](std::string, bool) {});
        });
    });
    running_server running(server);
    REQUIRE(server.stats().active_connections() == 0);

    {
        raw_client first(running.port);
        raw_client second(running.port);
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
        stream_test::raw_ws_client client(running.port);
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

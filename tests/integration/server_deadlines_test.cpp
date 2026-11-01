#include "server_test_helpers.hpp"
#include "../fixtures/hostile_peer.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

// The deadlines of a server connection (see connection_options) in the cases the hostile-peer
// matrix does not cover (server_hostile_peer_test.cpp), over TCP: what the handler of a request
// timed out sees, the header deadline of a later request and without one, the request deadline of a route, a group or a virtual host, the requests it
// never applies to, a response started before it, and the idle deadline while a handler runs

using namespace thinger;
using namespace std::chrono_literals;
using namespace server_test;

namespace {

    using steady = std::chrono::steady_clock;

    constexpr auto EARLY = 100ms;
    constexpr auto TOLERANCE = 1s;

    awaitable<void> sleep_for(std::chrono::milliseconds time) {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, time);
        co_await timer.async_wait(use_nothrow_awaitable);
    }

    template<typename Condition>
    bool eventually(Condition condition, std::chrono::milliseconds timeout = 5s) {
        auto deadline = steady::now() + timeout;
        while (!condition()) {
            if (steady::now() > deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    std::string post_head(const std::string& path, size_t length, const std::string& host = "localhost") {
        return "POST " + path + " HTTP/1.1\r\nHost: " + host + "\r\nContent-Length: " + std::to_string(length) +
               "\r\n\r\n";
    }

    // Read the body to its end, then answer its size
    const auto answer_size = [](http::request& req, http::response& res) -> awaitable<void> {
        if (!co_await req.read_body()) co_return;
        res.send("size=" + std::to_string(req.body().size()));
    };

    // Send a 1000-byte body to `path` steadily over 2 s (100 bytes per 200 ms): what comes back
    std::string upload(const transport_endpoint& endpoint, const std::string& path,
                       const std::string& host = "localhost") {
        http::test::raw_connection connection(endpoint);
        if (!connection.write(post_head(path, 1000, host))) return {};
        for (int i = 0; i < 10; i++) {
            std::this_thread::sleep_for(200ms);
            if (!connection.write(std::string(100, 'x'))) break;
        }
        std::string received;
        connection.read_until(received, "\r\n\r\n", 3s);
        return received;
    }

    void check_at(steady::duration elapsed, steady::duration deadline, steady::duration tolerance = TOLERANCE) {
        INFO("elapsed " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms");
        CHECK(elapsed >= deadline - EARLY);
        CHECK(elapsed <= deadline + tolerance);
    }

}

// A keep-alive connection idle between two requests longer than the header deadline (not than
// idle_timeout): the header deadline of the second counts from its first byte, not from the end of
// the first response
TEST_CASE("The header deadline of a later request counts from its first byte", "[server][deadline][integration]") {
    http::server server;
    http::connection_options options;
    options.header_timeout = 1s;
    server.set_connection_options(options);
    server.get("/", [](http::response& res) { res.send("ok"); });
    running_server running(server);

    http::test::raw_connection connection(running.endpoint());
    const std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::string first;
    REQUIRE(connection.write(request));
    REQUIRE(connection.read_until(first, "\r\n\r\nok", 2s));
    CHECK(first.starts_with("HTTP/1.1 200"));

    std::this_thread::sleep_for(1500ms);
    std::string second;
    REQUIRE(connection.write(request));
    REQUIRE(connection.read_until(second, "\r\n\r\nok", 2s));
    CHECK(second.starts_with("HTTP/1.1 200"));
}

// Zero disables the header deadline: a trickled head is bounded as before it existed, by the idle
// deadline its bytes move
TEST_CASE("Without a header deadline, a trickled head is not cut", "[server][deadline][integration]") {
    http::server server;
    http::connection_options options;
    options.header_timeout = 0ms;
    options.idle_timeout = 2s;
    server.set_connection_options(options);
    server.get("/", [](http::response& res) { res.send("ok"); });
    running_server running(server);

    http::test::hostile_client client(running.endpoint(), 200ms);
    auto outcome = client.trickle_head(4s);
    INFO("received: " << outcome.data << " (" << outcome.trickled << " bytes trickled)");
    CHECK_FALSE(outcome.closed);
    CHECK(outcome.data.empty());
    CHECK(outcome.trickled >= 15);
}

// The handler reading the body as its deadline passes: its read fails, its scope says why, and
// what it answers afterwards fails on the connection, closed after its 408 (as Node does)
TEST_CASE("A request timed out ends its body read, cancels its scope and its later answer fails",
          "[server][deadline][integration]") {
    http::server server;
    http::connection_options options;
    options.request_timeout = 1s;
    options.linger_timeout = 500ms;
    server.set_connection_options(options);
    std::atomic<bool> read_failed{false};
    std::atomic<bool> answered{false};
    std::atomic<http::cancel_reason> reason{http::cancel_reason::none};
    server.post("/upload", [&](http::request& req, http::response& res) -> awaitable<void> {
        read_failed = !co_await req.read_body();
        reason = req.cancelled();
        res.send("late answer");
        answered = true;
    });
    running_server running(server);

    http::test::raw_connection connection(running.endpoint());
    auto start = steady::now();
    REQUIRE(connection.write(post_head("/upload", 1000) + std::string(10, 'x')));
    auto result = connection.read_until_closed(1s + 500ms + TOLERANCE + 1s);
    INFO("received: " << result.data);
    REQUIRE(result.closed);
    CHECK(result.data.starts_with("HTTP/1.1 408"));
    CHECK(result.data.find("late answer") == std::string::npos);
    check_at(steady::now() - start, 1s, 500ms + TOLERANCE);
    REQUIRE(eventually([&] { return answered.load(); }));
    CHECK(read_failed);
    CHECK(reason == http::cancel_reason::timeout);
}

// Precedence: route > group > host > connection option, zero for none (as max_body_size and
// timeout). A steady 2 s upload outlives the connection's 1 s request deadline only where a
// longer one (or none) applies
TEST_CASE("A route, a group or a virtual host sets its own request deadline", "[server][deadline][integration]") {
    http::server server;
    http::connection_options options;
    options.request_timeout = 1s;
    server.set_connection_options(options);
    server.post("/default", answer_size);
    server.post("/big", answer_size).request_timeout(10s);
    server.post("/unbounded", answer_size).request_timeout(0ms);
    auto group = server.group("/group").request_timeout(10s);
    group.post("/upload", answer_size);
    group.post("/own", answer_size).request_timeout(500ms);
    auto& host = server.host("uploads.example.com").request_timeout(10s);
    host.post("/upload", answer_size);
    host.group("/group").request_timeout(0ms).post("/upload", answer_size);
    host.post("/own", answer_size).request_timeout(1s);
    running_server running(server);
    const auto& endpoint = running.endpoint();

    SECTION("The connection option applies by default") {
        CHECK(upload(endpoint, "/default").starts_with("HTTP/1.1 408"));
    }
    SECTION("A route raising it, or turning it off") {
        CHECK(upload(endpoint, "/big").starts_with("HTTP/1.1 200"));
        CHECK(upload(endpoint, "/unbounded").starts_with("HTTP/1.1 200"));
    }
    SECTION("A group, over its routes unless they set their own") {
        CHECK(upload(endpoint, "/group/upload").starts_with("HTTP/1.1 200"));
        CHECK(upload(endpoint, "/group/own").starts_with("HTTP/1.1 408"));
    }
    SECTION("A virtual host, over its routes and groups unless they set their own") {
        CHECK(upload(endpoint, "/upload", "uploads.example.com").starts_with("HTTP/1.1 200"));
        CHECK(upload(endpoint, "/group/upload", "uploads.example.com").starts_with("HTTP/1.1 200"));
        CHECK(upload(endpoint, "/own", "uploads.example.com").starts_with("HTTP/1.1 408"));
    }
}

// A deadline lowered by a route counts from the start of the request, not from the match
TEST_CASE("A request deadline set by a route counts from the start of the request", "[server][deadline][integration]") {
    http::server server;
    http::connection_options options;
    options.request_timeout = 10s;
    options.linger_timeout = 500ms;
    server.set_connection_options(options);
    server.post("/short", answer_size).request_timeout(1s);
    running_server running(server);

    http::test::raw_connection connection(running.endpoint());
    auto start = steady::now();
    REQUIRE(connection.write(post_head("/short", 1000)));
    std::string received;
    REQUIRE(connection.read_until(received, "\r\n\r\n", 1s + TOLERANCE + 1s));
    CHECK(received.starts_with("HTTP/1.1 408"));
    check_at(steady::now() - start, 1s);
}

// A request without a body is received with its head: answering it is what route::timeout bounds
TEST_CASE("A request without a body is never cut by the request deadline", "[server][deadline][integration]") {
    http::server server;
    http::connection_options options;
    options.request_timeout = 500ms;
    server.set_connection_options(options);
    server.get("/poll", [](http::response& res) -> awaitable<void> {
        co_await sleep_for(1500ms);
        res.send("polled");
    });
    running_server running(server);

    http::test::raw_connection connection(running.endpoint());
    REQUIRE(connection.write("GET /poll HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    std::string received;
    REQUIRE(connection.read_until(received, "polled", 3s));
    CHECK(received.starts_with("HTTP/1.1 200"));
}

// Its response started, the request is not answered again: its connection is closed at the
// deadline, which ends the response the client got so far
TEST_CASE("A request whose response started is closed at the request deadline, without a 408",
          "[server][deadline][integration]") {
    http::server server;
    http::connection_options options;
    options.request_timeout = 1s;
    server.set_connection_options(options);
    server.post("/stream", [](http::request& req, http::response& res) -> awaitable<void> {
        res.start_chunked("text/plain");
        res.write_chunk("started");
        if (co_await req.read_body()) res.write_chunk("read");
        res.end_chunked();
    });
    running_server running(server);

    http::test::raw_connection connection(running.endpoint());
    auto start = steady::now();
    REQUIRE(connection.write(post_head("/stream", 1000) + std::string(10, 'x')));
    auto result = connection.read_until_closed(1s + TOLERANCE + 1s);
    INFO("received: " << result.data);
    REQUIRE(result.closed);
    CHECK(result.data.starts_with("HTTP/1.1 200"));
    CHECK(result.data.find("started") != std::string::npos);
    CHECK(result.data.find("408") == std::string::npos);
    check_at(steady::now() - start, 1s);
}

// A route timeout keeps the connection from being closed as idle while it waits for its
// response, not past the deadlines of its request
TEST_CASE("A route timeout longer than the request deadline does not delay it", "[server][deadline][integration]") {
    http::server server;
    http::connection_options options;
    options.request_timeout = 1s;
    options.linger_timeout = 500ms;
    server.set_connection_options(options);
    server.post("/upload", answer_size).timeout(10s);
    running_server running(server);

    http::test::raw_connection connection(running.endpoint());
    auto start = steady::now();
    REQUIRE(connection.write(post_head("/upload", 1000)));
    std::string received;
    // A byte per 200 ms, each moving the idle deadline
    while (received.empty() && !connection.eof() && steady::now() - start < 1s + TOLERANCE + 1s) {
        connection.read_some(received, 200ms);
        if (received.empty() && !connection.write("x")) break;
    }
    connection.read_until(received, "\r\n\r\n", 1s);
    INFO("received: " << received);
    CHECK(received.starts_with("HTTP/1.1 408"));
    check_at(steady::now() - start, 1s);
}

// A slow client that keeps reading takes its whole response, however long it lasts: the write
// deadline bounds a stall, not the length of a response, and a response being written is no idle
// connection. Over a Unix socket, whose buffers (about 200 KB) keep the response waiting for the
// client: 3 MiB taken 64 KiB per 100 ms, about 5 s, past both deadlines
TEST_CASE("A slow client that keeps reading is cut neither by the write deadline nor as idle",
          "[server][deadline][integration]") {
    constexpr size_t size = 3 << 20;
    http::server server;
    http::connection_options options;
    options.write_timeout = 1s;
    options.idle_timeout = 2s;
    server.set_connection_options(options);
    server.get("/large", [](http::response& res) { res.send(std::string(size, 'x')); });
    running_server running(server, http::test::transport::unix);

    http::test::raw_connection connection(running.endpoint());
    auto start = steady::now();
    REQUIRE(connection.write("GET /large HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
    std::string received;
    while (!connection.eof() && steady::now() - start < 30s) {
        connection.read_some(received, 1s);
        std::this_thread::sleep_for(100ms);
    }
    auto took = steady::now() - start;
    INFO("received " << received.size() << " bytes in "
                     << std::chrono::duration_cast<std::chrono::milliseconds>(took).count() << " ms");
    REQUIRE(connection.eof());
    CHECK(received.starts_with("HTTP/1.1 200"));
    auto body = received.find("\r\n\r\n");
    REQUIRE(body != std::string::npos);
    CHECK(received.size() - (body + 4) == size);
    // It lasted longer than either deadline
    CHECK(took > 3s);
}

// Zero disables the write deadline alone: a client that stops reading is closed as idle, as
// before it existed, counting from the read of its request
TEST_CASE("Without a write deadline, a client that stops reading is closed as idle", "[server][deadline][integration]") {
    http::server server;
    http::connection_options options;
    options.write_timeout = 0ms;
    options.idle_timeout = 1s;
    server.set_connection_options(options);
    std::atomic<bool> handled{false};
    server.get("/large", [&handled](http::response& res) {
        handled = true;
        res.send(std::string(16 << 20, 'x'));
    });
    running_server running(server, http::test::transport::unix);

    http::test::raw_connection connection(running.endpoint());
    auto start = steady::now();
    REQUIRE(connection.write("GET /large HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    REQUIRE(eventually([&] { return handled.load(); }));
    REQUIRE(eventually([&] { return server.stats().http_connections == 0; }, 1s + TOLERANCE + 1s));
    check_at(steady::now() - start, 1s);
}

// A request with a body, then two pipelined ones read at once after it: the second of them, its head
// incomplete or invalid, arms the deadlines while the request before it is gone. The request deadline
// of the first one does not outlive it (an ASan build tells a stream used after it was freed)
TEST_CASE("A pipelined request after one with a body is timed on its own", "[server][deadline][integration]") {
    http::server server;
    server.post("/upload", [](http::request& req, http::response& res) -> awaitable<void> {
        if (co_await req.read_body()) res.send("ok");
    });
    server.get("/", [](http::response& res) { res.send("root"); });
    running_server running(server);

    http::test::raw_connection connection(running.endpoint());
    REQUIRE(connection.write(post_head("/upload", 5)));
    std::this_thread::sleep_for(200ms);
    REQUIRE(connection.write("hello"));
    std::string received;
    REQUIRE(connection.read_until(received, "\r\n\r\nok", 2s));
    std::this_thread::sleep_for(200ms);

    const std::string get = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    SECTION("The second head incomplete") {
        REQUIRE(connection.write(get + "GET / HTTP/1.1\r\nHo"));
        std::this_thread::sleep_for(300ms);
        REQUIRE(connection.write("st: localhost\r\n\r\n"));
        std::string first;
        REQUIRE(connection.read_until(first, "\r\n\r\nroot", 2s));
        // (the second one may have come with the first)
        std::string second = first.substr(first.find("\r\n\r\nroot") + 8);
        CHECK(connection.read_until(second, "\r\n\r\nroot", 2s));
        CHECK(second.starts_with("HTTP/1.1 200"));
    }
    SECTION("The second request invalid") {
        REQUIRE(connection.write(get + "NOT HTTP\r\n\r\n"));
        auto result = connection.read_until_closed(2s);
        INFO("received: " << result.data);
        CHECK(result.closed);
        CHECK(result.data.starts_with("HTTP/1.1 200"));
        CHECK(result.data.find("HTTP/1.1 400") != std::string::npos);
    }
}

// Zero disables the request deadline of the connection: a body is bounded only by the idle deadline
// its bytes move
TEST_CASE("Without a request deadline, a slow upload is not cut", "[server][deadline][integration]") {
    http::server server;
    http::connection_options options;
    options.request_timeout = 0ms;
    options.idle_timeout = 1s;
    server.set_connection_options(options);
    server.post("/upload", answer_size);
    running_server running(server);

    CHECK(upload(running.endpoint(), "/upload").starts_with("HTTP/1.1 200"));
}

// A request received whole whose handler runs is not idle, as in Node and Go: a handler longer
// than the idle timeout, on a route without a timeout of its own, gets its response to the client.
// Kept alive afterwards, the connection is idle again, from the response
TEST_CASE("A handler that runs longer than the idle timeout is not cut", "[server][deadline][integration]") {
    http::server server;
    http::connection_options options;
    options.idle_timeout = 1s;
    server.set_connection_options(options);
    std::atomic<int> handled{0};
    server.get("/slow", [&handled](http::request&, http::response& res) -> awaitable<void> {
        co_await sleep_for(2500ms);
        handled++;
        res.send("done");
    });
    running_server running(server);

    http::test::raw_connection connection(running.endpoint());
    auto start = steady::now();
    REQUIRE(connection.write("GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    std::string received;
    REQUIRE(connection.read_until(received, "\r\n\r\ndone", 5s));
    auto answered = steady::now();
    INFO("received: " << received);
    CHECK(received.starts_with("HTTP/1.1 200"));
    CHECK_FALSE(connection.eof());
    CHECK(handled == 1);
    check_at(answered - start, 2500ms);

    // Nothing more sent: closed at the idle deadline, counting from the response
    auto result = connection.read_until_closed(1s + TOLERANCE + 1s);
    CHECK(result.closed);
    CHECK(result.data.empty());
    check_at(steady::now() - answered, 1s);
}

// A body still arriving keeps the deadlines that bound a silent client: its request deadline (408),
// and without any request deadline, the idle one, though its handler runs (it reads the body)
TEST_CASE("A client silent in the middle of its body is not held by its running handler",
          "[server][deadline][integration]") {
    http::server server;
    http::connection_options options;
    server.post("/upload", answer_size);

    SECTION("Answered 408 at the request deadline") {
        options.idle_timeout = 5s;
        options.request_timeout = 1s;
        server.set_connection_options(options);
        running_server running(server);

        http::test::raw_connection connection(running.endpoint());
        auto start = steady::now();
        REQUIRE(connection.write(post_head("/upload", 1000) + std::string(100, 'x')));
        auto result = connection.read_until_closed(1s + TOLERANCE + 1s);
        INFO("received: " << result.data);
        CHECK(result.data.starts_with("HTTP/1.1 408"));
        check_at(steady::now() - start, 1s);
    }
    SECTION("Closed at the idle deadline with all request deadlines off") {
        options.idle_timeout = 1s;
        options.header_timeout = 0ms;
        options.request_timeout = 0ms;
        options.write_timeout = 0ms;
        server.set_connection_options(options);
        running_server running(server);

        http::test::raw_connection connection(running.endpoint());
        auto start = steady::now();
        REQUIRE(connection.write(post_head("/upload", 1000) + std::string(100, 'x')));
        auto result = connection.read_until_closed(1s + TOLERANCE + 1s);
        INFO("received: " << result.data);
        CHECK(result.closed);
        CHECK(result.data.empty());
        check_at(steady::now() - start, 1s);
    }
}

#include "server_test_helpers.hpp"
#include "../fixtures/hostile_peer.hpp"
#include "../fixtures/time_scale.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// A deadline the handler sets and moves while it reads the body (request::set_read_deadline()):
// how a silent and a trickling client are cut, on every transport, how it replaces the request
// deadline and what time_point::max() leaves, a deadline moved with a read pending or past, a
// response written meanwhile, and in-memory dispatch. Every wait on a peer has its deadline: the
// handlers and the clients are bounded below by what they wait for, and a server that never acts
// fails a case instead of hanging it

using namespace thinger;
using namespace std::chrono_literals;
using namespace server_test;

namespace {

    using steady = std::chrono::steady_clock;
    using deadline_point = steady::time_point;

    constexpr auto EARLY = 100ms;
    // The margin over the deadline: the watchdog that wakes the read, scaled in instrumented
    // builds (see time_scale.hpp). The spec asks for ±100 ms
    constexpr auto LATE = http::test::scaled(100ms);

    // Long enough for a body never to be complete
    constexpr size_t ANNOUNCED = 1000 * 1000;

    std::string post_head(const std::string& path, size_t length) {
        return "POST " + path + " HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(length) +
               "\r\n\r\n";
    }

    std::string lowercase(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
        return text;
    }

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

    // What the handler saw once its reads stopped
    struct read_outcome {
        std::atomic<bool> done{false};
        std::atomic<http::body_error> error{http::body_error::none};
        std::atomic<std::uint64_t> bytes{0};
        std::atomic<std::uint64_t> elapsed_ms{0};
    };

    // Read the body with read_some() until it returns 0
    awaitable<void> read_to_end(http::request& req, read_outcome& outcome, deadline_point start) {
        uint8_t buffer[65536];
        while (size_t bytes = co_await req.read_some(buffer, sizeof(buffer))) outcome.bytes += bytes;
        outcome.error = req.get_body_error();
        outcome.elapsed_ms =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - start).count());
        outcome.done = true;
    }

    void check_elapsed(const read_outcome& outcome, std::chrono::milliseconds expected,
                       std::chrono::milliseconds late = LATE) {
        INFO("elapsed " << outcome.elapsed_ms.load() << " ms");
        CHECK(outcome.elapsed_ms.load() + EARLY.count() >= static_cast<std::uint64_t>(expected.count()));
        CHECK(outcome.elapsed_ms.load() <= static_cast<std::uint64_t>((expected + late).count()));
    }

    // A deferred route whose reads are bounded by `deadline` from the start of its handler, and that
    // answers 408 itself, as a handler may, once they stop
    void route_read_deadline(http::server& server, read_outcome& outcome, std::chrono::milliseconds deadline) {
        server.post("/upload", [&outcome, deadline](http::request& req, http::response& res) -> awaitable<void> {
            auto start = steady::now();
            req.set_read_deadline(start + deadline);
            co_await read_to_end(req, outcome, start);
            res.error(http::http_response::status::timed_out, "too slow");
        });
    }

}

// A client silent after its head: the read returns 0 at the deadline, the error says why and the
// handler answers; the connection then closes, as the body stayed unread. Over every transport
TEST_CASE("A silent client is cut at the read deadline", "[server][deadline][body][integration]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    http::connection_options options;
    options.linger_timeout = 200ms;
    server.set_connection_options(options);
    read_outcome outcome;
    route_read_deadline(server, outcome, 500ms);
    running_server running(server, kind);

    http::test::raw_connection connection(running.endpoint());
    REQUIRE(connection.write(post_head("/upload", ANNOUNCED)));
    REQUIRE(eventually([&] { return outcome.done.load(); }, 3s));

    CHECK(outcome.error.load() == http::body_error::timeout);
    CHECK(outcome.bytes.load() == 0);
    check_elapsed(outcome, 500ms);

    std::string received;
    REQUIRE(connection.read_until(received, "\r\n\r\n", 2s));
    INFO("received: " << received);
    CHECK(received.starts_with("HTTP/1.1 408"));
    CHECK(lowercase(received).find("connection: close") != std::string::npos);
    auto rest = connection.read_until_closed(2s);
    CHECK(rest.closed);
}

// A client that keeps trickling bytes does not move the deadline: its bytes arrive, and the read
// is still cut in time, whatever it had read
TEST_CASE("A trickling client does not extend the read deadline", "[server][deadline][body][integration]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    http::connection_options options;
    options.linger_timeout = 200ms;
    server.set_connection_options(options);
    read_outcome outcome;
    route_read_deadline(server, outcome, 500ms);
    running_server running(server, kind);

    http::test::hostile_client client(running.endpoint(), 100ms);
    auto seen = client.trickle_body(post_head("/upload", ANNOUNCED), 3s);

    REQUIRE(outcome.done.load());
    CHECK(outcome.error.load() == http::body_error::timeout);
    CHECK(outcome.bytes.load() >= 2);
    check_elapsed(outcome, 500ms);
    INFO("received: " << seen.data);
    CHECK(seen.closed);
    CHECK(seen.answered_with("HTTP/1.1 408"));
}

// A handler moving the deadline as chunks arrive reads a body far longer than the deadline, at
// a steady pace, and the deadline never passes
TEST_CASE("A deadline moved as the body arrives does not cut a long upload", "[server][deadline][body][integration]") {
    http::server server;
    read_outcome outcome;
    constexpr size_t TOTAL = 8 * 1024 * 1024;
    constexpr size_t CHUNK = 128 * 1024;
    server.post("/upload", [&outcome](http::request& req, http::response& res) -> awaitable<void> {
        uint8_t buffer[65536];
        std::uint64_t since_move = 0;
        auto start = steady::now();
        req.set_read_deadline(start + 500ms);
        for (;;) {
            size_t bytes = co_await req.read_some(buffer, sizeof(buffer));
            if (bytes == 0) break;
            outcome.bytes += bytes;
            since_move += bytes;
            if (since_move >= sizeof(buffer)) {
                since_move = 0;
                req.set_read_deadline(steady::now() + 500ms);
            }
        }
        outcome.error = req.get_body_error();
        outcome.elapsed_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - start).count());
        outcome.done = true;
        res.send("size=" + std::to_string(outcome.bytes.load()));
    });
    running_server running(server);

    http::test::raw_connection connection(running.endpoint(), true);
    std::string chunk(CHUNK, 'x');
    REQUIRE(connection.write(post_head("/upload", TOTAL)));
    for (size_t sent = 0; sent < TOTAL; sent += CHUNK) {
        REQUIRE(connection.write(chunk));
        std::this_thread::sleep_for(35ms);
    }

    REQUIRE(eventually([&] { return outcome.done.load(); }, 5s));
    CHECK(outcome.error.load() == http::body_error::none);
    CHECK(outcome.bytes.load() == TOTAL);
    CHECK(outcome.elapsed_ms.load() >= 2000);

    std::string received;
    REQUIRE(connection.read_until(received, "size=" + std::to_string(TOTAL), 2s));
    CHECK(received.starts_with("HTTP/1.1 200"));
}

// The deadline of the handler replaces the request deadline for the rest of the body, as
// SetReadDeadline does with ReadTimeout in Go: a client silent past that deadline is cut at the
// handler's one, and its handler answers it
TEST_CASE("A read deadline replaces the request timeout", "[server][deadline][body][integration]") {
    http::server server;
    http::connection_options options;
    options.linger_timeout = 200ms;
    read_outcome outcome;
    auto handler = [&outcome](http::request& req, http::response& res) -> awaitable<void> {
        auto start = steady::now();
        req.set_read_deadline(start + 900ms);
        co_await read_to_end(req, outcome, start);
        res.error(http::http_response::status::timed_out, "my 408");
    };

    SECTION("The one of the connection") {
        options.request_timeout = 300ms;
        server.set_connection_options(options);
        server.post("/upload", handler);
    }
    SECTION("The one of the route") {
        options.request_timeout = 0ms;
        server.set_connection_options(options);
        server.post("/upload", handler).request_timeout(300ms);
    }
    running_server running(server);

    http::test::raw_connection connection(running.endpoint());
    REQUIRE(connection.write(post_head("/upload", ANNOUNCED)));
    REQUIRE(eventually([&] { return outcome.done.load(); }, 3s));
    CHECK(outcome.error.load() == http::body_error::timeout);
    check_elapsed(outcome, 900ms);

    std::string received;
    REQUIRE(connection.read_until(received, "my 408", 2s));
    CHECK(received.starts_with("HTTP/1.1 408"));
}

// time_point::max() removes the deadline, the request deadline with it: only the idle timeout
// bounds the read, as a zero time does in Go (what it leaves here is idle_timeout)
TEST_CASE("time_point::max() removes the read deadline", "[server][deadline][body][integration]") {
    http::server server;
    http::connection_options options;
    options.request_timeout = 300ms;
    options.idle_timeout = 1s;
    server.set_connection_options(options);
    read_outcome outcome;
    server.post("/upload", [&outcome](http::request& req, http::response&) -> awaitable<void> {
        auto start = steady::now();
        req.set_read_deadline(start + 200ms);
        req.set_read_deadline(deadline_point::max());
        co_await read_to_end(req, outcome, start);
    });
    running_server running(server);

    http::test::raw_connection connection(running.endpoint());
    REQUIRE(connection.write(post_head("/upload", ANNOUNCED)));
    REQUIRE(eventually([&] { return outcome.done.load(); }, 3s));
    CHECK(outcome.error.load() != http::body_error::timeout);
    check_elapsed(outcome, 1s, 1s);
}

// Set with a read pending, it applies to it: a move earlier cuts it sooner, a move later
// (or away) lets it go on past the first one
TEST_CASE("A read deadline moved applies to the pending read", "[server][deadline][body][integration]") {
    http::server server;
    http::connection_options options;
    options.linger_timeout = 200ms;
    server.set_connection_options(options);
    read_outcome outcome;
    std::chrono::milliseconds first{};
    std::chrono::milliseconds second{};
    std::chrono::milliseconds expected{};

    SECTION("Moved earlier") {
        first = 3s;
        second = 200ms;
        expected = 300ms;   // moved at 100 ms from the start
    }
    SECTION("Moved later") {
        first = 300ms;
        second = 800ms;
        expected = 900ms;
    }

    server.post("/upload", [&](http::request& req, http::response& res) -> awaitable<void> {
        auto start = steady::now();
        req.set_read_deadline(start + first);
        auto move = [&]() -> awaitable<void> {
            co_await sleep_for(100ms);
            req.set_read_deadline(steady::now() + second);
            co_await sleep_for(5s);
        };
        co_await (read_to_end(req, outcome, start) || move());
        res.error(http::http_response::status::timed_out, "too slow");
    });
    running_server running(server);

    http::test::raw_connection connection(running.endpoint());
    REQUIRE(connection.write(post_head("/upload", ANNOUNCED)));
    REQUIRE(eventually([&] { return outcome.done.load(); }, 3s));
    CHECK(outcome.error.load() == http::body_error::timeout);
    check_elapsed(outcome, expected);
}

// Set for the first time with a read already waiting (which started without one), on every
// transport: it reaches that read, moves, or goes away, as the one set before it does
TEST_CASE("A read deadline set while a read waits applies to it", "[server][deadline][body][integration]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    http::connection_options options;
    options.linger_timeout = 200ms;
    options.idle_timeout = 2s;
    read_outcome outcome;
    using step = std::pair<std::chrono::milliseconds, std::optional<std::chrono::milliseconds>>;
    std::vector<step> steps;        // after waiting the first, the deadline at the second from now (none: removed)
    std::chrono::milliseconds expected{};
    auto late = LATE;
    bool timed_out = true;

    SECTION("First set") {
        steps = {{100ms, 300ms}};
        expected = 400ms;
    }
    SECTION("First set, already past") {
        steps = {{100ms, -1000ms}};
        expected = 100ms;
    }
    SECTION("First set, then moved later") {
        steps = {{100ms, 300ms}, {100ms, 500ms}};
        expected = 700ms;
    }
    SECTION("First set, replacing the request timeout") {
        options.request_timeout = 300ms;
        steps = {{100ms, 600ms}};
        expected = 700ms;
    }
    SECTION("First set, then removed: only the idle timeout is left") {
        options.idle_timeout = 1s;
        options.request_timeout = 300ms;
        steps = {{100ms, 300ms}, {100ms, std::nullopt}};
        expected = 1s;
        late = 1s;
        timed_out = false;
    }
    SECTION("Removed with none set: only the idle timeout is left") {
        options.idle_timeout = 1s;
        options.request_timeout = 300ms;
        steps = {{100ms, std::nullopt}};
        expected = 1s;
        late = 1s;
        timed_out = false;
    }

    server.set_connection_options(options);
    server.post("/upload", [&](http::request& req, http::response& res) -> awaitable<void> {
        auto start = steady::now();
        auto set = [&]() -> awaitable<void> {
            for (const auto& [wait, deadline] : steps) {
                co_await sleep_for(wait);
                req.set_read_deadline(deadline ? steady::now() + *deadline : deadline_point::max());
            }
            co_await sleep_for(5s);
        };
        co_await (read_to_end(req, outcome, start) || set());
        res.error(http::http_response::status::timed_out, "too slow");
    });
    running_server running(server, kind);

    http::test::raw_connection connection(running.endpoint());
    REQUIRE(connection.write(post_head("/upload", ANNOUNCED)));
    REQUIRE(eventually([&] { return outcome.done.load(); }, 4s));
    CHECK((outcome.error.load() == http::body_error::timeout) == timed_out);
    check_elapsed(outcome, expected, late);
}

// A deadline already past makes the next read fail at once; passed while no read waited and moved
// before the next one, it stops being in the way, as in Go
TEST_CASE("A read deadline only bounds the reads it finds past", "[server][deadline][body][integration]") {
    http::server server;
    http::connection_options options;
    options.linger_timeout = 200ms;
    server.set_connection_options(options);
    std::atomic<int> scenario{0};
    read_outcome outcome;

    server.post("/upload", [&](http::request& req, http::response& res) -> awaitable<void> {
        auto start = steady::now();
        if (scenario == 0) {
            req.set_read_deadline(start - 1s);
            // (checked on the test thread, through outcome.error: Catch is not thread-safe)
            co_await read_to_end(req, outcome, start);
            res.error(http::http_response::status::timed_out, "too slow");
            co_return;
        }
        req.set_read_deadline(start + 100ms);
        co_await sleep_for(300ms);
        req.set_read_deadline(steady::now() + 3s);
        uint8_t buffer[64];
        size_t total = 0;
        while (total < 10) {
            size_t bytes = co_await req.read_some(buffer, sizeof(buffer));
            if (bytes == 0) break;
            total += bytes;
        }
        outcome.bytes = total;
        outcome.error = req.get_body_error();
        outcome.done = true;
        res.send("ok");
    });
    running_server running(server);

    SECTION("Past already: the read fails at once") {
        scenario = 0;
        http::test::raw_connection connection(running.endpoint());
        REQUIRE(connection.write(post_head("/upload", ANNOUNCED)));
        REQUIRE(eventually([&] { return outcome.done.load(); }, 3s));
        check_elapsed(outcome, 0ms);
        CHECK(outcome.error.load() == http::body_error::timeout);
    }
    SECTION("Past while no read waited, then moved: the read goes on") {
        scenario = 1;
        http::test::raw_connection connection(running.endpoint());
        REQUIRE(connection.write(post_head("/upload", 10)));
        std::this_thread::sleep_for(500ms);
        REQUIRE(connection.write(std::string(10, 'x')));
        REQUIRE(eventually([&] { return outcome.done.load(); }, 3s));
        CHECK(outcome.error.load() == http::body_error::none);
        CHECK(outcome.bytes.load() == 10);
        std::string received;
        REQUIRE(connection.read_until(received, "\r\n\r\nok", 2s));
    }
}

// The deadline bounds the reads of the body only: a response still being written (a client that
// has not read it yet) is not cut when the read times out
TEST_CASE("A read deadline does not touch a response being written", "[server][deadline][body][integration]") {
    http::server server;
    http::connection_options options;
    options.linger_timeout = 200ms;
    server.set_connection_options(options);
    read_outcome outcome;
    constexpr size_t RESPONSE = 16 * 1024 * 1024;
    std::string body(RESPONSE, 'r');
    bool set_while_waiting = false;
    SECTION("Set before the read") {}
    SECTION("Set while the read waits") { set_while_waiting = true; }

    server.post("/upload", [&](http::request& req, http::response& res) -> awaitable<void> {
        auto start = steady::now();
        res.start_chunked("text/plain");
        res.write_chunk(body);
        if (set_while_waiting) {
            auto set = [&]() -> awaitable<void> {
                co_await sleep_for(100ms);
                req.set_read_deadline(start + 300ms);
                co_await sleep_for(5s);
            };
            co_await (read_to_end(req, outcome, start) || set());
        } else {
            req.set_read_deadline(start + 300ms);
            co_await read_to_end(req, outcome, start);
        }
        res.write_chunk("after");
        res.end_chunked();
    });
    running_server running(server);

    // A small receive buffer: the write above stays pending until the client reads
    http::test::raw_connection connection(running.endpoint(), false, 10s, 4096);
    REQUIRE(connection.write(post_head("/upload", ANNOUNCED)));
    REQUIRE(eventually([&] { return outcome.done.load(); }, 3s));
    CHECK(outcome.error.load() == http::body_error::timeout);
    check_elapsed(outcome, 300ms);

    auto result = connection.read_until_closed(5s);
    REQUIRE(result.closed);
    CHECK(result.data.starts_with("HTTP/1.1 200"));
    CHECK(result.data.find(std::string("0\r\n\r\n")) != std::string::npos);
    CHECK(result.data.find("after") != std::string::npos);
    CHECK(result.data.size() > RESPONSE);
}

// The body of an in-memory dispatch never waits, so the deadline never expires, even one past
TEST_CASE("A read deadline in memory never expires", "[server][deadline][body][dispatch]") {
    http::server server;
    bool failed = false;
    std::string read;
    server.post("/upload", [&](http::request& req, http::response& res) -> awaitable<void> {
        req.set_read_deadline(steady::now() - 1s);
        co_await sleep_for(10ms);
        uint8_t buffer[64];
        while (size_t bytes = co_await req.read_some(buffer, sizeof(buffer))) {
            read.append(reinterpret_cast<const char*>(buffer), bytes);
        }
        failed = req.get_body_error() != http::body_error::none;
        res.send("ok");
    });

    auto response = run_dispatch(server, make_request(http::method::POST, "/upload", "{\"a\":1}"));
    REQUIRE(response);
    CHECK(response->get_status_code() == 200);
    CHECK_FALSE(failed);
    CHECK(read == "{\"a\":1}");
}

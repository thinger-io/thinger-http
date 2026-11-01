#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>

#include "server_test_helpers.hpp"
#include "../fixtures/awaited_shutdown.hpp"
#include "../fixtures/handler_probe.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// The cancellation scope of a request on a connection (route::cancel_on, request::cancelled(),
// client_gone() and cancellation_slot()): a route declaring a reason has its handler interrupted
// for it within 50 ms (and every route at the shutdown deadline); a default route runs to its end
// and finds the reason. A client closing only its sending side after a complete request is not
// gone (it gets its response); one that resets the connection, or ends it in the middle of the
// body, is

using namespace thinger;
using namespace std::chrono_literals;
using http::cancel_reason;
using http::test::awaited_shutdown;
using http::test::handler_probe;
using http::test::interrupted_wait;
using http::test::raw_connection;
using http::test::waiting_route;
using server_test::running_server;
using server_test::transport;

namespace {

    using clock = std::chrono::steady_clock;

    // How long after its reason a handler that declared it sees operation_aborted, at most
    constexpr auto interruption_bound = 50ms;

    std::string get(const std::string& path) {
        return "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    }

    std::chrono::milliseconds ran_for(const handler_probe& probe) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(probe.end_time() - probe.start_time());
    }

    // The head of the response `client` reads first
    std::string response_head(raw_connection& client) {
        std::string received;
        REQUIRE(client.read_until(received, "\r\n\r\n", 3s));
        return received;
    }

    // A coroutine handler reading its body to its end (or failure), then waiting 300 ms: told to
    // `probe`, and whether reading the body failed to `body_failed`
    auto reading_handler(handler_probe& probe, std::atomic<bool>& body_failed) {
        return [&probe, &body_failed](http::request& req, http::response& res) -> awaitable<void> {
            probe.start();
            bool interrupted = false;
            try {
                std::vector<uint8_t> buffer(64 * 1024);
                while (co_await req.read_some(buffer.data(), buffer.size()) > 0) {}
                body_failed = req.get_body_error() != http::body_error::none;
                interrupted = co_await interrupted_wait(300ms);
            } catch (const boost::system::system_error& e) {
                // A coroutine cancelled throws at its next co_await
                interrupted = e.code() == boost::asio::error::operation_aborted;
            }
            probe.end(req, interrupted);
            res.send("done");
        };
    }

}

// --- A cancel_on route interrupts its handler (US1) ---

TEST_CASE("A cancel_on route interrupts its handler within 50 ms of a reset", "[cancellation][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    if (kind == transport::unix) SKIP("a Unix socket has no reset");
    handler_probe probe;
    http::server server;
    waiting_route(server, "/wait", probe, 10s).cancel_on({cancel_reason::client_gone});
    running_server running(server, kind);

    raw_connection client(running.endpoint());
    REQUIRE(client.write(get("/wait")));
    REQUIRE(probe.wait_started());
    auto reset_at = clock::now();
    client.reset();

    REQUIRE(probe.wait_ended());
    INFO("interrupted " << probe.ended_after(reset_at).count() << " ms after the reset");
    CHECK(probe.interrupted);
    CHECK(probe.reason == cancel_reason::client_gone);
    CHECK(probe.ended_after(reset_at) <= interruption_bound);
}

TEST_CASE("A cancel_on route interrupts its handler within 50 ms of its timeout", "[cancellation][timeout][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    handler_probe probe;
    http::server server;
    waiting_route(server, "/wait", probe, 10s).timeout(200ms).cancel_on({cancel_reason::timeout});
    running_server running(server, kind);

    raw_connection client(running.endpoint());
    // The timeout expires 200 ms after the request is read, so no earlier than this
    auto expiry = clock::now() + 200ms;
    REQUIRE(client.write(get("/wait")));
    // The client gets the 504 (answered before the handler is cancelled)
    CHECK(response_head(client).starts_with("HTTP/1.1 504"));

    REQUIRE(probe.wait_ended());
    INFO("interrupted " << probe.ended_after(expiry).count() << " ms after the timeout");
    CHECK(probe.interrupted);
    CHECK(probe.reason == cancel_reason::timeout);
    CHECK(ran_for(probe) >= 150ms);
    CHECK(probe.ended_after(expiry) <= interruption_bound);
}

TEST_CASE("A cancel_on route interrupts its handler within 50 ms of the shutdown deadline", "[cancellation][shutdown][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    handler_probe probe;
    http::server server;
    // Interrupted at the deadline whatever reason the route declared
    waiting_route(server, "/wait", probe, 10s).cancel_on({cancel_reason::client_gone});
    running_server running(server, kind);

    raw_connection client(running.endpoint());
    REQUIRE(client.write(get("/wait")));
    REQUIRE(probe.wait_started());

    auto by = clock::now() + 200ms;
    awaited_shutdown shutdown(server, by);
    REQUIRE(probe.wait_ended());
    INFO("interrupted " << probe.ended_after(by).count() << " ms after the deadline");
    CHECK(probe.interrupted);
    CHECK(probe.reason == cancel_reason::shutdown);
    CHECK(probe.end_time() >= by);
    CHECK(probe.ended_after(by) <= interruption_bound);

    // Then its connection is closed
    CHECK(client.read_until_closed(2s).closed);
    REQUIRE(shutdown.ended_within(2s));
    CHECK_FALSE(shutdown.get());
}

TEST_CASE("A cancel_on route goes on for a reason it did not declare", "[cancellation][timeout][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    handler_probe probe;
    http::server server;
    waiting_route(server, "/wait", probe, 400ms).timeout(100ms).cancel_on({cancel_reason::client_gone});
    running_server running(server, kind);

    raw_connection client(running.endpoint());
    REQUIRE(client.write(get("/wait")));
    CHECK(response_head(client).starts_with("HTTP/1.1 504"));

    REQUIRE(probe.wait_ended());
    CHECK_FALSE(probe.interrupted);
    CHECK(probe.reason == cancel_reason::timeout);
    CHECK(ran_for(probe) >= 390ms);
}

TEST_CASE("A cancel_on route runs work kept out of the cancellation state of its handler to its end",
          "[cancellation][timeout][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    // As route::cancel_on documents it: the cancellation state of the handler reset with
    // cancellation disabled, or a coroutine of its own, spawned with no slot
    bool reset_state = GENERATE(true, false);
    CAPTURE(reset_state);
    handler_probe probe;
    http::server server;
    server.get("/work", [&probe, reset_state](http::request& req, http::response& res) -> awaitable<void> {
        probe.start();
        bool interrupted;
        if (reset_state) {
            co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
            interrupted = co_await interrupted_wait(300ms);
        } else {
            interrupted = co_await co_spawn(co_await boost::asio::this_coro::executor, interrupted_wait(300ms),
                                            boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, use_awaitable));
        }
        probe.end(req, interrupted);
        res.send("done");
    }).timeout(100ms).cancel_on({cancel_reason::timeout});
    running_server running(server, kind);

    raw_connection client(running.endpoint());
    REQUIRE(client.write(get("/work")));
    CHECK(response_head(client).starts_with("HTTP/1.1 504"));

    REQUIRE(probe.wait_ended());
    CHECK_FALSE(probe.interrupted);
    CHECK(probe.reason == cancel_reason::timeout);
    CHECK(ran_for(probe) >= 290ms);
}

// --- A default route runs to its end, and observes its scope (US2; shutdown in shutdown_test) ---

TEST_CASE("A default route runs to its end past its timeout, and finds the reason", "[cancellation][timeout][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    handler_probe probe;
    http::server server;
    waiting_route(server, "/wait", probe, 300ms).timeout(100ms);
    running_server running(server, kind);

    raw_connection client(running.endpoint());
    REQUIRE(client.write(get("/wait")));
    CHECK(response_head(client).starts_with("HTTP/1.1 504"));

    REQUIRE(probe.wait_ended());
    CHECK_FALSE(probe.interrupted);
    CHECK(probe.reason == cancel_reason::timeout);
    CHECK(ran_for(probe) >= 290ms);
}

TEST_CASE("A default route runs to its end when its client resets", "[cancellation][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    if (kind == transport::unix) SKIP("a Unix socket has no reset");
    handler_probe probe;
    http::server server;
    waiting_route(server, "/wait", probe, 300ms);
    running_server running(server, kind);

    raw_connection client(running.endpoint());
    REQUIRE(client.write(get("/wait")));
    REQUIRE(probe.wait_started());
    client.reset();

    REQUIRE(probe.wait_ended());
    CHECK_FALSE(probe.interrupted);
    CHECK(probe.reason == cancel_reason::client_gone);
    CHECK(ran_for(probe) >= 290ms);
}

TEST_CASE("A default route handler bound to its cancellation slot is interrupted within 50 ms",
          "[cancellation][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    handler_probe probe;
    http::server server;
    waiting_route(server, "/reset", probe, 10s, true);
    waiting_route(server, "/timeout", probe, 10s, true).timeout(200ms);
    running_server running(server, kind);
    raw_connection client(running.endpoint());

    SECTION("By a reset") {
        if (kind == transport::unix) SKIP("a Unix socket has no reset");
        REQUIRE(client.write(get("/reset")));
        REQUIRE(probe.wait_started());
        auto reset_at = clock::now();
        client.reset();
        REQUIRE(probe.wait_ended());
        INFO("interrupted " << probe.ended_after(reset_at).count() << " ms after the reset");
        CHECK(probe.interrupted);
        CHECK(probe.reason == cancel_reason::client_gone);
        CHECK(probe.ended_after(reset_at) <= interruption_bound);
    }

    SECTION("By its timeout") {
        auto expiry = clock::now() + 200ms;
        REQUIRE(client.write(get("/timeout")));
        CHECK(response_head(client).starts_with("HTTP/1.1 504"));
        REQUIRE(probe.wait_ended());
        INFO("interrupted " << probe.ended_after(expiry).count() << " ms after the timeout");
        CHECK(probe.interrupted);
        CHECK(probe.reason == cancel_reason::timeout);
        CHECK(probe.ended_after(expiry) <= interruption_bound);
    }
}

TEST_CASE("client_gone() completes with the reason once the scope is cancelled", "[cancellation][integration][matrix]") {
    using namespace boost::asio::experimental::awaitable_operators;
    MATRIX_TRANSPORT(kind);
    handler_probe probe;
    std::atomic<cancel_reason> returned{cancel_reason::none};
    std::atomic<bool> completed{false};
    http::server server;
    auto awaiting_gone = [&](http::request& req, http::response& res) -> awaitable<void> {
        probe.start();
        returned = co_await req.client_gone();
        probe.end(req, false);
        res.send("done");
    };
    server.get("/reset", awaiting_gone);
    server.get("/timeout", awaiting_gone).timeout(100ms);
    // Never cancelled: it does not complete (the timer ends the wait)
    server.get("/quiet", [&](http::request& req, http::response& res) -> awaitable<void> {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 150ms);
        auto result = co_await (req.client_gone() || timer.async_wait(use_awaitable));
        completed = result.index() == 0;
        res.send(completed ? "completed" : "quiet");
    });
    running_server running(server, kind);
    raw_connection client(running.endpoint());

    SECTION("By a reset") {
        if (kind == transport::unix) SKIP("a Unix socket has no reset");
        REQUIRE(client.write(get("/reset")));
        REQUIRE(probe.wait_started());
        client.reset();
        REQUIRE(probe.wait_ended());
        CHECK(returned == cancel_reason::client_gone);
        CHECK(probe.reason == cancel_reason::client_gone);
    }

    SECTION("By its timeout") {
        REQUIRE(client.write(get("/timeout")));
        CHECK(response_head(client).starts_with("HTTP/1.1 504"));
        REQUIRE(probe.wait_ended());
        CHECK(returned == cancel_reason::timeout);
        CHECK(probe.reason == cancel_reason::timeout);
    }

    SECTION("Not cancelled, it does not complete") {
        REQUIRE(client.write(get("/quiet")));
        std::string received;
        CHECK(client.read_until(received, "quiet", 3s));
        CHECK_FALSE(completed);
    }
}

TEST_CASE("cancelled() is none for a request whose scope is not cancelled", "[cancellation][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    handler_probe probe;
    http::server server;
    waiting_route(server, "/wait", probe, 20ms, GENERATE(false, true));
    running_server running(server, kind);

    raw_connection client(running.endpoint());
    REQUIRE(client.write(get("/wait")));
    std::string received;
    CHECK(client.read_until(received, "done", 3s));
    CHECK(received.starts_with("HTTP/1.1 200"));
    REQUIRE(probe.wait_ended());
    CHECK_FALSE(probe.interrupted);
    CHECK(probe.reason == cancel_reason::none);
}

// --- Edge cases: a scope cancelled twice, and after its handler ended ---

TEST_CASE("A scope is cancelled once: the first reason stays", "[cancellation][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    if (kind == transport::unix) SKIP("a Unix socket has no reset");
    handler_probe probe;
    std::atomic<int> emitted{0};
    http::server server;
    server.get("/twice", [&](http::request& req, http::response& res) -> awaitable<void> {
        probe.start();
        req.cancellation_slot().assign([&emitted](boost::asio::cancellation_type) { ++emitted; });
        bool interrupted = co_await interrupted_wait(400ms);
        req.cancellation_slot().clear();
        probe.end(req, interrupted);
        res.send("done");
    }).timeout(100ms);
    running_server running(server, kind);

    // The timeout, then a reset
    raw_connection client(running.endpoint());
    REQUIRE(client.write(get("/twice")));
    CHECK(response_head(client).starts_with("HTTP/1.1 504"));
    client.reset();

    REQUIRE(probe.wait_ended());
    CHECK(probe.reason == cancel_reason::timeout);
    CHECK(emitted == 1);
    CHECK_FALSE(probe.interrupted);
}

TEST_CASE("A cancellation after the handler ended emits nothing", "[cancellation][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    std::atomic<int> emitted{0};
    std::mutex mutex;
    std::optional<http::response> kept;
    http::server server;
    // The handler ends without answering, keeping a copy of the response: its timeout comes later
    server.get("/late", [&](http::request& req, http::response& res) {
        req.cancellation_slot().assign([&emitted](boost::asio::cancellation_type) { ++emitted; });
        std::scoped_lock lock(mutex);
        kept.emplace(res);
    }).timeout(150ms);
    running_server running(server, kind);

    raw_connection client(running.endpoint());
    REQUIRE(client.write(get("/late")));
    CHECK(response_head(client).starts_with("HTTP/1.1 504"));
    // And the client leaves
    client.reset();
    std::this_thread::sleep_for(100ms);
    CHECK(emitted == 0);
    std::scoped_lock lock(mutex);
    kept.reset();
}

// --- A half-close after a complete request is no leaving; EOF in the middle of the body is (US3) ---

TEST_CASE("A client closing its sending side after a complete request gets its response",
          "[cancellation][half_close][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    handler_probe probe;
    http::server server;
    waiting_route(server, "/default", probe, 200ms);
    waiting_route(server, "/declared", probe, 200ms).cancel_on({cancel_reason::client_gone});
    running_server running(server, kind);
    auto path = GENERATE(std::string("/default"), std::string("/declared"));
    INFO("route " << path);

    raw_connection client(running.endpoint());
    REQUIRE(client.write(get(path)));
    // Over TLS, a FIN without a close_notify (see the next test)
    client.shutdown_send();

    auto response = client.read_until_closed(3s);
    INFO("received: " << response.data);
    CHECK(response.closed);
    CHECK(response.data.starts_with("HTTP/1.1 200"));
    CHECK(response.data.find("Connection: Close") != std::string::npos);
    CHECK(response.data.ends_with("done"));
    REQUIRE(probe.wait_ended());
    CHECK_FALSE(probe.interrupted);
    CHECK(probe.reason == cancel_reason::none);
}

TEST_CASE("A TLS client sending its close_notify after a complete request gets its response",
          "[cancellation][half_close][tls][integration]") {
    handler_probe probe;
    http::server server;
    waiting_route(server, "/declared", probe, 200ms).cancel_on({cancel_reason::client_gone});
    running_server running(server, transport::tls);

    raw_connection client(running.endpoint());
    // The session tickets of TLS 1.3 read first (see raw_connection::close_notify())
    std::string tickets;
    client.read_some(tickets, 100ms);
    REQUIRE(tickets.empty());
    REQUIRE(client.write(get("/declared")));
    client.close_notify();

    auto response = client.read_until_closed(3s);
    INFO("received: " << response.data);
    CHECK(response.closed);
    CHECK(response.data.starts_with("HTTP/1.1 200"));
    CHECK(response.data.find("Connection: Close") != std::string::npos);
    CHECK(response.data.ends_with("done"));
    REQUIRE(probe.wait_ended());
    CHECK_FALSE(probe.interrupted);
    CHECK(probe.reason == cancel_reason::none);
}

TEST_CASE("A response written after the client reset its half-closed connection cancels with client_gone",
          "[cancellation][half_close][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    if (kind == transport::unix) SKIP("a Unix socket has no reset");
    handler_probe probe;
    http::server server;
    server.get("/late", [&](http::request& req, http::response& res) -> awaitable<void> {
        probe.start();
        co_await interrupted_wait(200ms);
        res.send("done");
        // Its write fails meanwhile
        bool interrupted = co_await interrupted_wait(300ms);
        probe.end(req, interrupted);
    });
    running_server running(server, kind);

    raw_connection client(running.endpoint());
    REQUIRE(client.write(get("/late")));
    client.shutdown_send();
    REQUIRE(probe.wait_started());
    // The server reads the FIN, then the reset
    std::this_thread::sleep_for(50ms);
    client.reset();

    REQUIRE(probe.wait_ended());
    CHECK_FALSE(probe.interrupted);
    CHECK(probe.reason == cancel_reason::client_gone);
}

TEST_CASE("A client ending its connection in the middle of the body is gone", "[cancellation][body][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    bool reset = GENERATE(false, true);
    INFO((reset ? "a reset" : "a FIN"));
    if (reset && kind == transport::unix) SKIP("a Unix socket has no reset");
    handler_probe probe;
    std::atomic<bool> body_failed{false};
    http::server server;
    server.post("/default", reading_handler(probe, body_failed));
    server.post("/declared", reading_handler(probe, body_failed)).cancel_on({cancel_reason::client_gone});
    running_server running(server, kind);
    bool declared = GENERATE(false, true);
    INFO((declared ? "cancel_on(client_gone)" : "default route"));

    // 1 MiB announced, half of it sent
    raw_connection client(running.endpoint());
    REQUIRE(client.write(std::string("POST ") + (declared ? "/declared" : "/default") +
                         " HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1048576\r\n\r\n"));
    REQUIRE(probe.wait_started());
    REQUIRE(client.write(std::string(512 * 1024, 'a')));
    if (reset) {
        client.reset();
    } else {
        client.shutdown_send();
    }

    REQUIRE(probe.wait_ended());
    CHECK(probe.reason == cancel_reason::client_gone);
    // Interrupted only where the route declared it
    CHECK(probe.interrupted == declared);
    if (!declared) CHECK(body_failed);
}

TEST_CASE("Pipelined requests are served, what follows a request kept for the next one",
          "[cancellation][pipelining][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    handler_probe probe;
    http::server server;
    waiting_route(server, "/slow", probe, 150ms).cancel_on({cancel_reason::client_gone});
    server.get("/fast", [](http::response& res) { res.send("fast"); });
    running_server running(server, kind);
    raw_connection client(running.endpoint());

    SECTION("Sent along with the request") {
        REQUIRE(client.write(get("/slow") + get("/fast")));
    }

    SECTION("Sent while the request is handled, read by the connection watching its client") {
        REQUIRE(client.write(get("/slow")));
        REQUIRE(probe.wait_started());
        std::this_thread::sleep_for(20ms);
        REQUIRE(client.write(get("/fast")));
    }

    std::string received;
    REQUIRE(client.read_until(received, "fast", 3s));
    CHECK(received.find("done") < received.find("fast"));
    CHECK(server_test::count(received, "HTTP/1.1 200") == 2);
    REQUIRE(probe.wait_ended());
    CHECK_FALSE(probe.interrupted);
    CHECK(probe.reason == cancel_reason::none);
}

TEST_CASE("The connection watches the client of each pipelined request in turn",
          "[cancellation][pipelining][integration][matrix]") {
    // One answered at once (the client is not watched for it), then two whose handlers yield,
    // sent together: the client is watched for the second only once the first was answered,
    // and its reset cancels only the scope of the second
    MATRIX_TRANSPORT(kind);
    if (kind == transport::unix) SKIP("a Unix socket has no reset");
    handler_probe first;
    handler_probe second;
    http::server server;
    server.get("/fast", [](http::response& res) { res.send("fast"); });
    waiting_route(server, "/first", first, 100ms).cancel_on({cancel_reason::client_gone});
    waiting_route(server, "/second", second, 10s).cancel_on({cancel_reason::client_gone});
    running_server running(server, kind);
    raw_connection client(running.endpoint());

    REQUIRE(client.write(get("/fast") + get("/first") + get("/second")));
    REQUIRE(second.wait_started());
    std::string received;
    REQUIRE(client.read_until(received, "done", 3s));
    CHECK(received.find("fast") < received.find("done"));
    REQUIRE(first.wait_ended());
    CHECK_FALSE(first.interrupted);
    CHECK(first.reason == cancel_reason::none);

    client.reset();
    REQUIRE(second.wait_ended());
    CHECK(second.interrupted);
    CHECK(second.reason == cancel_reason::client_gone);
}

TEST_CASE("A body read by its handler is not read by the connection meanwhile", "[cancellation][body][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    handler_probe probe;
    http::server server;
    // The body arrives in pieces while the handler reads it, and the next request right after it
    server.post("/upload", [&](http::request& req, http::response& res) -> awaitable<void> {
        probe.start();
        uint8_t buffer[512];
        size_t total = 0;
        while (auto bytes = co_await req.read_some(buffer, sizeof(buffer))) total += bytes;
        probe.end(req, false);
        res.send("got " + std::to_string(total));
    }).cancel_on({cancel_reason::client_gone});
    server.get("/fast", [](http::response& res) { res.send("fast"); });
    running_server running(server, kind);

    raw_connection client(running.endpoint());
    REQUIRE(client.write("POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3000\r\n\r\n"));
    REQUIRE(probe.wait_started());
    for (int piece = 0; piece < 2; ++piece) {
        REQUIRE(client.write(std::string(1000, 'a')));
        std::this_thread::sleep_for(30ms);
    }
    REQUIRE(client.write(std::string(1000, 'a') + get("/fast")));

    std::string received;
    REQUIRE(client.read_until(received, "fast", 3s));
    CHECK(received.find("got 3000") < received.find("fast"));
    REQUIRE(probe.wait_ended());
    CHECK(probe.reason == cancel_reason::none);
}

// A server stopped or destroyed while its connections live, on workers that outlive it: from the
// application's thread (US1), from inside one of its own handlers (US2), and listening again
// afterwards (US3). Once stop() or the destructor returned, its connections are closed and the
// requests in flight cancelled (cancel_reason::shutdown), without waiting for their handlers: a
// handler still running finishes against the serving state it keeps alive, and nothing touches
// freed memory (the tests run under ASan and TSan)
#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/pool_server.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/websocket_connection.hpp>

#include "server_test_helpers.hpp"
#include "shutdown_test_helpers.hpp"
#include "../fixtures/handler_probe.hpp"
#include "../fixtures/running_workers.hpp"
#include "../fixtures/stream_test_helpers.hpp"
#include "../fixtures/transport_matrix.hpp"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

using namespace thinger;
using namespace std::chrono_literals;
using shutdown_test::eventually;
using shutdown_test::raw_client;
using clock_type = std::chrono::steady_clock;

namespace {

// Ends the test program if the test is still running after `limit`: a deadlock fails the test
// instead of hanging it
class watchdog {
public:
    explicit watchdog(std::chrono::seconds limit) {
        thread_ = std::thread([this, limit] {
            std::unique_lock lock(mutex_);
            if (!done_.wait_for(lock, limit, [this] { return disarmed_; })) {
                std::fprintf(stderr, "watchdog: the test is still running after %llds, aborting\n",
                             static_cast<long long>(limit.count()));
                std::abort();
            }
        });
    }

    ~watchdog() {
        {
            std::lock_guard lock(mutex_);
            disarmed_ = true;
        }
        done_.notify_all();
        thread_.join();
    }

    watchdog(const watchdog&) = delete;
    watchdog& operator=(const watchdog&) = delete;

private:
    std::mutex mutex_;
    std::condition_variable done_;
    bool disarmed_ = false;
    std::thread thread_;
};

const std::string second_request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";

// How long `action` took
template<typename Action>
std::chrono::milliseconds timed(Action&& action) {
    auto begin = clock_type::now();
    action();
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - begin);
}

// A client of `endpoint` with a keep-alive GET / answered: an idle keep-alive connection
std::unique_ptr<raw_client> idle_client(const http::test::transport_endpoint& endpoint) {
    auto client = std::make_unique<raw_client>(endpoint);
    client->get("/");
    REQUIRE(client->read_until("hi"));
    REQUIRE(client->received.find("Connection: Keep-Alive") != std::string::npos);
    return client;
}

// Whether the second request on the idle keep-alive connection of `client` is left unanswered,
// the connection seen closed within 1 s
bool closed_unanswered(raw_client& client) {
    client.connection.write(second_request, 1s);
    auto rest = client.rest(1s);
    return rest.closed && rest.data.empty();
}

} // namespace

// --- From the application's thread (US1) ---

TEST_CASE("Destroying a pool server closes its idle keep-alive connections", "[server][lifetime][matrix]") {
    MATRIX_TRANSPORT(kind);
    watchdog guard(30s);
    http::test::running_workers workers;
    auto server = std::make_unique<http::pool_server>(workers);
    std::atomic<int> served{0};
    server->get("/", [&served](http::response& res) {
        ++served;
        res.send("hi");
    });
    http::test::transport_listener listener(*server, kind);

    auto client = idle_client(listener.endpoint());

    server.reset();

    // The second request on the same connection is never served: the connection is closed (a
    // write to it may already fail)
    REQUIRE(closed_unanswered(*client));
    REQUIRE(served == 1);
}

TEST_CASE("Stopping a pool server closes its idle keep-alive connections", "[server][lifetime]") {
    watchdog guard(30s);
    std::atomic<int> served{0};
    http::test::running_workers workers;
    http::pool_server server(workers);
    server.get("/", [&served](http::response& res) {
        ++served;
        res.send("hi");
    });
    http::test::transport_listener listener(server, http::test::transport::tcp);
    auto client = idle_client(listener.endpoint());
    REQUIRE(server.stats().active_connections() == 1);

    REQUIRE(timed([&] { server.stop(); }) < 2s);

    // The server lives on, without the connection
    REQUIRE(closed_unanswered(*client));
    REQUIRE(served == 1);
    REQUIRE(eventually([&] { return server.stats().active_connections() == 0; }, 1s));
}

TEST_CASE("Destroying a pool server cancels a handler awaiting, without waiting for it", "[server][lifetime]") {
    watchdog guard(30s);
    http::test::handler_probe probe;
    std::atomic<int> hooked{0};
    http::test::running_workers workers;
    auto server = std::make_unique<http::pool_server>(workers);
    // A route not declaring cancel_on, and a response hook, read by the request once it ends
    http::test::waiting_route(*server, "/wait", probe, 10s);
    server->on_response([&hooked](const http::request&, const http::response_info&) { ++hooked; });
    http::test::transport_listener listener(*server, http::test::transport::tcp);

    raw_client client(listener.endpoint());
    client.get("/wait");
    REQUIRE(probe.wait_started());

    REQUIRE(timed([&] { server.reset(); }) < 2s);

    REQUIRE(client.rest(1s).closed);
    // Its wait ends, interrupted by the destruction: the handler finishes, the hook too, against
    // the serving state they keep
    REQUIRE(probe.wait_ended(2s));
    CHECK(probe.interrupted);
    CHECK(probe.reason == http::cancel_reason::shutdown);
    REQUIRE(eventually([&] { return hooked == 1; }, 2s));
}

TEST_CASE("Destroying a pool server while a handler blocks its thread does not wait for it", "[server][lifetime]") {
    watchdog guard(30s);
    std::atomic<bool> blocking{false};
    std::atomic<bool> step_ended{false};
    http::test::running_workers workers;
    auto server = std::make_unique<http::pool_server>(workers);
    server->get("/block", [&](http::response& res) {
        blocking = true;
        std::this_thread::sleep_for(500ms);   // synchronous: nothing interrupts it
        step_ended = true;
        res.send("late");
    });
    http::test::transport_listener listener(*server, http::test::transport::tcp);

    raw_client client(listener.endpoint());
    client.get("/block");
    REQUIRE(eventually([&] { return blocking.load(); }));

    auto took = timed([&] { server.reset(); });
    INFO("destroyed in " << took.count() << " ms");
    CHECK(took < 400ms);
    CHECK_FALSE(step_ended);

    // The step ends on its own, and its response goes to a connection closed meanwhile (what
    // was written before the close may arrive)
    REQUIRE(eventually([&] { return step_ended.load(); }, 2s));
    REQUIRE(client.rest(1s).closed);
}

TEST_CASE("Destroying a pool server with a route timeout pending, or expired with its handler running",
          "[server][lifetime]") {
    watchdog guard(30s);
    http::test::handler_probe probe;
    http::test::running_workers workers;
    auto server = std::make_unique<http::pool_server>(workers);
    // Served through serve_with_timeout: the handler, and the timer of the route
    http::test::waiting_route(*server, "/slow", probe, 1s).timeout(200ms);
    http::test::transport_listener listener(*server, http::test::transport::tcp);
    auto expired = GENERATE(false, true);
    INFO((expired ? "after the route timeout" : "before the route timeout"));

    raw_client client(listener.endpoint());
    client.get("/slow");
    REQUIRE(probe.wait_started());
    if (expired) {
        // The 504 is sent at 200 ms, the handler goes on (its route did not declare the timeout)
        REQUIRE(client.read_until("HTTP/1.1 504", 2s));
        std::this_thread::sleep_until(probe.start_time() + 300ms);
        REQUIRE_FALSE(probe.ended);
    } else {
        std::this_thread::sleep_until(probe.start_time() + 50ms);
    }

    REQUIRE(timed([&] { server.reset(); }) < 2s);

    auto rest = client.rest(1s);
    REQUIRE(rest.closed);
    REQUIRE(probe.wait_ended(2s));
    if (!expired) {
        // Interrupted, and neither the timeout nor the handler answered
        CHECK(probe.interrupted);
        CHECK(probe.reason == http::cancel_reason::shutdown);
        CHECK(rest.data.empty());
    }
    // After the timeout, its scope was cancelled already (with a reason its route did not
    // declare): the handler runs its wait to the end, against the serving state it keeps
}

TEST_CASE("Destroying a pool server after a shutdown left a handler running", "[server][lifetime][matrix]") {
    MATRIX_TRANSPORT(kind);
    watchdog guard(30s);
    std::atomic<bool> aborted{false};
    std::atomic<bool> finished{false};
    http::test::running_workers workers;
    auto server = std::make_unique<http::pool_server>(workers);
    // Interrupted at the deadline of the shutdown (B3), it goes on awaiting 1 s more, out of its
    // cancellation state
    server->get("/stubborn", [&](http::response& res) -> awaitable<void> {
        aborted = co_await http::test::interrupted_wait(10s);
        co_await boost::asio::this_coro::reset_cancellation_state();
        co_await boost::asio::this_coro::throw_if_cancelled(false);
        boost::asio::cancellation_signal never;
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 1s);
        co_await timer.async_wait(boost::asio::bind_cancellation_slot(never.slot(), use_nothrow_awaitable));
        finished = true;
        res.send("late");
    });
    http::test::transport_listener listener(*server, kind);

    raw_client client(listener.endpoint());
    client.get("/stubborn");
    REQUIRE(eventually([&] { return server->stats().http_connections == 1; }));
    std::this_thread::sleep_for(50ms);   // the handler awaits

    REQUIRE_FALSE(server->shutdown(100ms));
    REQUIRE(eventually([&] { return aborted.load(); }, 1s));
    REQUIRE_FALSE(finished);

    REQUIRE(timed([&] { server.reset(); }) < 2s);

    // The handler ends its wait, then answers to a connection closed by the shutdown
    REQUIRE(client.rest(1s).closed);
    REQUIRE(eventually([&] { return finished.load(); }, 3s));
}

TEST_CASE("Destroying a pool server closes its WebSockets", "[server][lifetime][websocket]") {
    watchdog guard(30s);
    http::test::running_workers workers;
    auto server = std::make_unique<http::pool_server>(workers);
    server->get("/ws", [](http::response& res) {
        res.upgrade_websocket({}, [](std::shared_ptr<http::websocket_connection> ws) {
            ws->on_message([](std::string, bool) {});
        });
    });
    http::test::transport_listener listener(*server, http::test::transport::tcp);

    stream_test::raw_ws_client client(listener.endpoint());
    REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
    REQUIRE(eventually([&] { return server->stats().websocket_connections == 1; }));

    REQUIRE(timed([&] { server.reset(); }) < 2s);

    REQUIRE(client.closed_within(1s));
}

TEST_CASE("Destroying a pool server on workers already stopped returns at once", "[server][lifetime]") {
    watchdog guard(30s);
    std::optional<http::test::running_workers> workers;
    workers.emplace();
    auto server = std::make_unique<http::pool_server>(*workers);
    server->get("/", [](http::response& res) { res.send("hi"); });
    http::test::transport_listener listener(*server, http::test::transport::tcp);
    auto client = idle_client(listener.endpoint());

    // The wrong order: nothing runs the connections any more
    workers->stop();
    REQUIRE(timed([&] { server.reset(); }) < 2s);
    // Destroying the workers destroys what was left on them, the connection too
    workers.reset();
    REQUIRE(client->rest(1s).closed);
}

TEST_CASE("Stopping and destroying a pool server that never served return at once", "[server][lifetime]") {
    watchdog guard(30s);
    SECTION("workers never started") {
        asio::workers idle;
        auto server = std::make_unique<http::pool_server>(idle);
        server->get("/", [](http::response& res) { res.send("hi"); });
        REQUIRE(timed([&] { server->stop(); }) < 2s);
        REQUIRE(timed([&] { server.reset(); }) < 2s);
    }
    SECTION("server never listened") {
        http::test::running_workers workers;
        auto server = std::make_unique<http::pool_server>(workers);
        server->get("/", [](http::response& res) { res.send("hi"); });
        REQUIRE(timed([&] { server->stop(); }) < 2s);
        REQUIRE(timed([&] { server.reset(); }) < 2s);
    }
}

// --- From inside one of its own handlers (US2) ---

TEST_CASE("Destroying a pool server from one of its handlers, on one worker thread", "[server][lifetime]") {
    watchdog guard(30s);
    auto sends = GENERATE(false, true);
    INFO((sends ? "the handler sends after it" : "the handler returns after it"));
    std::atomic<bool> destroyed{false};
    std::atomic<long long> took{-1};
    http::test::running_workers workers(1);
    auto server = std::make_unique<http::pool_server>(workers);
    server->get("/", [](http::response& res) { res.send("hi"); });
    // On the only thread, which must run nothing the destructor would wait for
    server->get("/quit", [&](http::response& res) {
        took = timed([&] { server.reset(); }).count();
        destroyed = true;
        if (sends) res.send("bye");
    });
    http::test::transport_listener listener(*server, http::test::transport::tcp);
    auto other = idle_client(listener.endpoint());

    raw_client client(listener.endpoint());
    client.get("/quit");
    REQUIRE(eventually([&] { return destroyed.load(); }));
    CHECK(took < 2000);

    REQUIRE(closed_unanswered(*other));
    // A response is not required
    REQUIRE(client.rest(1s).closed);
}

TEST_CASE("Destroying a pool server from a handler while another awaits on another worker", "[server][lifetime]") {
    watchdog guard(30s);
    http::test::handler_probe probe;
    std::thread::id waiting_on;   // written before probe.start(), read once it started
    std::atomic<bool> other_worker{false};
    std::atomic<bool> destroyed{false};
    std::atomic<long long> took{-1};
    http::test::running_workers workers(4);
    auto server = std::make_unique<http::pool_server>(workers);
    server->get("/wait", [&](http::request& req, http::response& res) -> awaitable<void> {
        waiting_on = std::this_thread::get_id();
        probe.start();
        bool interrupted = co_await http::test::interrupted_wait(10s);
        probe.end(req, interrupted);
        res.send("done");
    });
    server->get("/quit", [&](http::response& res) {
        // (reading `started` first: what the other handler wrote before it is seen)
        other_worker = probe.started && std::this_thread::get_id() != waiting_on;
        took = timed([&] { server.reset(); }).count();
        destroyed = true;
        res.send("bye");
    });
    http::test::transport_listener listener(*server, http::test::transport::tcp);

    raw_client waiting(listener.endpoint());
    waiting.get("/wait");
    REQUIRE(probe.wait_started());
    // The next connection goes to the next worker
    raw_client client(listener.endpoint());
    client.get("/quit");
    REQUIRE(eventually([&] { return destroyed.load(); }));
    CHECK(other_worker);
    CHECK(took < 2000);

    // The wait is interrupted, and ends against the serving state the handler keeps
    REQUIRE(waiting.rest(1s).closed);
    REQUIRE(probe.wait_ended(2s));
    CHECK(probe.interrupted);
    CHECK(probe.reason == http::cancel_reason::shutdown);
    REQUIRE(client.rest(1s).closed);
}

TEST_CASE("Stopping a pool server from one of its handlers", "[server][lifetime]") {
    watchdog guard(30s);
    auto threads = GENERATE(size_t{1}, size_t{4});
    INFO(threads << " worker threads");
    std::atomic<bool> stopped{false};
    std::atomic<long long> took{-1};
    http::test::running_workers workers(threads);
    auto server = std::make_unique<http::pool_server>(workers);
    server->get("/", [](http::response& res) { res.send("hi"); });
    server->get("/stop", [&](http::response& res) {
        took = timed([&] { server->stop(); }).count();
        stopped = true;
        res.send("bye");
    });
    http::test::transport_listener listener(*server, http::test::transport::tcp);
    auto other = idle_client(listener.endpoint());

    raw_client client(listener.endpoint());
    client.get("/stop");
    REQUIRE(eventually([&] { return stopped.load(); }));
    CHECK(took < 2000);

    REQUIRE(closed_unanswered(*other));
    REQUIRE(client.rest(1s).closed);
    REQUIRE(eventually([&] { return server->stats().active_connections() == 0; }, 1s));
    // Destroyed later, from the test thread
    REQUIRE(timed([&] { server.reset(); }) < 2s);
}

TEST_CASE("Two handlers stopping their pool server at once", "[server][lifetime]") {
    watchdog guard(30s);
    std::atomic<int> arrived{0};
    std::atomic<int> together{0};
    std::atomic<int> returned{0};
    std::atomic<long long> slowest{0};
    http::test::running_workers workers(4);
    http::pool_server server(workers);
    server.get("/stop", [&](http::response& res) {
        // Both stop it at the same time, each on its worker
        ++arrived;
        auto deadline = clock_type::now() + 2s;
        while (arrived < 2 && clock_type::now() < deadline) std::this_thread::yield();
        if (arrived == 2) ++together;
        long long took = timed([&] { server.stop(); }).count();
        for (auto seen = slowest.load(); seen < took && !slowest.compare_exchange_weak(seen, took);) {}
        ++returned;
        res.send("bye");
    });
    http::test::transport_listener listener(server, http::test::transport::tcp);

    raw_client first(listener.endpoint());
    raw_client second(listener.endpoint());
    first.get("/stop");
    second.get("/stop");
    REQUIRE(eventually([&] { return returned == 2; }, 5s));
    CHECK(together == 2);
    CHECK(slowest < 2000);

    REQUIRE(first.rest(1s).closed);
    REQUIRE(second.rest(1s).closed);
}

// --- Listening again, and a shutdown (US3) ---

TEST_CASE("A pool server stopped serves again, never its old connections", "[server][lifetime]") {
    watchdog guard(30s);
    std::atomic<int> served{0};
    http::test::running_workers workers;
    http::pool_server server(workers);
    server.get("/", [&served](http::response& res) {
        ++served;
        res.send("hi");
    });
    http::test::transport_listener first(server, http::test::transport::tcp);
    auto old = idle_client(first.endpoint());

    server.stop();
    // At once, the closes of stop() maybe still pending
    http::test::transport_listener again(server, http::test::transport::tcp);
    auto fresh = idle_client(again.endpoint());

    REQUIRE(closed_unanswered(*old));
    REQUIRE(served == 2);
    // The new connection is served as any other
    fresh->received.clear();
    fresh->get("/");
    REQUIRE(fresh->read_until("hi"));
    REQUIRE(served == 3);
}

TEST_CASE("Stopping a pool server during a shutdown ends it, and closes its connections", "[server][lifetime]") {
    watchdog guard(30s);
    http::test::handler_probe probe;
    http::test::running_workers workers;
    http::pool_server server(workers);
    http::test::waiting_route(server, "/wait", probe, 10s);
    http::test::transport_listener listener(server, http::test::transport::tcp);

    raw_client client(listener.endpoint());
    client.get("/wait");
    REQUIRE(probe.wait_started());

    // Draining: the connection stays open until its handler ends
    auto shutdown = std::async(std::launch::async, [&] { return server.shutdown(30s); });
    std::this_thread::sleep_for(50ms);
    REQUIRE(shutdown.wait_for(0s) == std::future_status::timeout);

    REQUIRE(timed([&] { server.stop(); }) < 2s);
    REQUIRE(shutdown.wait_for(1s) == std::future_status::ready);
    REQUIRE_FALSE(shutdown.get());

    // Closed, not left as it was
    REQUIRE(client.rest(1s).closed);
    REQUIRE(probe.wait_ended(2s));
    CHECK(probe.interrupted);
    CHECK(probe.reason == http::cancel_reason::shutdown);
}

TEST_CASE("A standalone server stopped never serves its old connections when it runs again", "[server][lifetime]") {
    watchdog guard(30s);
    std::atomic<int> served{0};
    http::server server;
    server.get("/", [&served](http::response& res) {
        ++served;
        res.send("hi");
    });
    std::optional<server_test::running_server> running;
    running.emplace(server);
    auto old = idle_client(running->endpoint());

    // wait() returns, and its io_context stops: the closes of stop() may not have run yet
    server.stop();
    running->thread.join();
    REQUIRE_FALSE(server.is_listening());

    // Listening and running again
    running.reset();
    running.emplace(server);
    REQUIRE(closed_unanswered(*old));
    auto fresh = idle_client(running->endpoint());
    REQUIRE(served == 2);
}

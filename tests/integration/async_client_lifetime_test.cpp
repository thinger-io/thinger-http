// An async_client stopped or destroyed with requests in flight, while its workers run: from the
// application's thread (US1), from inside one of its own completions (US2), and in the less
// common states (US3 and the edge cases). Nothing of the client runs once stop() or the
// destructor returned: no callback, no coroutine of run()
#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/async_client.hpp>
#include <thinger/http/client/websocket_client.hpp>
#include <thinger/http/server/websocket_connection.hpp>
#include "../fixtures/server_listen.hpp"
#include "../fixtures/test_server_fixture.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace thinger;
using namespace std::chrono_literals;
using tcp = boost::asio::ip::tcp;
using http::test::running_workers;

namespace {

// A server that accepts every connection and never sends a byte (over TLS too, a peer that never
// answers the handshake), or, with `answer`, answers every request with a 200 and keeps the
// connection open: it counts the connections accepted, and those the client closed
struct silent_server {
    boost::asio::io_context context;
    tcp::acceptor acceptor{context, {boost::asio::ip::make_address("127.0.0.1"), 0}};
    std::atomic<int> accepted{0};
    std::atomic<int> closed{0};
    const bool answer;
    // Touched only on the server's thread
    std::list<tcp::socket> peers;
    std::array<char, 4096> discard{};
    std::thread thread;

    explicit silent_server(bool answer = false) : answer(answer) {
        accept();
        thread = std::thread([this] { context.run(); });
    }

    ~silent_server() {
        boost::asio::post(context, [this] { context.stop(); });
        thread.join();
    }

    std::string url(const std::string& path = "/", const std::string& scheme = "http") const {
        return scheme + "://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + path;
    }

    // Whether every connection accepted was closed by the client
    bool all_closed() const { return closed.load() == accepted.load(); }

private:
    void accept() {
        acceptor.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (ec) return;
            auto& peer = peers.emplace_back(std::move(socket));
            ++accepted;
            read(peer);
            accept();
        });
    }

    // Read the requests (small ones, read whole), answering them or not, until the client
    // closes the connection
    void read(tcp::socket& peer) {
        peer.async_read_some(boost::asio::buffer(discard), [this, &peer](boost::system::error_code ec, size_t size) {
            if (ec) {
                ++closed;
                return;
            }
            if (answer && std::string_view(discard.data(), size).find("\r\n\r\n") != std::string_view::npos) {
                static constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
                boost::asio::async_write(peer, boost::asio::buffer(response), [](boost::system::error_code, size_t) {});
            }
            read(peer);
        });
    }
};

// Poll `condition` every 5 ms until it holds or `deadline` passes: whether it held
template<typename Condition>
bool eventually(Condition condition, std::chrono::milliseconds deadline) {
    auto end = std::chrono::steady_clock::now() + deadline;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

// Run a marker on every thread of the pool and wait for them, within `deadline`: whatever was
// queued on the workers before has run, or was dropped
bool drain(asio::workers& workers, size_t threads, std::chrono::milliseconds deadline = 2s) {
    std::set<boost::asio::io_context*> contexts;
    for (size_t i = 0; contexts.size() < threads && i < threads * 16; ++i) {
        contexts.insert(&workers.get_next_io_context());
    }
    std::vector<std::future<void>> markers;
    for (auto* context : contexts) {
        auto marker = std::make_shared<std::promise<void>>();
        markers.push_back(marker->get_future());
        boost::asio::post(*context, [marker] { marker->set_value(); });
    }
    auto end = std::chrono::steady_clock::now() + deadline;
    for (auto& marker : markers) {
        if (marker.wait_until(end) != std::future_status::ready) return false;
    }
    return contexts.size() == threads;
}

// Start `request` on every thread of the pool (from that thread, so it runs there), and wait
// until they all were started, within `deadline`
template<typename Request>
bool start_on_every_worker(asio::workers& workers, size_t threads, Request request,
                           std::chrono::milliseconds deadline = 2s) {
    std::set<boost::asio::io_context*> contexts;
    for (size_t i = 0; contexts.size() < threads && i < threads * 16; ++i) {
        contexts.insert(&workers.get_next_io_context());
    }
    auto started = std::make_shared<std::atomic<size_t>>(0);
    for (auto* context : contexts) {
        boost::asio::post(*context, [request, started] {
            request();
            ++*started;
        });
    }
    return contexts.size() == threads && eventually([&] { return started->load() == threads; }, deadline);
}

// What the callbacks of the requests of a test saw: each one by its index
struct callback_log {
    explicit callback_log(size_t requests) : calls(requests) {}

    // Set right after stop() or the destructor returned
    std::atomic<bool> client_done{false};
    std::vector<std::atomic<int>> calls;
    // Callbacks run once the client was done, and those that saw something else than an
    // operation_aborted error
    std::atomic<int> after_done{0};
    std::atomic<int> not_aborted{0};

    // The callback of request `index`
    auto callback(size_t index) {
        return [this, index](http::client_response& response) {
            ++calls[index];
            if (client_done) ++after_done;
            if (response.ok() || response.error_code() != boost::asio::error::operation_aborted) ++not_aborted;
        };
    }

    int total_calls() const {
        int total = 0;
        for (const auto& count : calls) total += count.load();
        return total;
    }

    bool each_at_most_once() const {
        for (const auto& count : calls) {
            if (count.load() > 1) return false;
        }
        return true;
    }
};

} // namespace

// --- From the application's thread (US1) ---

TEST_CASE("Destroying a client with requests to a silent server", "[client][async][lifetime]") {
    constexpr size_t threads = 2;
    constexpr size_t requests = 4;
    running_workers workers(threads);
    silent_server silent;
    callback_log log(requests);
    std::atomic<int> coroutine_after_done{0};
    std::atomic<int> coroutine_resumed{0};

    auto client = std::make_unique<http::async_client>(workers);
    for (size_t i = 0; i < requests; ++i) client->get(silent.url("/" + std::to_string(i)), log.callback(i));
    // The coroutine holds the client itself, not the pointer the test resets
    client->run([&, raw = client.get()]() -> awaitable<void> {
        co_await raw->get(silent.url("/coroutine"));
        ++coroutine_resumed;
        if (log.client_done) ++coroutine_after_done;
    });
    // Every request waits for its response
    REQUIRE(eventually([&] { return silent.accepted == static_cast<int>(requests) + 1; }, 2s));

    auto start = std::chrono::steady_clock::now();
    client.reset();
    log.client_done = true;
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < 2s);

    // The client closed every connection, and whatever was queued on the workers has run
    CHECK(eventually([&] { return silent.all_closed(); }, 2s));
    REQUIRE(drain(workers, threads));
    CHECK(log.after_done == 0);
    CHECK(coroutine_after_done == 0);
    CHECK(coroutine_resumed <= 1);
    CHECK(log.each_at_most_once());
    CHECK(log.not_aborted == 0);
}

TEST_CASE("Stopping a client with requests to a silent server", "[client][async][lifetime]") {
    constexpr size_t threads = 2;
    constexpr size_t requests = 4;
    running_workers workers(threads);
    silent_server silent;
    callback_log log(requests);

    http::async_client client(workers);
    for (size_t i = 0; i < requests; ++i) client.get(silent.url("/" + std::to_string(i)), log.callback(i));
    REQUIRE(eventually([&] { return silent.accepted == static_cast<int>(requests); }, 2s));
    REQUIRE(client.pending_requests() == requests);

    auto start = std::chrono::steady_clock::now();
    client.stop();
    log.client_done = true;
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < 2s);
    CHECK(client.pending_requests() == 0);

    CHECK(eventually([&] { return silent.all_closed(); }, 2s));
    REQUIRE(drain(workers, threads));
    CHECK(log.after_done == 0);
    CHECK(log.each_at_most_once());
    CHECK(log.not_aborted == 0);

    // Then the workers stop, and the client is destroyed on them stopped (at the end of the case)
    workers.stop();
    CHECK(log.after_done == 0);
}

TEST_CASE("Destroying a client waits for its callback running on a worker", "[client][async][lifetime]") {
    running_workers workers(2);
    http::test::TestServerFixture fixture;
    std::atomic<bool> was_ok{false};
    std::atomic<bool> entered{false};
    std::atomic<bool> returned{false};

    auto client = std::make_unique<http::async_client>(workers);
    client->get(fixture.base_url + "/get", [&](http::client_response& response) {
        was_ok = response.ok();
        entered = true;
        std::this_thread::sleep_for(300ms);
        returned = true;
    });
    REQUIRE(eventually([&] { return entered.load(); }, 5s));

    client.reset();
    CHECK(returned);
    CHECK(was_ok);
}

TEST_CASE("A request cancelled by stop() or the destructor reports operation_aborted", "[client][async][lifetime]") {
    constexpr size_t threads = 2;
    running_workers workers(threads);
    silent_server silent;
    std::atomic<int> aborted{0};
    std::atomic<int> failed_otherwise{0};
    std::atomic<int> stream_failed{0};
    std::mutex mutex;
    std::string stream_error;
    callback_log log(1);

    // The coroutines of run() see their requests fail; a callback, if invoked, too
    auto start_requests = [&](http::async_client& client) {
        client.run([&, raw = &client]() -> awaitable<void> {
            auto response = co_await raw->get(silent.url("/plain"));
            if (!response.ok() && response.error_code() == boost::asio::error::operation_aborted) {
                ++aborted;
            } else {
                ++failed_otherwise;
            }
        });
        client.run([&, raw = &client]() -> awaitable<void> {
            auto request = std::make_shared<http::http_request>();
            request->set_url(silent.url("/streamed"));
            request->set_method(http::method::GET);
            auto result = co_await raw->send_streaming(request, [](const http::stream_info&) { return true; });
            if (!result.ok() && !result.error.empty()) ++stream_failed;
            std::lock_guard lock(mutex);
            stream_error = result.error;
        });
        client.get(silent.url("/callback"), log.callback(0));
    };

    SECTION("stop()") {
        http::async_client client(workers);
        start_requests(client);
        REQUIRE(eventually([&] { return silent.accepted == 3; }, 2s));
        client.stop();
        log.client_done = true;
    }

    SECTION("the destructor") {
        {
            http::async_client client(workers);
            start_requests(client);
            REQUIRE(eventually([&] { return silent.accepted == 3; }, 2s));
        }
        log.client_done = true;
    }

    // Both coroutines ended before the return: the requests were waited for
    CHECK(aborted == 1);
    CHECK(failed_otherwise == 0);
    CHECK(stream_failed == 1);
    {
        std::lock_guard lock(mutex);
        INFO("stream error: " << stream_error);
        CHECK_FALSE(stream_error.empty());
    }
    REQUIRE(drain(workers, threads));
    CHECK(log.after_done == 0);
    CHECK(log.each_at_most_once());
    CHECK(log.not_aborted == 0);
}

// --- From inside one of its own completions (US2) ---

TEST_CASE("A callback destroys its client", "[client][async][lifetime]") {
    constexpr size_t threads = 2;
    running_workers workers(threads);
    http::test::TestServerFixture fixture;
    std::atomic<bool> done{false};
    std::atomic<bool> was_ok{false};

    auto client = std::make_unique<http::async_client>(workers);
    client->get(fixture.base_url + "/get", [&](http::client_response& response) {
        was_ok = response.ok();
        client.reset();
        done = true;
    });

    // The destructor returned on the worker, and the callback after it
    REQUIRE(eventually([&] { return done.load(); }, 5s));
    CHECK(was_ok);
    // The rest of the request ends on its worker, touching nothing of the client
    REQUIRE(drain(workers, threads));
}

TEST_CASE("A callback destroys its client with other requests in flight", "[client][async][lifetime]") {
    for (size_t threads : {size_t{1}, size_t{3}}) {
        DYNAMIC_SECTION(threads << " worker thread(s)") {
            running_workers workers(threads);
            http::test::TestServerFixture fixture;
            silent_server silent;
            std::atomic<bool> done{false};
            std::atomic<bool> was_ok{false};
            callback_log log(threads + 1);

            auto test_start = std::chrono::steady_clock::now();
            auto client = std::make_unique<http::async_client>(workers);
            // A request waiting for a silent server from the application's thread, and one on
            // every worker (with a single thread, on the same one as the callback below)
            client->get(silent.url("/main"), log.callback(threads));
            size_t index = 0;
            std::mutex index_mutex;
            REQUIRE(start_on_every_worker(workers, threads, [&] {
                size_t mine;
                {
                    std::lock_guard lock(index_mutex);
                    mine = index++;
                }
                client->get(silent.url("/worker"), log.callback(mine));
            }));
            REQUIRE(eventually([&] { return silent.accepted == static_cast<int>(threads) + 1; }, 2s));

            client->get(fixture.base_url + "/get", [&](http::client_response& response) {
                was_ok = response.ok();
                client.reset();
                log.client_done = true;
                done = true;
            });

            // Nothing deadlocks: the client is destroyed, the requests still in flight end
            // (their connections are closed), and none of their callbacks runs afterwards
            REQUIRE(eventually([&] { return done.load(); }, 5s));
            CHECK(was_ok);
            CHECK(eventually([&] { return silent.all_closed(); }, 5s));
            REQUIRE(drain(workers, threads));
            CHECK(std::chrono::steady_clock::now() - test_start < 5s);
            CHECK(log.after_done == 0);
            CHECK(log.each_at_most_once());
            CHECK(log.not_aborted == 0);
        }
    }
}

TEST_CASE("A callback stops its client, then the application destroys it", "[client][async][lifetime]") {
    constexpr size_t threads = 2;
    constexpr size_t requests = 3;
    running_workers workers(threads);
    http::test::TestServerFixture fixture;
    silent_server silent;
    std::atomic<bool> stopped{false};
    std::atomic<bool> was_ok{false};
    callback_log log(requests);

    auto client = std::make_unique<http::async_client>(workers);
    for (size_t i = 0; i < requests; ++i) client->get(silent.url("/" + std::to_string(i)), log.callback(i));
    REQUIRE(eventually([&] { return silent.accepted == static_cast<int>(requests); }, 2s));

    client->get(fixture.base_url + "/get", [&](http::client_response& response) {
        was_ok = response.ok();
        client->stop();
        stopped = true;
    });

    // stop() returned on the worker, and the other requests end as cancelled: their
    // connections are closed, their callbacks are not called
    REQUIRE(eventually([&] { return stopped.load(); }, 5s));
    CHECK(was_ok);
    CHECK(eventually([&] { return silent.all_closed(); }, 2s));
    REQUIRE(drain(workers, threads));
    CHECK(client->pending_requests() == 0);
    CHECK(log.total_calls() == 0);

    auto start = std::chrono::steady_clock::now();
    client.reset();
    log.client_done = true;
    CHECK(std::chrono::steady_clock::now() - start < 2s);
    REQUIRE(drain(workers, threads));
    CHECK(log.after_done == 0);
}

// --- Less common states (US3) ---

TEST_CASE("Destroying a client with a request waiting in its TLS handshake", "[client][async][lifetime]") {
    constexpr size_t threads = 2;
    running_workers workers(threads);
    silent_server silent;
    callback_log log(1);

    // The peer accepts and never answers the ClientHello
    auto client = std::make_unique<http::async_client>(workers);
    client->get(silent.url("/", "https"), log.callback(0));
    REQUIRE(eventually([&] { return silent.accepted == 1; }, 2s));

    auto start = std::chrono::steady_clock::now();
    client.reset();
    log.client_done = true;
    CHECK(std::chrono::steady_clock::now() - start < 2s);

    CHECK(eventually([&] { return silent.all_closed(); }, 2s));
    REQUIRE(drain(workers, threads));
    CHECK(log.after_done == 0);
    CHECK(log.each_at_most_once());
    CHECK(log.not_aborted == 0);
}

TEST_CASE("A coroutine of run() suspended in a request does not resume after the destructor",
          "[client][async][lifetime]") {
    constexpr size_t threads = 2;
    constexpr int coroutines = 4;
    running_workers workers(threads);
    silent_server silent;
    std::atomic<bool> client_done{false};
    std::atomic<int> resumed{0};
    std::atomic<int> resumed_after_done{0};
    std::atomic<int> resumed_ok{0};

    auto client = std::make_unique<http::async_client>(workers);
    for (int i = 0; i < coroutines; ++i) {
        // One that retries forever too: once cancelled, its next request fails at once
        bool retries = i == 0;
        client->run([&, raw = client.get(), retries]() -> awaitable<void> {
            do {
                auto response = co_await raw->get(silent.url("/coroutine"));
                ++resumed;
                if (client_done) ++resumed_after_done;
                if (response.ok()) ++resumed_ok;
            } while (retries);
        });
    }
    REQUIRE(eventually([&] { return silent.accepted == coroutines; }, 2s));

    auto start = std::chrono::steady_clock::now();
    client.reset();
    client_done = true;
    CHECK(std::chrono::steady_clock::now() - start < 2s);

    CHECK(eventually([&] { return silent.all_closed(); }, 2s));
    REQUIRE(drain(workers, threads));
    // Those that resumed before the return saw their request fail
    CHECK(resumed_after_done == 0);
    CHECK(resumed_ok == 0);
}

TEST_CASE("A callback starts a request while the client stops or is destroyed", "[client][async][lifetime]") {
    constexpr size_t threads = 2;
    running_workers workers(threads);
    http::test::TestServerFixture fixture;
    silent_server silent;
    std::atomic<bool> entered{false};
    std::atomic<bool> started{false};
    std::atomic<bool> client_done{false};
    std::atomic<int> late_calls{0};
    std::atomic<int> late_coroutines{0};

    // The callback of a request runs while stop() or the destructor runs (which wait for it),
    // and starts a request and a coroutine on the client then
    auto start_requests = [&](http::async_client& client) {
        client.get(fixture.base_url + "/get", [&, raw = &client](http::client_response&) {
            entered = true;
            // Until stop() or the destructor started (which clears is_running() first), and a
            // little more, so that it already refuses new requests
            eventually([&] { return !raw->is_running(); }, 5s);
            std::this_thread::sleep_for(50ms);
            raw->get(silent.url("/late"), [&](http::client_response&) { ++late_calls; });
            raw->run([&]() -> awaitable<void> {
                ++late_coroutines;
                co_return;
            });
            started = true;
        });
        REQUIRE(eventually([&] { return entered.load(); }, 5s));
    };

    std::chrono::steady_clock::duration elapsed{};
    SECTION("stop()") {
        http::async_client client(workers);
        start_requests(client);
        auto start = std::chrono::steady_clock::now();
        client.stop();
        elapsed = std::chrono::steady_clock::now() - start;
        client_done = true;
        CHECK(client.pending_requests() == 0);
    }

    SECTION("the destructor") {
        auto client = std::make_unique<http::async_client>(workers);
        start_requests(*client);
        auto start = std::chrono::steady_clock::now();
        client.reset();
        elapsed = std::chrono::steady_clock::now() - start;
        client_done = true;
    }

    // The callback returned before stop() or the destructor; what it started did not run
    CHECK(started);
    CHECK(elapsed < 2s);
    REQUIRE(drain(workers, threads));
    CHECK(silent.accepted == 0);
    CHECK(late_calls == 0);
    CHECK(late_coroutines == 0);
}

// --- Edge cases ---

TEST_CASE("Stopping a client twice, then destroying it", "[client][async][lifetime]") {
    constexpr size_t threads = 2;
    constexpr size_t requests = 3;
    running_workers workers(threads);
    silent_server silent;
    callback_log log(requests);

    auto client = std::make_unique<http::async_client>(workers);
    for (size_t i = 0; i < requests; ++i) client->get(silent.url("/" + std::to_string(i)), log.callback(i));
    REQUIRE(eventually([&] { return silent.accepted == static_cast<int>(requests); }, 2s));

    CHECK(client->stop());
    log.client_done = true;
    CHECK(client->pending_requests() == 0);

    // Nothing is in flight anymore: they return at once
    auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(client->stop());
    CHECK(client->pending_requests() == 0);
    client.reset();
    CHECK(std::chrono::steady_clock::now() - start < 500ms);

    CHECK(eventually([&] { return silent.all_closed(); }, 2s));
    REQUIRE(drain(workers, threads));
    CHECK(log.total_calls() == 0);
}

TEST_CASE("stop() and the destructor close the idle pooled connections", "[client][async][lifetime]") {
    constexpr size_t threads = 2;
    running_workers workers(threads);
    silent_server server(true);
    std::atomic<int> ok{0};

    // A request answered, once its callback ran (wait() no longer waits after stop())
    auto client = std::make_unique<http::async_client>(workers);
    auto request = [&] {
        int expected = ok + 1;
        client->get(server.url("/"), [&](http::client_response& response) {
            if (response.ok()) ++ok;
        });
        REQUIRE(eventually([&] { return ok == expected && client->pending_requests() == 0; }, 5s));
    };

    // Its connection stays open, idle in the pool, until...
    request();
    REQUIRE(server.accepted == 1);
    CHECK(server.closed == 0);

    SECTION("stop()") {
        client->stop();
        CHECK(eventually([&] { return server.closed == 1; }, 2s));
    }

    SECTION("the destructor") {
        client.reset();
        CHECK(eventually([&] { return server.closed == 1; }, 2s));
    }

    SECTION("stop(), with a later request, then the destructor") {
        client->stop();
        REQUIRE(eventually([&] { return server.closed == 1; }, 2s));
        // A request started after stop() returned runs as before
        request();
        REQUIRE(server.accepted == 2);
        client.reset();
        CHECK(eventually([&] { return server.closed == 2; }, 2s));
    }
}

TEST_CASE("stop() releases wait() and wait_for() blocked on other threads", "[client][async][lifetime]") {
    constexpr size_t threads = 2;
    constexpr size_t requests = 2;
    running_workers workers(threads);
    silent_server silent;
    callback_log log(requests);

    http::async_client client(workers);
    for (size_t i = 0; i < requests; ++i) client.get(silent.url("/" + std::to_string(i)), log.callback(i));
    REQUIRE(eventually([&] { return silent.accepted == static_cast<int>(requests); }, 2s));

    std::atomic<int> waiting{0};
    std::vector<std::future<void>> waiters;
    for (int i = 0; i < 2; ++i) {
        waiters.push_back(std::async(std::launch::async, [&] {
            ++waiting;
            client.wait();
        }));
        waiters.push_back(std::async(std::launch::async, [&] {
            ++waiting;
            client.wait_for(30s);
        }));
    }
    REQUIRE(eventually([&] { return waiting == 4; }, 2s));

    client.stop();
    log.client_done = true;
    // Released whether they were already waiting or not
    auto end = std::chrono::steady_clock::now() + 2s;
    for (auto& waiter : waiters) CHECK(waiter.wait_until(end) == std::future_status::ready);
    REQUIRE(drain(workers, threads));
    CHECK(log.total_calls() == 0);
}

TEST_CASE("A WebSocket handed out by a client outlives it", "[client][async][lifetime]") {
    constexpr size_t threads = 2;
    running_workers workers(threads);
    http::server server;
    server.get("/ws/echo", [](http::request&, http::response& res) {
        res.upgrade_websocket({}, [](std::shared_ptr<http::websocket_connection> ws) {
            ws->on_message([ws](std::string message, bool) { ws->send_text(std::move(message)); });
        });
    });
    REQUIRE_FALSE(server.listen("127.0.0.1", 0));
    // Stopped when the case ends, even by a failed REQUIRE
    struct serving {
        http::server& server;
        std::thread thread{[this] { server.wait(); }};
        ~serving() {
            server.stop();
            thread.join();
        }
    } running{server};
    auto url = "ws://127.0.0.1:" + std::to_string(http::test::listening_port(server)) + "/ws/echo";

    // The WebSocket, and the worker it runs on (that of the callback)
    std::mutex mutex;
    std::shared_ptr<http::websocket_client> websocket;
    boost::asio::io_context* context = nullptr;
    std::atomic<bool> handed{false};

    auto client = std::make_unique<http::async_client>(workers);
    client->websocket(url, [&](std::shared_ptr<http::websocket_client> ws) {
        std::lock_guard lock(mutex);
        websocket = std::move(ws);
        context = &workers.get_thread_io_context();
        handed = true;
    });
    REQUIRE(eventually([&] { return handed.load(); }, 5s));

    client.reset();
    std::lock_guard lock(mutex);
    REQUIRE(websocket);
    CHECK(websocket->is_open());
    // It still sends and receives on its worker (the coroutine API: the synchronous one runs
    // the io_context, which the workers run here)
    auto echo = boost::asio::co_spawn(*context, [ws = websocket]() -> awaitable<std::string> {
        if (!co_await ws->send_text_async("still open")) co_return "";
        auto [message, binary] = co_await ws->receive_async();
        co_await ws->close_async();
        co_return message;
    }, boost::asio::use_future);
    REQUIRE(echo.wait_for(5s) == std::future_status::ready);
    CHECK(echo.get() == "still open");
    websocket.reset();
}

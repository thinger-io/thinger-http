// An async_client stopped or destroyed with requests in flight, while its workers run: from the
// application's thread (US1), from inside one of its own completions (US2), and in the less
// common states (US3 and the edge cases). No callback of the client runs once stop() or the
// destructor returned. No coroutine of run() does either when they were called from the
// application's thread; from a worker, those may resume afterwards, and see their requests fail
// with operation_aborted or a response that completed before the cancellation reached them, so
// they hold what they use by shared_ptr, the client included
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
#include <iterator>
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

    // Read the requests, answering them or not, until the client closes the connection. The end
    // of a request has to arrive within one read, which holds for these small ones, written at
    // once. Each read has its own buffer: the read of another connection could overwrite a shared
    // one before this handler runs, and the request would never be answered
    void read(tcp::socket& peer) {
        auto own = std::make_shared<std::array<char, 4096>>();
        peer.async_read_some(boost::asio::buffer(*own), [this, &peer, own](boost::system::error_code ec, size_t size) {
            if (ec) {
                ++closed;
                return;
            }
            if (answer && std::string_view(own->data(), size).find("\r\n\r\n") != std::string_view::npos) {
                static constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
                boost::asio::async_write(peer, boost::asio::buffer(response), [](boost::system::error_code, size_t) {});
            }
            read(peer);
        });
    }
};

// A server answering every request with a chunked body that never ends, written as fast as the
// client reads it: several chunks reach the client in a single read. It counts the connections
// accepted, and those the client closed
struct endless_stream_server {
    boost::asio::io_context context;
    tcp::acceptor acceptor{context, {boost::asio::ip::make_address("127.0.0.1"), 0}};
    std::atomic<int> accepted{0};
    std::atomic<int> closed{0};
    std::thread thread;

    endless_stream_server() {
        accept();
        thread = std::thread([this] { context.run(); });
    }

    ~endless_stream_server() {
        boost::asio::post(context, [this] { context.stop(); });
        thread.join();
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/"; }

private:
    // Touched only on the server's thread
    std::array<char, 4096> discard{};

    static const std::string& chunks() {
        static const std::string burst = [] {
            std::string result;
            for (int i = 0; i < 64; ++i) result += "4\r\ndata\r\n";
            return result;
        }();
        return burst;
    }

    void accept() {
        acceptor.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (ec) return;
            ++accepted;
            auto peer = std::make_shared<tcp::socket>(std::move(socket));
            // The request (a small one, read whole), then the head with the first chunks
            peer->async_read_some(boost::asio::buffer(discard), [this, peer](boost::system::error_code ec, size_t) {
                if (ec) {
                    ++closed;
                    return;
                }
                auto head = std::make_shared<std::string>("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + chunks());
                boost::asio::async_write(*peer, boost::asio::buffer(*head), [this, peer, head](boost::system::error_code ec, size_t) {
                    write(peer, ec);
                });
            });
            accept();
        });
    }

    void write(const std::shared_ptr<tcp::socket>& peer, boost::system::error_code ec) {
        if (ec) {
            ++closed;
            return;
        }
        boost::asio::async_write(*peer, boost::asio::buffer(chunks()), [this, peer](boost::system::error_code ec, size_t) {
            write(peer, ec);
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

TEST_CASE("A callback destroys its client, and a coroutine of run() on its worker resumes afterwards",
          "[client][async][lifetime]") {
    constexpr size_t threads = 1;
    running_workers workers(threads);
    http::test::TestServerFixture fixture;
    silent_server silent;

    // What the coroutine uses: it may outlive the client, so it holds it
    struct seen_by_coroutine {
        std::atomic<bool> client_done{false};
        std::atomic<int> resumed{0};
        std::atomic<int> resumed_after_done{0};
        std::atomic<int> not_aborted{0};
    };
    auto seen = std::make_shared<seen_by_coroutine>();

    auto client = std::make_unique<http::async_client>(workers);
    client->run([seen, raw = client.get(), url = silent.url("/coroutine")]() -> awaitable<void> {
        auto response = co_await raw->get(url);
        ++seen->resumed;
        if (seen->client_done) ++seen->resumed_after_done;
        if (response.ok() || response.error_code() != boost::asio::error::operation_aborted) ++seen->not_aborted;
    });
    REQUIRE(eventually([&] { return silent.accepted == 1; }, 2s));

    // The callback holds the only worker while this thread queues a request and a coroutine on
    // it, which cannot start before the destructor, and then destroys the client
    callback_log log(1);
    std::atomic<bool> entered{false};
    std::atomic<bool> queued{false};
    std::atomic<bool> done{false};
    std::atomic<bool> resumed_in_destructor{false};
    std::atomic<int> not_started_coroutines{0};
    client->get(fixture.base_url + "/get", [&](http::client_response&) {
        entered = true;
        eventually([&] { return queued.load(); }, 5s);
        client.reset();
        resumed_in_destructor = seen->resumed > 0;
        seen->client_done = true;
        log.client_done = true;
        done = true;
    });
    REQUIRE(eventually([&] { return entered.load(); }, 5s));
    client->get(silent.url("/not-started"), log.callback(0));
    client->run([&]() -> awaitable<void> {
        ++not_started_coroutines;
        co_return;
    });
    queued = true;

    REQUIRE(eventually([&] { return done.load(); }, 5s));
    // It resumes on its worker once the callback returns, with the client gone, and only sees its
    // request fail; the connection closes
    REQUIRE(eventually([&] { return seen->resumed == 1; }, 5s));
    CHECK_FALSE(resumed_in_destructor);
    CHECK(seen->resumed_after_done == 1);
    CHECK(seen->not_aborted == 0);
    CHECK(eventually([&] { return silent.all_closed(); }, 2s));
    REQUIRE(drain(workers, threads));
    // What had not started yet does not run: no connection, no callback, no coroutine
    CHECK(silent.accepted == 1);
    CHECK(log.total_calls() == 0);
    CHECK(not_started_coroutines == 0);
}

TEST_CASE("A callback destroys its client while a callback of another worker runs", "[client][async][lifetime]") {
    constexpr size_t threads = 2;
    running_workers workers(threads);
    http::test::TestServerFixture fixture;
    std::atomic<bool> sleeping{false};
    std::atomic<bool> destroying{false};
    std::atomic<bool> slept{false};
    std::atomic<bool> slept_at_return{false};
    std::atomic<bool> done{false};
    // Each one written by its callback, and read once `done` is set
    std::thread::id sleeper;
    std::thread::id destroyer;

    auto client = std::make_unique<http::async_client>(workers);
    // One request from each worker, so that it runs there
    std::atomic<int> started{0};
    REQUIRE(start_on_every_worker(workers, threads, [&] {
        if (started++ == 0) {
            client->get(fixture.base_url + "/get", [&](http::client_response&) {
                sleeper = std::this_thread::get_id();
                sleeping = true;
                // Asleep for sure when the destructor starts
                eventually([&] { return destroying.load(); }, 5s);
                std::this_thread::sleep_for(300ms);
                slept = true;
            });
        } else {
            client->get(fixture.base_url + "/get", [&](http::client_response&) {
                destroyer = std::this_thread::get_id();
                eventually([&] { return sleeping.load(); }, 5s);
                destroying = true;
                client.reset();
                slept_at_return = slept.load();
                done = true;
            });
        }
    }));

    REQUIRE(eventually([&] { return done.load(); }, 5s));
    CHECK(sleeping);
    CHECK(sleeper != destroyer);
    CHECK(slept_at_return);
    REQUIRE(drain(workers, threads));
}

TEST_CASE("A callback stops its client, and the destructor waits for the coroutines cancelled",
          "[client][async][lifetime]") {
    constexpr size_t threads = 1;
    running_workers workers(threads);
    http::test::TestServerFixture fixture;
    silent_server silent;
    std::atomic<bool> stopped{false};
    std::atomic<bool> destroying{false};
    std::atomic<bool> client_done{false};
    std::atomic<int> resumed{0};
    std::atomic<int> resumed_after_done{0};
    std::atomic<int> not_aborted{0};

    auto client = std::make_unique<http::async_client>(workers);
    client->run([&, raw = client.get()]() -> awaitable<void> {
        auto response = co_await raw->get(silent.url("/coroutine"));
        ++resumed;
        if (client_done) ++resumed_after_done;
        if (response.ok() || response.error_code() != boost::asio::error::operation_aborted) ++not_aborted;
    });
    REQUIRE(eventually([&] { return silent.accepted == 1; }, 2s));

    // stop() does not wait on the worker; the callback then holds it, so the coroutine cancelled
    // cannot end before the destructor starts
    client->get(fixture.base_url + "/get", [&](http::client_response&) {
        client->stop();
        stopped = true;
        eventually([&] { return destroying.load(); }, 5s);
        std::this_thread::sleep_for(100ms);
    });
    REQUIRE(eventually([&] { return stopped.load(); }, 5s));
    CHECK(resumed == 0);

    destroying = true;
    auto start = std::chrono::steady_clock::now();
    client.reset();
    client_done = true;
    CHECK(std::chrono::steady_clock::now() - start < 2s);
    // It ended before the return, having seen its request fail
    CHECK(resumed == 1);
    CHECK(not_aborted == 0);

    CHECK(eventually([&] { return silent.all_closed(); }, 2s));
    REQUIRE(drain(workers, threads));
    CHECK(resumed_after_done == 0);
}

namespace {

// What the coroutines of the next cases see, which hold it and the client by shared_ptr: they may
// outlive the call that stops it
struct seen_by_holders {
    std::atomic<bool> client_done{false};
    std::atomic<int> ended{0};
    std::atomic<int> responses_after_done{0};
    std::atomic<int> aborted_after_done{0};
    std::atomic<int> other_errors{0};
    std::atomic<int> cancelled_callbacks{0};

    void failed(boost::system::error_code error) {
        if (error != boost::asio::error::operation_aborted) ++other_errors;
        else if (client_done) ++aborted_after_done;
    }
};

// A coroutine holding its client: it makes requests until one is aborted, reported or thrown
awaitable<void> request_until_aborted(std::shared_ptr<seen_by_holders> seen, std::shared_ptr<http::async_client> client,
                                      std::string url) {
    try {
        for (;;) {
            auto response = co_await client->get(url);
            if (!response.ok()) {
                seen->failed(response.error_code());
                break;
            }
            if (seen->client_done) ++seen->responses_after_done;
        }
    } catch (const boost::system::system_error& error) {
        seen->failed(error.code());
    }
    ++seen->ended;
}

} // namespace

// The worker B is held while a callback on the worker A stops the client, so the cancellation
// reaches the coroutine of B only after stop() returned there
TEST_CASE("A callback stops a client its coroutines hold, and one of another worker resumes afterwards",
          "[client][async][lifetime]") {
    constexpr size_t threads = 3;
    running_workers workers(threads);
    http::test::TestServerFixture fixture;
    silent_server silent;
    auto seen = std::make_shared<seen_by_holders>();

    std::set<boost::asio::io_context*> contexts;
    for (size_t i = 0; contexts.size() < threads && i < threads * 16; ++i) {
        contexts.insert(&workers.get_next_io_context());
    }
    REQUIRE(contexts.size() == threads);
    auto& worker_a = **contexts.begin();
    auto& worker_b = **std::next(contexts.begin());

    auto client = std::make_shared<http::async_client>(workers);
    std::weak_ptr<http::async_client> handle = client;
    boost::asio::post(worker_b, [seen, client, url = silent.url("/coroutine")] {
        client->run(request_until_aborted(seen, client, url));
    });
    REQUIRE(eventually([&] { return silent.accepted == 1; }, 5s));

    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
    boost::asio::post(worker_b, [&] {
        held = true;
        eventually([&] { return release.load(); }, 5s);
    });
    REQUIRE(eventually([&] { return held.load(); }, 5s));

    boost::asio::post(worker_a, [&, seen, client, url = fixture.base_url + "/get"] {
        client->get(url, [&, seen, client](http::client_response&) {
            client->stop();
            seen->client_done = true;
            release = true;
        });
    });
    client.reset();

    // It resumes once stop() returned, and the client goes with the coroutine, on its worker
    REQUIRE(eventually([&] { return seen->ended == 1; }, 5s));
    CHECK(seen->aborted_after_done == 1);
    CHECK(seen->responses_after_done == 0);
    CHECK(seen->other_errors == 0);
    CHECK(eventually([&] { return handle.expired(); }, 2s));
    CHECK(eventually([&] { return silent.all_closed(); }, 2s));
    REQUIRE(drain(workers, threads));
}

// Racing with responses: the coroutines of the other workers may see correct ones after stop()
// returned, or operation_aborted, and nothing else; no callback of a request cancelled is called.
// Under the sanitizers, they use the client until the end
TEST_CASE("A callback stops a client its coroutines hold, while they get responses on every worker",
          "[client][async][lifetime]") {
    constexpr size_t threads = 3;
    constexpr int per_worker = 4;
    constexpr int iterations = 30;
    running_workers workers(threads);
    silent_server answering(true);
    silent_server silent;
    int responses_after_done = 0;
    int aborted_after_done = 0;

    for (int iteration = 0; iteration < iterations; ++iteration) {
        auto seen = std::make_shared<seen_by_holders>();
        auto client = std::make_shared<http::async_client>(workers);
        std::weak_ptr<http::async_client> handle = client;
        REQUIRE(start_on_every_worker(workers, threads, [seen, client, url = answering.url(), silent_url = silent.url()] {
            for (int i = 0; i < per_worker; ++i) {
                client->run(request_until_aborted(seen, client, url));
            }
            client->get(silent_url, [seen, client](http::client_response&) { ++seen->cancelled_callbacks; });
        }));
        client->get(answering.url(), [seen, client](http::client_response&) {
            client->stop();
            seen->client_done = true;
        });
        client.reset();

        REQUIRE(eventually([&] { return seen->ended == static_cast<int>(threads) * per_worker; }, 5s));
        REQUIRE(eventually([&] { return handle.expired(); }, 5s));
        CHECK(seen->other_errors == 0);
        CHECK(seen->cancelled_callbacks == 0);
        CHECK(seen->client_done);
        responses_after_done += seen->responses_after_done;
        aborted_after_done += seen->aborted_after_done;
    }
    INFO("after stop(): " << responses_after_done << " responses, " << aborted_after_done << " aborted");
    CHECK(eventually([&] { return answering.all_closed() && silent.all_closed(); }, 5s));
    REQUIRE(drain(workers, threads));
}

TEST_CASE("A coroutine holds the last reference to its client", "[client][async][lifetime]") {
    constexpr size_t threads = 3;
    running_workers workers(threads);
    silent_server answering(true);
    auto ended = std::make_shared<std::atomic<int>>(0);
    auto responses = std::make_shared<std::atomic<int>>(0);

    auto client = std::make_shared<http::async_client>(workers);
    std::weak_ptr<http::async_client> handle = client;
    REQUIRE(start_on_every_worker(workers, threads, [ended, responses, client, url = answering.url()] {
        client->run([ended, responses, client, url]() -> awaitable<void> {
            for (int i = 0; i < 3; ++i) {
                auto response = co_await client->get(url);
                if (response.ok()) ++*responses;
            }
            ++*ended;
        });
    }));
    client.reset();

    // The last one ending destroys it on its worker, within its own frame
    REQUIRE(eventually([&] { return ended->load() == static_cast<int>(threads); }, 5s));
    CHECK(responses->load() == static_cast<int>(threads) * 3);
    CHECK(eventually([&] { return handle.expired(); }, 2s));
    CHECK(eventually([&] { return answering.all_closed(); }, 2s));
    REQUIRE(drain(workers, threads));
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

TEST_CASE("What is queued on a worker when the application destroys the client does not start",
          "[client][async][lifetime]") {
    constexpr size_t threads = 1;
    running_workers workers(threads);
    http::test::TestServerFixture fixture;
    silent_server silent;
    std::atomic<bool> entered{false};
    std::atomic<bool> queued{false};
    std::atomic<int> started_coroutines{0};
    callback_log log(1);

    // The callback holds the only worker until the destructor marked what is queued as cancelled,
    // so that it finds the mark when it starts, before the cancellation reaches it
    auto client = std::make_unique<http::async_client>(workers);
    client->get(fixture.base_url + "/get", [&, raw = client.get()](http::client_response&) {
        entered = true;
        eventually([&] { return queued.load() && !raw->is_running(); }, 5s);
        std::this_thread::sleep_for(100ms);
    });
    REQUIRE(eventually([&] { return entered.load(); }, 5s));
    client->get(silent.url("/queued"), log.callback(0));
    client->run([&]() -> awaitable<void> {
        ++started_coroutines;
        co_return;
    });
    queued = true;

    client.reset();
    log.client_done = true;
    REQUIRE(drain(workers, threads));
    CHECK(started_coroutines == 0);
    CHECK(silent.accepted == 0);
    CHECK(log.total_calls() == 0);
}

TEST_CASE("A chunk of a streamed body does not reach its callback once the client is gone",
          "[client][async][lifetime]") {
    constexpr size_t threads = 1;
    running_workers workers(threads);
    http::test::TestServerFixture fixture;
    endless_stream_server server;
    std::atomic<bool> client_done{false};
    std::atomic<bool> destroy_in_chunk{false};
    std::atomic<int> chunks{0};
    std::atomic<int> chunks_after_done{0};
    std::atomic<int> results{0};

    auto client = std::make_unique<http::async_client>(workers);
    auto start_stream = [&] {
        client->get_streaming(server.url(), [&](const http::stream_info&) {
            ++chunks;
            if (client_done) ++chunks_after_done;
            if (destroy_in_chunk) {
                destroy_in_chunk = false;
                client.reset();
                client_done = true;
            }
            return true;
        }, [&](http::stream_result&) { ++results; });
    };

    SECTION("destroyed from the application's thread") {
        start_stream();
        REQUIRE(eventually([&] { return chunks > 0; }, 5s));
        client.reset();
        client_done = true;
    }

    SECTION("destroyed by a chunk callback, with more chunks read") {
        destroy_in_chunk = true;
        start_stream();
    }

    SECTION("destroyed by a callback on the same worker, with a read completed") {
        start_stream();
        REQUIRE(eventually([&] { return chunks > 0; }, 5s));
        client->get(fixture.base_url + "/get", [&](http::client_response&) {
            client.reset();
            client_done = true;
        });
    }

    REQUIRE(eventually([&] { return client_done.load(); }, 5s));
    CHECK(eventually([&] { return server.closed == server.accepted; }, 2s));
    REQUIRE(drain(workers, threads));
    CHECK(chunks > 0);
    CHECK(chunks_after_done == 0);
    CHECK(results == 0);
}

TEST_CASE("The callbacks of the request builder go through the same entry as the others",
          "[client][async][lifetime]") {
    constexpr size_t threads = 1;
    running_workers workers(threads);
    http::test::TestServerFixture fixture;
    silent_server silent;
    callback_log log(2);
    std::atomic<int> websocket_calls{0};
    std::atomic<int> websocket_after_done{0};

    auto client = std::make_unique<http::async_client>(workers);
    auto websocket_callback = [&](std::shared_ptr<http::websocket_client>) {
        ++websocket_calls;
        if (log.client_done) ++websocket_after_done;
    };

    SECTION("Cancelled in flight, they are not called once the client is gone") {
        client->request(silent.url("/plain")).get(log.callback(0));
        client->request(silent.url("/socket", "ws")).websocket(websocket_callback);
        REQUIRE(eventually([&] { return silent.accepted == 2; }, 2s));
        client.reset();
        log.client_done = true;
        CHECK(eventually([&] { return silent.all_closed(); }, 2s));
    }

    SECTION("Running, the destructor waits for them") {
        std::atomic<bool> entered{false};
        std::atomic<bool> returned{false};
        client->request(fixture.base_url + "/get").get([&](http::client_response&) {
            entered = true;
            std::this_thread::sleep_for(300ms);
            returned = true;
        });
        REQUIRE(eventually([&] { return entered.load(); }, 5s));
        client.reset();
        CHECK(returned);
        log.client_done = true;
    }

    SECTION("Not started when a callback destroys the client, they never run") {
        std::atomic<bool> entered{false};
        std::atomic<bool> queued{false};
        client->get(fixture.base_url + "/get", [&](http::client_response&) {
            entered = true;
            eventually([&] { return queued.load(); }, 5s);
            client.reset();
            log.client_done = true;
        });
        REQUIRE(eventually([&] { return entered.load(); }, 5s));
        // Queued on the worker while the callback holds it
        client->request(silent.url("/plain")).get(log.callback(0));
        client->request(silent.url("/socket", "ws")).websocket(websocket_callback);
        queued = true;
        REQUIRE(eventually([&] { return log.client_done.load(); }, 5s));
        REQUIRE(drain(workers, threads));
        CHECK(silent.accepted == 0);
        CHECK(log.total_calls() == 0);
        CHECK(websocket_calls == 0);
    }

    REQUIRE(drain(workers, threads));
    CHECK(log.after_done == 0);
    CHECK(log.each_at_most_once());
    CHECK(log.not_aborted == 0);
    CHECK(websocket_after_done == 0);
    CHECK(websocket_calls <= 1);
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
    // It still sends and receives on its worker (the coroutine API, on the executor of the
    // WebSocket)
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

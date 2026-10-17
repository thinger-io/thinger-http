#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/executor_client.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_future.hpp>

#include "server_test_helpers.hpp"
#include "../fixtures/ssl_test_server_fixture.hpp"

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

// The executor client runs its requests on an io_context the caller runs (e.g. the IO thread of
// a server coroutine): on the caller's thread, with no thread or timer of its own, cancelled
// with the coroutine awaiting them, and closing its connections when destroyed. Otherwise it
// behaves as the other clients

using namespace thinger;
using namespace std::chrono_literals;
using server_test::running_server;
using tcp = boost::asio::ip::tcp;

namespace {

    using clock = std::chrono::steady_clock;

    // Whether `condition` holds within the timeout
    bool eventually(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 3s) {
        auto deadline = clock::now() + timeout;
        while (clock::now() < deadline) {
            if (condition()) return true;
            std::this_thread::sleep_for(5ms);
        }
        return condition();
    }

    // Run a coroutine on `context`, on this thread, until the context runs out of work: it must,
    // as the client leaves no timer or connection behind
    template<typename T>
    T run(boost::asio::io_context& context, awaitable<T> coroutine) {
        auto result = co_spawn(context, std::move(coroutine), boost::asio::use_future);
        context.run_for(20s);
        REQUIRE(context.stopped());
        context.restart();
        return result.get();
    }

    awaitable<void> sleep_for(std::chrono::milliseconds duration) {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, duration);
        co_await timer.async_wait(use_nothrow_awaitable);
    }

    // The url of a server, set once it listens and read by the routes, on the server thread
    struct shared_url {
        mutable std::mutex mutex;
        std::string value;

        void set(std::string url) {
            std::scoped_lock lock(mutex);
            value = std::move(url);
        }
        std::string get() const {
            std::scoped_lock lock(mutex);
            return value;
        }
    };

    // Routes of the test servers, which redirect to themselves (`self`) or to the other one
    void routes(http::server& server, const shared_url& self, const shared_url& other) {
        server.get("/get", [](http::response& res) { res.json({{"ok", true}}); });
        server.get("/headers", [](http::request& req, http::response& res) {
            nlohmann::json headers = nlohmann::json::object();
            for (const auto& [name, value] : req.get_http_request()->get_headers()) headers[name] = value;
            res.json(headers);
        });
        server.get("/redirect/:n", [&self](http::request& req, http::response& res) {
            int n = std::stoi(req["n"]);
            res.redirect(self.get() + (n > 1 ? "/redirect/" + std::to_string(n - 1) : "/get"));
        });
        server.get("/same-origin", [&self](http::response& res) { res.redirect(self.get() + "/headers"); });
        server.get("/other-origin", [&other](http::response& res) { res.redirect(other.get() + "/headers"); });
    }

    // Two servers on 127.0.0.1, on two ports: two origins
    struct servers {
        shared_url a_url;
        shared_url b_url;
        http::server a;
        http::server b;
        std::optional<running_server> running_a;
        std::optional<running_server> running_b;
        std::string url_a;
        std::string url_b;

        servers() {
            routes(a, a_url, b_url);
            routes(b, b_url, a_url);
            running_a.emplace(a);
            running_b.emplace(b);
            url_a = "http://127.0.0.1:" + std::to_string(running_a->port);
            url_b = "http://127.0.0.1:" + std::to_string(running_b->port);
            a_url.set(url_a);
            b_url.set(url_b);
        }
    };

    // A server that accepts one connection and never answers: tells when the client closes it
    struct silent_server {
        boost::asio::io_context context;
        tcp::acceptor acceptor{context, {boost::asio::ip::make_address("127.0.0.1"), 0}};
        std::atomic<bool> accepted{false};
        std::atomic<bool> closed{false};
        std::thread thread;

        silent_server() {
            thread = std::thread([this] {
                tcp::socket peer(context);
                std::array<char, 4096> buffer{};
                std::function<void()> read = [&] {
                    peer.async_read_some(boost::asio::buffer(buffer), [&](boost::system::error_code ec, size_t) {
                        if (ec) {
                            closed = true;
                            return;
                        }
                        read();
                    });
                };
                acceptor.async_accept(peer, [&](boost::system::error_code ec) {
                    if (ec) return;
                    accepted = true;
                    read();
                });
                context.run_for(10s);
            });
        }

        ~silent_server() {
            boost::asio::post(context, [this] { context.stop(); });
            thread.join();
        }

        std::string url() const { return "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()); }
    };

}

TEST_CASE("io_context_of finds the io_context behind an executor", "[client][executor_client]") {
    boost::asio::io_context context;
    boost::asio::any_io_executor io = context.get_executor();

    CHECK(http::io_context_of(context.get_executor()) == &context);
    CHECK(http::io_context_of(io) == &context);
    CHECK(http::io_context_of(boost::asio::make_strand(context)) == &context);
    CHECK(http::io_context_of(boost::asio::make_strand(io)) == &context);
    CHECK(http::io_context_of(boost::asio::make_strand(boost::asio::any_io_executor(boost::asio::make_strand(io)))) ==
          &context);

    boost::asio::thread_pool pool(1);
    CHECK(http::io_context_of(pool.get_executor()) == nullptr);
    CHECK(http::io_context_of(boost::asio::make_strand(boost::asio::any_io_executor(pool.get_executor()))) == nullptr);
    CHECK(http::io_context_of(boost::asio::any_io_executor()) == nullptr);
    pool.join();
}

TEST_CASE("The executor client runs its requests on the caller's thread", "[client][executor_client]") {
    servers origins;
    boost::asio::io_context context;
    auto caller = std::this_thread::get_id();

    SECTION("On the io_context") {
        auto ok = run(context, [&]() -> awaitable<bool> {
            auto client = co_await http::executor_client::for_current_coroutine();
            auto response = co_await client->get(origins.url_a + "/get");
            co_return response.ok() && std::this_thread::get_id() == caller;
        }());
        CHECK(ok);
    }

    SECTION("On a strand of the io_context") {
        auto request = [&]() -> awaitable<bool> {
            auto client = co_await http::executor_client::for_current_coroutine();
            auto response = co_await client->get(origins.url_a + "/get");
            co_return response.ok() && std::this_thread::get_id() == caller;
        };
        auto ok = co_spawn(boost::asio::make_strand(context), request(), boost::asio::use_future);
        context.run_for(20s);
        REQUIRE(context.stopped());
        CHECK(ok.get());
    }

    SECTION("With the request builder") {
        auto status = run(context, [&]() -> awaitable<int> {
            http::executor_client client(context);
            auto response = co_await client.request(origins.url_a + "/headers").header("X-Test", "builder").get();
            co_return response.json().value("X-Test", "") == "builder" ? response.status() : 0;
        }());
        CHECK(status == 200);
    }

    SECTION("A coroutine of an executor without io_context cannot have one") {
        boost::asio::thread_pool pool(1);
        auto client = co_spawn(pool, http::executor_client::for_current_coroutine(), boost::asio::use_future);
        CHECK_THROWS_AS(client.get(), std::logic_error);
        pool.join();
    }
}

TEST_CASE("Server coroutines call other servers with the executor client", "[client][executor_client]") {
    servers origins;
    http::server gateway;
    std::atomic<bool> same_thread{false};
    gateway.get("/gateway", [&](http::response& res) -> awaitable<void> {
        auto thread = std::this_thread::get_id();
        auto client = co_await http::executor_client::for_current_coroutine();
        auto response = co_await client->get(origins.url_a + "/get");
        same_thread = std::this_thread::get_id() == thread;
        res.json({{"upstream", response.status()}});
    });
    running_server running(gateway);

    http::client client;
    auto response = client.get("http://127.0.0.1:" + std::to_string(running.port) + "/gateway");
    REQUIRE(response.ok());
    CHECK(response.json()["upstream"] == 200);
    CHECK(same_thread);
}

TEST_CASE("Cancelling the coroutine cancels the executor client's request", "[client][executor_client]") {
    boost::asio::io_context context;

    SECTION("Raced against a timer") {
        silent_server upstream;
        auto start = clock::now();
        auto winner = run(context, [&]() -> awaitable<size_t> {
            http::executor_client client(context);
            auto result = co_await (client.get(upstream.url() + "/never") || sleep_for(200ms));
            co_return result.index();
        }());
        CHECK(winner == 1);
        CHECK(clock::now() - start < 5s);
        CHECK(upstream.accepted);
        CHECK(eventually([&] { return upstream.closed.load(); }));
    }

    SECTION("With a cancellation signal") {
        silent_server upstream;
        boost::asio::cancellation_signal cancel;
        std::atomic<bool> finished{false};
        co_spawn(context, [&]() -> awaitable<void> {
            http::executor_client client(context);
            try {
                auto response = co_await client.get(upstream.url() + "/never");
                CHECK(response.has_error());
            } catch (const boost::system::system_error& e) {
                CHECK(e.code() == boost::asio::error::operation_aborted);
            }
            finished = true;
        }, boost::asio::bind_cancellation_slot(cancel.slot(), detached));
        boost::asio::steady_timer timer(context, 200ms);
        timer.async_wait([&](boost::system::error_code) { cancel.emit(boost::asio::cancellation_type::terminal); });
        context.run_for(20s);
        REQUIRE(context.stopped());
        CHECK(finished);
        CHECK(eventually([&] { return upstream.closed.load(); }));
    }
}

TEST_CASE("The executor client closes its connections", "[client][executor_client]") {
    servers origins;
    boost::asio::io_context context;

    SECTION("It keeps no idle connection by default") {
        auto idle = run(context, [&]() -> awaitable<size_t> {
            http::executor_client client(context);
            auto response = co_await client.get(origins.url_a + "/get");
            REQUIRE(response.ok());
            co_return client.pool_size();
        }());
        CHECK(idle == 0);
        CHECK(eventually([&] { return origins.a.stats().active_connections() == 0; }));
    }

    SECTION("Destroying it closes the idle connections kept with keep-alive") {
        auto client = std::make_unique<http::executor_client>(context);
        client->max_idle_connections_per_host(4);
        auto idle = run(context, [&]() -> awaitable<size_t> {
            auto first = co_await client->get(origins.url_a + "/get");
            auto second = co_await client->get(origins.url_a + "/get");
            REQUIRE((first.ok() && second.ok()));
            co_return client->pool_size();
        }());
        CHECK(idle == 1);
        CHECK(client->pool_stats().reused == 1);
        CHECK(eventually([&] { return origins.a.stats().active_connections() == 1; }));

        client.reset();
        CHECK(eventually([&] { return origins.a.stats().active_connections() == 0; }));
    }
}

TEST_CASE("The executor client follows redirects as the other clients", "[client][executor_client][redirect]") {
    servers origins;
    boost::asio::io_context context;
    http::executor_client client(context);

    SECTION("Redirects are followed, up to the limit") {
        auto followed = run(context, client.get(origins.url_a + "/redirect/3"));
        CHECK(followed.status() == 200);

        client.max_redirects(2);
        auto limited = run(context, client.get(origins.url_a + "/redirect/3"));
        CHECK(limited.is_redirect());
    }

    SECTION("Credentials go to the same origin only") {
        http::headers_map credentials{{"Authorization", "Bearer secret-token"}};
        auto same = run(context, client.get(origins.url_a + "/same-origin", credentials));
        REQUIRE(same.ok());
        CHECK(same.json().value("Authorization", "") == "Bearer secret-token");

        auto other = run(context, client.get(origins.url_a + "/other-origin", credentials));
        REQUIRE(other.ok());
        CHECK_FALSE(other.json().contains("Authorization"));
    }
}

TEST_CASE("The executor client speaks TLS as the other clients", "[client][executor_client][ssl]") {
    thinger::http::test::SSLTestServerFixture fixture;
    boost::asio::io_context context;
    http::executor_client client(context);
    client.timeout(10s);

    SECTION("A self-signed certificate is refused by default") {
        auto response = run(context, client.get(fixture.base_url + "/get"));
        CHECK(response.has_network_error());
    }

    SECTION("Without verification") {
        client.verify_ssl(false);
        auto response = run(context, client.get(fixture.base_url + "/get"));
        REQUIRE(response.ok());
        CHECK(response.json()["secure"] == true);
    }
}

TEST_CASE("Executor clients on strands of an io_context run by several threads", "[client][executor_client][threading]") {
    servers origins;
    boost::asio::io_context context;
    std::atomic<int> succeeded{0};
    constexpr int coroutines = 8;
    constexpr int requests = 3;

    for (int i = 0; i < coroutines; ++i) {
        co_spawn(boost::asio::make_strand(context), [&]() -> awaitable<void> {
            auto client = co_await http::executor_client::for_current_coroutine();
            client->max_idle_connections_per_host(2);
            for (int r = 0; r < requests; ++r) {
                auto response = co_await client->get(origins.url_a + "/get");
                if (response.ok()) ++succeeded;
            }
        }, detached);
    }

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back([&] { context.run_for(20s); });
    for (auto& thread : threads) thread.join();
    CHECK(succeeded == coroutines * requests);
}

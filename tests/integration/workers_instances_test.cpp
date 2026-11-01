#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/pool_server.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/client/async_client.hpp>
#include <boost/asio/post.hpp>
#include "../fixtures/running_workers.hpp"

#include <chrono>
#include <filesystem>
#include <future>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include "../fixtures/server_listen.hpp"

// Two workers in one process: each runs a pool server and an async client of its own, and
// neither confuses the other's threads

using namespace thinger;
using namespace std::chrono_literals;
using http::test::running_workers;

namespace {

    // Which of the two workers the caller thread belongs to: "A", "B", "AB" or "none"
    std::string owner(const asio::workers& a, const asio::workers& b) {
        std::string result;
        if (a.in_worker_thread()) result += "A";
        if (b.in_worker_thread()) result += "B";
        return result.empty() ? "none" : result;
    }

    // What `work` returns, run on `executor`
    template<typename Work>
    auto run_on(const boost::asio::any_io_executor& executor, Work work) {
        std::promise<decltype(work())> result;
        auto future = result.get_future();
        boost::asio::post(executor, [&] { result.set_value(work()); });
        REQUIRE(future.wait_for(5s) == std::future_status::ready);
        return future.get();
    }

    // A pool server on `workers`, listening on an ephemeral port, whose /owner answers which
    // workers its handler runs on, and records the thread
    struct owner_server {
        http::pool_server server;
        std::mutex mutex;
        std::set<std::thread::id> threads;

        owner_server(asio::workers& workers, const asio::workers& a, const asio::workers& b) : server(workers) {
            server.get("/owner", [this, &a, &b](http::response& res) {
                {
                    std::scoped_lock lock(mutex);
                    threads.insert(std::this_thread::get_id());
                }
                res.send(owner(a, b));
            });
            REQUIRE_FALSE(server.listen("127.0.0.1", 0));
        }

        std::set<std::thread::id> handler_threads() {
            std::scoped_lock lock(mutex);
            return threads;
        }

        std::string url() const { return "http://127.0.0.1:" + std::to_string(http::test::listening_port(server)) + "/owner"; }
    };

    // A response a client got: from which server, what it answered, and where the callback ran
    struct received {
        std::string server;
        bool ok = false;
        std::string body;
        std::string callback_owner;
        std::thread::id thread;
    };

    // An async client on `workers` that records its responses
    struct recording_client {
        http::async_client client;
        std::mutex mutex;
        std::vector<received> responses;

        explicit recording_client(asio::workers& workers) : client(workers) {}

        std::vector<received> received_responses() {
            std::scoped_lock lock(mutex);
            return responses;
        }

        void request(const std::string& server, const std::string& url, const asio::workers& a, const asio::workers& b) {
            client.get(url, [this, server, &a, &b](http::client_response& res) {
                received response{server, res.ok(), res.body(), owner(a, b), std::this_thread::get_id()};
                std::scoped_lock lock(mutex);
                responses.push_back(std::move(response));
            });
        }
    };

#ifdef __linux__
    // The threads of the process
    size_t thread_count() {
        auto tasks = std::filesystem::directory_iterator("/proc/self/task");
        return std::distance(std::filesystem::begin(tasks), std::filesystem::end(tasks));
    }
#endif

}

TEST_CASE("Two workers in one process, each with a pool server and an async client",
          "[asio][workers][instances][integration]") {
    running_workers a(2);
    running_workers b(3);
    owner_server server_a(a, a, b);
    owner_server server_b(b, a, b);
    recording_client client_a(a);
    recording_client client_b(b);

    SECTION("Each request is served on its server's workers, and answered on its client's") {
        constexpr int rounds = 5;
        for (int i = 0; i < rounds; ++i) {
            for (auto* client : {&client_a, &client_b}) {
                client->request("A", server_a.url(), a, b);
                client->request("B", server_b.url(), a, b);
            }
        }
        REQUIRE(client_a.client.wait_for(10s));
        REQUIRE(client_b.client.wait_for(10s));

        auto threads_a = server_a.handler_threads();
        auto threads_b = server_b.handler_threads();
        for (auto [client, tag, threads] : {std::tuple{&client_a, "A", &threads_a}, std::tuple{&client_b, "B", &threads_b}}) {
            auto responses = client->received_responses();
            REQUIRE(responses.size() == 2 * rounds);
            for (const auto& response : responses) {
                REQUIRE(response.ok);
                // the handler of server X ran on the workers of X
                REQUIRE(response.body == response.server);
                // the callback ran on the workers of the client
                REQUIRE(response.callback_owner == tag);
                threads->insert(response.thread);
            }
        }
        // no thread runs on both
        for (auto thread : threads_a) REQUIRE_FALSE(threads_b.contains(thread));
        REQUIRE_FALSE(threads_a.contains(std::this_thread::get_id()));
        REQUIRE_FALSE(threads_b.contains(std::this_thread::get_id()));
    }

    SECTION("Each tells its own threads only") {
        REQUIRE(run_on(a.next_executor(), [&] { return owner(a, b); }) == "A");
        REQUIRE(run_on(b.next_executor(), [&] { return owner(a, b); }) == "B");
        REQUIRE(run_on(a.isolated_executor("isolated-a"), [&] { return owner(a, b); }) == "A");
        REQUIRE(run_on(b.isolated_executor("isolated-b"), [&] { return owner(a, b); }) == "B");
        REQUIRE(owner(a, b) == "none");
    }

    SECTION("Stopping A leaves B serving and requesting") {
        // the application stops what runs on A first, then A
        client_a.client.stop();
        server_a.server.stop();
        REQUIRE_FALSE(server_a.server.is_listening());
        a.stop();
        REQUIRE_FALSE(a.running());
        REQUIRE(b.running());

        for (int i = 0; i < 3; ++i) client_b.request("B", server_b.url(), a, b);
        REQUIRE(client_b.client.wait_for(10s));
        auto responses = client_b.received_responses();
        REQUIRE(responses.size() == 3);
        for (const auto& response : responses) {
            REQUIRE(response.ok);
            REQUIRE(response.body == "B");
            REQUIRE(response.callback_owner == "B");
        }
        REQUIRE(run_on(b.next_executor(), [&] { return owner(a, b); }) == "B");
    }
}

TEST_CASE("Nothing starts workers by themselves", "[asio][workers][instances][integration]") {
#ifdef __linux__
    // A thread started and joined first: a runtime that starts a thread of its own with the first
    // one the process creates (ThreadSanitizer) has started it before the count
    std::thread([] {}).join();
    auto threads_before = thread_count();
#endif
    asio::workers workers;
    {
        http::pool_server server(workers);
        http::async_client client(workers);
        REQUIRE_FALSE(workers.running());
        REQUIRE(!workers.next_executor());
#ifdef __linux__
        // no thread was created
        REQUIRE(thread_count() == threads_before);
#endif
    }
    REQUIRE_FALSE(workers.running());

    // until the application starts them
    REQUIRE_FALSE(workers.start(2));
#ifdef __linux__
    REQUIRE(thread_count() == threads_before + 2);
#endif
    workers.stop();
}

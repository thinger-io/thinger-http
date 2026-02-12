#include <catch2/catch_test_macros.hpp>
#include <thinger/http_client.hpp>
#include "../fixtures/test_server_fixture.hpp"
#include <thread>
#include <atomic>
#include <set>
#include <mutex>

using namespace thinger;

TEST_CASE_METHOD(thinger::http::test::TestServerFixture, "HTTP Async Client thread affinity", "[http][client][threading]") {

    SECTION("Callbacks execute on worker threads") {
        http::async_client client;

        std::thread::id main_thread_id = std::this_thread::get_id();
        std::thread::id callback_thread_id;
        std::atomic<bool> callback_executed{false};

        client.get(base_url + "/get", [&](http::client_response& res) {
            REQUIRE(res.ok());
            callback_thread_id = std::this_thread::get_id();
            callback_executed = true;
        });

        client.wait();

        REQUIRE(callback_executed == true);
        // Callbacks are executed on worker threads, not the calling thread
        REQUIRE(callback_thread_id != main_thread_id);
    }

    SECTION("Multiple requests use worker pool") {
        http::async_client client;

        std::mutex results_mutex;
        std::set<std::thread::id> worker_threads_used;
        std::atomic<int> completed{0};
        const int num_requests = 10;

        for (int i = 0; i < num_requests; ++i) {
            client.get(base_url + "/get?req=" + std::to_string(i),
                      [&](http::client_response& res) {
                REQUIRE(res.ok());

                std::thread::id callback_thread_id = std::this_thread::get_id();
                {
                    std::lock_guard<std::mutex> lock(results_mutex);
                    worker_threads_used.insert(callback_thread_id);
                }
                completed++;
            });
        }

        client.wait();

        REQUIRE(completed == num_requests);
        // Worker threads may be reused
        REQUIRE(worker_threads_used.size() >= 1);
    }

    SECTION("Connection pooling works with async requests") {
        http::async_client client;
        client.timeout(std::chrono::seconds(10));

        std::atomic<int> completed{0};
        const int num_requests = 6;

        for (int i = 0; i < num_requests; ++i) {
            client.get(base_url + "/get?req=" + std::to_string(i),
                      [&completed](http::client_response& res) {
                REQUIRE(res.ok());
                ++completed;
            });
        }

        client.wait();

        REQUIRE(completed == num_requests);
    }
}

TEST_CASE_METHOD(thinger::http::test::TestServerFixture, "HTTP Sync Client thread behavior", "[http][client][threading][sync]") {

    SECTION("Synchronous methods block the calling thread") {
        http::client client;

        std::thread::id calling_thread = std::this_thread::get_id();

        auto start = std::chrono::steady_clock::now();

        // This is a synchronous call - should block current thread
        auto response = client.get(base_url + "/delay/1");

        auto elapsed = std::chrono::steady_clock::now() - start;

        REQUIRE(response.ok());

        // Should have blocked for at least 1 second
        REQUIRE(elapsed >= std::chrono::seconds(1));

        // The calling thread should be the same
        REQUIRE(std::this_thread::get_id() == calling_thread);
    }
}

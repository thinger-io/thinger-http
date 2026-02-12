#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/async_client.hpp>
#include <thinger/util/logger.hpp>
#include "../fixtures/test_server_fixture.hpp"
#include <chrono>
#include <atomic>
#include <thread>
#include <future>

using namespace thinger;
using namespace std::chrono_literals;

// wait() functionality only applies to async_client (http::client is synchronous)
TEST_CASE("HTTP Async Client wait() functionality", "[http][client][wait]") {

    thinger::http::test::TestServerFixture fixture;
    auto& base_url = fixture.base_url;

    SECTION("wait() blocks until all requests complete") {
        http::async_client client;
        std::atomic<int> completed_requests{0};
        const int total_requests = 5;

        auto start = std::chrono::steady_clock::now();

        // Launch multiple requests
        for (int i = 0; i < total_requests; ++i) {
            client.get(base_url + "/get", [&](http::client_response& res) {
                if (res.ok()) {
                    completed_requests++;
                }
            });
        }

        // wait() should block until all requests complete
        client.wait();

        REQUIRE(completed_requests == total_requests);
    }

    SECTION("wait() returns immediately when no active requests") {
        http::async_client client;

        auto start = std::chrono::steady_clock::now();
        client.wait();
        auto elapsed = std::chrono::steady_clock::now() - start;

        REQUIRE(elapsed < 100ms);
    }

    SECTION("wait() can be called multiple times") {
        http::async_client client;

        // First batch of requests
        std::atomic<int> batch1_completed{0};
        for (int i = 0; i < 3; ++i) {
            client.get(base_url + "/get", [&](http::client_response& res) {
                if (res.ok()) batch1_completed++;
            });
        }

        client.wait();
        REQUIRE(batch1_completed == 3);

        // Second batch of requests
        std::atomic<int> batch2_completed{0};
        for (int i = 0; i < 2; ++i) {
            client.get(base_url + "/get", [&](http::client_response& res) {
                if (res.ok()) batch2_completed++;
            });
        }

        client.wait();
        REQUIRE(batch2_completed == 2);
    }

    SECTION("wait() unblocks when client is stopped") {
        http::async_client client;

        // Start a long-running request
        client.get(base_url + "/delay/10", [&](http::client_response& res) {
            // Callback may or may not be called depending on stop() timing
        });

        // Stop the client in another thread after a short delay
        std::thread stopper([&client]() {
            std::this_thread::sleep_for(500ms);
            client.stop();
        });

        auto start = std::chrono::steady_clock::now();
        client.wait();
        auto elapsed = std::chrono::steady_clock::now() - start;

        stopper.join();

        // Should have unblocked after ~500ms, not 10s
        REQUIRE(elapsed < 2s);
        // Note: callback may not be called if stop() cancels the request before it completes
    }
}

TEST_CASE("HTTP Async Client wait_for() functionality", "[http][client][wait_for]") {

    thinger::http::test::TestServerFixture fixture;
    auto& base_url = fixture.base_url;

    SECTION("wait_for() returns true when requests complete in time") {
        http::async_client client;
        std::atomic<bool> completed{false};

        client.get(base_url + "/delay/1", [&](http::client_response& res) {
            completed = true;
        });

        // Wait for up to 3 seconds
        bool result = client.wait_for(3s);

        REQUIRE(result == true);
        REQUIRE(completed == true);
    }

    SECTION("wait_for() returns false on timeout") {
        http::async_client client;
        std::atomic<bool> completed{false};

        client.get(base_url + "/delay/5", [&](http::client_response& res) {
            completed = true;
        });

        // Wait for only 1 second
        auto start = std::chrono::steady_clock::now();
        bool result = client.wait_for(1s);
        auto elapsed = std::chrono::steady_clock::now() - start;

        REQUIRE(result == false);
        REQUIRE(elapsed >= 1s);
        REQUIRE(elapsed < 2s);

        // Clean up
        client.wait();
    }

    SECTION("pending_requests() returns count of active requests") {
        http::async_client client;

        // Initially no pending requests
        REQUIRE(client.pending_requests() == 0);
        REQUIRE(client.has_pending_requests() == false);

        client.get(base_url + "/delay/1", [](http::client_response& res) {});

        // Now we should have 1 pending request
        REQUIRE(client.pending_requests() == 1);
        REQUIRE(client.has_pending_requests() == true);

        // Wait for the request to complete
        client.wait();

        // Now should have no pending requests
        REQUIRE(client.pending_requests() == 0);
        REQUIRE(client.has_pending_requests() == false);
    }
}

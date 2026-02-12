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

// Test asynchronous API for async_client (http::client is synchronous, so not tested here)
TEST_CASE("HTTP Async Client asynchronous API", "[http][client][async]") {

    // Create test server fixture
    thinger::http::test::TestServerFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;
    auto& port = fixture.port;

    SECTION("Asynchronous GET with callback") {
        http::async_client client;
        std::promise<bool> promise;
        auto future = promise.get_future();

        // Asynchronous call with callback
        client.get(base_url + "/get", [&promise](http::client_response& res) {
            bool success = res.ok() && res.status() == 200;
            promise.set_value(success);
        });

        // Wait for async operation to complete
        client.wait();

        REQUIRE(future.get() == true);
    }

    SECTION("Asynchronous POST with callback") {
        http::async_client client;
        std::promise<bool> promise;
        auto future = promise.get_future();

        std::string post_data = R"({"async": true})";

        // Asynchronous POST with callback
        client.post(base_url + "/post", [&promise](http::client_response& res) {
            bool success = res.ok() && res.status() == 200;
            promise.set_value(success);
        }, post_data, "application/json");

        // Wait for completion
        client.wait();

        REQUIRE(future.get() == true);
    }

    SECTION("Multiple async requests in parallel") {
        http::async_client client;
        std::atomic<int> completed{0};
        const int num_requests = 10;

        auto start = std::chrono::steady_clock::now();

        // Launch multiple requests in parallel
        for (int i = 0; i < num_requests; ++i) {
            client.get(base_url + "/get", [&completed, i](http::client_response& res) {
                if (res.ok()) {
                    completed++;
                    LOG_DEBUG("Request {} completed", i);
                }
            });
        }

        // Wait for all to complete
        client.wait();

        auto duration = std::chrono::steady_clock::now() - start;

        REQUIRE(completed == num_requests);

        // Should be reasonably fast (not serialized)
        LOG_INFO("Completed {} async requests in {}ms",
                num_requests,
                std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
    }

    SECTION("Error handling in async callbacks") {
        http::async_client client;
        std::promise<int> promise;
        auto future = promise.get_future();

        client.get(base_url + "/status/503", [&promise](http::client_response& res) {
            promise.set_value(res.status());
        });

        client.wait();

        REQUIRE(future.get() == 503);
    }

    SECTION("Async timeout handling") {
        http::async_client client;
        client.timeout(1s);

        std::promise<bool> promise;
        auto future = promise.get_future();

        // Request with 3 second delay, but timeout is 1s
        client.get(base_url + "/delay/3", [&promise](http::client_response& res) {
            bool timed_out = !res.ok();
            promise.set_value(timed_out);
        });

        client.wait();

        REQUIRE(future.get() == true); // Should have timed out
    }

    SECTION("Concurrent GET and POST") {
        http::async_client client;
        std::atomic<int> get_done{0};
        std::atomic<int> post_done{0};

        // Launch GETs
        for (int i = 0; i < 5; ++i) {
            client.get(base_url + "/get", [&get_done](http::client_response& res) {
                if (res.ok()) get_done++;
            });
        }

        // Launch POSTs
        for (int i = 0; i < 5; ++i) {
            std::string data = "post_" + std::to_string(i);
            client.post(base_url + "/post", [&post_done](http::client_response& res) {
                if (res.ok()) post_done++;
            }, data);
        }

        client.wait();

        REQUIRE(get_done == 5);
        REQUIRE(post_done == 5);
    }

    SECTION("wait_for with timeout") {
        http::async_client client;

        // Start a slow request
        client.get(base_url + "/delay/3", [](http::client_response& res) {
            // Response handler
        });

        // wait_for should timeout before request completes
        bool completed = client.wait_for(500ms);

        REQUIRE_FALSE(completed); // Should timeout

        // Now wait for completion
        client.wait();
    }

    SECTION("Using run() with coroutines") {
        http::async_client client;
        std::atomic<int> completed{0};

        // Using run() for coroutine-based async
        for (int i = 0; i < 3; ++i) {
            client.run([&client, &completed, &base_url]() -> awaitable<void> {
                auto res = co_await client.get(base_url + "/get");
                if (res.ok()) {
                    completed++;
                }
            });
        }

        client.wait();

        REQUIRE(completed == 3);
    }
}

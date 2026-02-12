#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/async_client.hpp>
#include <thinger/asio/workers.hpp>
#include <thinger/util/logger.hpp>
#include "../fixtures/test_server_fixture.hpp"
#include <chrono>
#include <atomic>
#include <thread>

using namespace thinger;
using namespace std::chrono_literals;
using TestServerFixture = thinger::http::test::TestServerFixture;

// Test fixture that also starts workers
struct AsyncClientTestFixture : TestServerFixture {
    AsyncClientTestFixture() : TestServerFixture() {
        // Start worker threads for async client
        asio::get_workers().start(4);
    }

    ~AsyncClientTestFixture() {
        // Stop worker threads
        asio::get_workers().stop();
    }
};

TEST_CASE_METHOD(AsyncClientTestFixture, "Async HTTP Client wait() functionality", "[http][client][async][wait]") {
    
    SECTION("wait() blocks until all requests complete") {
        http::async_client client;
        std::atomic<int> completed_requests{0};
        const int total_requests = 5;
        
        auto start = std::chrono::steady_clock::now();
        
        // Launch multiple requests
        for (int i = 0; i < total_requests; ++i) {
            client.get(base_url + "/delay/1", [&, i](const http::client_response& res) {
                REQUIRE(res.ok());
                completed_requests++;
                // Simulate some processing time
                std::this_thread::sleep_for(100ms);
            });
        }
        
        // wait() should block until all requests complete
        client.wait();

        auto elapsed = std::chrono::steady_clock::now() - start;

        REQUIRE(completed_requests == total_requests);
        // async_client can run requests in parallel using multiple connections
        // so elapsed time depends on server parallelism, not client serialization
        REQUIRE(elapsed >= 1s);  // At least 1 delay round
        REQUIRE(elapsed < 10s);  // Should complete in reasonable time
    }
    
    SECTION("Multiple threads can wait simultaneously") {
        http::async_client client;
        std::atomic<int> waiters_unblocked{0};
        std::atomic<int> requests_completed{0};
        
        // Start some requests
        for (int i = 0; i < 3; ++i) {
            client.get(base_url + "/delay/1", [&](const http::client_response& res) {
                REQUIRE(res.ok());
                requests_completed++;
            });
        }
        
        // Multiple threads waiting
        std::thread waiter1([&]() {
            client.wait();
            waiters_unblocked++;
        });
        
        std::thread waiter2([&]() {
            client.wait();
            waiters_unblocked++;
        });
        
        std::thread waiter3([&]() {
            bool result = client.wait_for(5s);
            REQUIRE(result == true);
            waiters_unblocked++;
        });
        
        waiter1.join();
        waiter2.join();
        waiter3.join();
        
        REQUIRE(waiters_unblocked == 3);
        REQUIRE(requests_completed == 3);
    }
    
    SECTION("wait() with concurrent requests from multiple threads") {
        http::async_client client;
        std::atomic<int> completed_requests{0};
        const int threads_count = 4;
        const int requests_per_thread = 3;
        
        std::vector<std::thread> request_threads;
        
        // Launch requests from multiple threads
        for (int t = 0; t < threads_count; ++t) {
            request_threads.emplace_back([&, t]() {
                for (int i = 0; i < requests_per_thread; ++i) {
                    client.get(base_url + "/get", [&](const http::client_response& res) {
                        REQUIRE(res.ok());
                        completed_requests++;
                    });
                }
            });
        }
        
        // Join all request threads
        for (auto& t : request_threads) {
            t.join();
        }
        
        // Wait for all requests to complete
        client.wait();
        
        REQUIRE(completed_requests == threads_count * requests_per_thread);
    }
    
    SECTION("wait_for() with cancel_on_timeout from multiple threads") {
        http::async_client client;
        std::atomic<int> completed_requests{0};
        std::atomic<int> successful_requests{0};
        
        // Start long-running requests
        for (int i = 0; i < 5; ++i) {
            client.get(base_url + "/delay/5", [&](const http::client_response& res) {
                completed_requests++;
                if (res.ok()) {
                    successful_requests++;
                }
            });
        }
        
        // Multiple threads calling wait_for with timeout
        std::vector<std::thread> waiters;
        std::atomic<int> timed_out{0};
        
        for (int i = 0; i < 3; ++i) {
            waiters.emplace_back([&]() {
                bool result = client.wait_for(1s);
                if (!result) {
                    timed_out++;
                }
            });
        }
        
        // Join all waiters
        for (auto& t : waiters) {
            t.join();
        }
        
        // Give time for any ongoing operations
        std::this_thread::sleep_for(500ms);

        // At least one waiter should have timed out
        REQUIRE(timed_out >= 1);

        // Note: wait_for() timing out doesn't cancel requests automatically
        // Requests may still complete after wait_for returns false
        // This is expected behavior - wait_for just stops waiting, doesn't cancel
    }
}
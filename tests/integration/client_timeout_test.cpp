#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/client/async_client.hpp>
#include "../fixtures/test_server_fixture.hpp"
#include <chrono>

using namespace thinger;
using namespace std::chrono_literals;

// Test timeout with synchronous client
TEST_CASE("HTTP Client Timeout (sync)", "[http][client][timeout][sync]") {

    thinger::http::test::TestServerFixture fixture;
    auto& base_url = fixture.base_url;

    SECTION("Connection timeout triggers when no data is received") {
        http::client client;
        client.timeout(2s);

        auto start = std::chrono::steady_clock::now();

        // This endpoint delays response for 5 seconds, should timeout after 2
        auto res = client.get(base_url + "/delay/5");

        auto elapsed = std::chrono::steady_clock::now() - start;

        REQUIRE_FALSE(res.ok());
        REQUIRE(elapsed >= 2s);
        REQUIRE(elapsed < 4s);  // Allow margin for timer precision
    }

    SECTION("Fast requests complete successfully with timeout") {
        http::client client;
        client.timeout(5s);

        // This endpoint delays for 1 second, should complete
        auto res = client.get(base_url + "/delay/1");

        REQUIRE(res.ok());
    }

    SECTION("Multiple clients have independent timeouts") {
        http::client client1;
        http::client client2;

        client1.timeout(2s);
        client2.timeout(10s);

        // client1 should timeout (2s < 5s delay)
        auto res1 = client1.get(base_url + "/delay/5");
        REQUIRE_FALSE(res1.ok());

        // client2 should succeed (10s > 5s delay)
        auto res2 = client2.get(base_url + "/delay/5");
        REQUIRE(res2.ok());
    }

    SECTION("Timeout can be changed between requests") {
        http::client client;

        // First request with short timeout - should timeout
        client.timeout(2s);
        auto res1 = client.get(base_url + "/delay/5");
        REQUIRE_FALSE(res1.ok());

        // Second request with longer timeout - should succeed
        client.timeout(10s);
        auto res2 = client.get(base_url + "/delay/1");
        REQUIRE(res2.ok());
    }
}

// Test timeout with async async_client
TEST_CASE("HTTP Async Client Timeout (async)", "[http][client][timeout][async]") {

    thinger::http::test::TestServerFixture fixture;
    auto& base_url = fixture.base_url;

    SECTION("Connection timeout triggers when no data is received") {
        http::async_client client;
        client.timeout(2s);

        std::promise<bool> promise;
        auto future = promise.get_future();

        auto start = std::chrono::steady_clock::now();

        client.get(base_url + "/delay/5", [&promise](http::client_response& res) {
            promise.set_value(!res.ok());
        });

        client.wait();

        auto elapsed = std::chrono::steady_clock::now() - start;

        REQUIRE(future.get() == true);  // Should have timed out
        REQUIRE(elapsed >= 2s);
        REQUIRE(elapsed < 4s);
    }

    SECTION("Fast requests complete successfully with timeout") {
        http::async_client client;
        client.timeout(5s);

        std::promise<bool> promise;
        auto future = promise.get_future();

        client.get(base_url + "/delay/1", [&promise](http::client_response& res) {
            promise.set_value(res.ok());
        });

        client.wait();

        REQUIRE(future.get() == true);
    }

    SECTION("Multiple async_clients have independent timeouts") {
        http::async_client client1;
        http::async_client client2;

        client1.timeout(2s);
        client2.timeout(10s);

        std::promise<bool> promise1, promise2;
        auto future1 = promise1.get_future();
        auto future2 = promise2.get_future();

        client1.get(base_url + "/delay/5", [&promise1](http::client_response& res) {
            promise1.set_value(!res.ok());  // Should timeout
        });

        client2.get(base_url + "/delay/5", [&promise2](http::client_response& res) {
            promise2.set_value(res.ok());  // Should succeed
        });

        client1.wait();
        client2.wait();

        REQUIRE(future1.get() == true);  // client1 timed out
        REQUIRE(future2.get() == true);  // client2 succeeded
    }
}

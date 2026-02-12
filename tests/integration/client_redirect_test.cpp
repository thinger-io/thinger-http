#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/client/async_client.hpp>
#include "../fixtures/test_server_fixture.hpp"
#include <nlohmann/json.hpp>
#include <thinger/util/logger.hpp>

using namespace thinger;

// Test redirect configuration (works for both clients)
TEMPLATE_TEST_CASE("HTTP Client redirect configuration", "[http][client][redirect][config]",
                   http::client, http::async_client) {

    SECTION("Client has default redirect settings") {
        TestType client;
        REQUIRE(client.get_follow_redirects() == true);
        REQUIRE(client.get_max_redirects() == 5); // Default max redirects
    }

    SECTION("Can configure max redirects") {
        TestType client;
        client.max_redirects(10);
        REQUIRE(client.get_max_redirects() == 10);
    }

    SECTION("Can disable redirect following") {
        TestType client;
        client.follow_redirects(false);
        REQUIRE(client.get_follow_redirects() == false);
    }

    SECTION("Fluent API for redirect configuration") {
        TestType client;
        client.max_redirects(3).follow_redirects(true);
        REQUIRE(client.get_max_redirects() == 3);
        REQUIRE(client.get_follow_redirects() == true);
    }
}

// Test redirect handling with synchronous client
TEST_CASE("HTTP Sync Client redirect handling", "[http][client][redirect][sync]") {

    // Create test server fixture
    thinger::http::test::TestServerFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    SECTION("Client follows redirects by default") {
        http::client client;

        auto res = client.get(base_url + "/redirect/2");

        REQUIRE(res.ok());
        REQUIRE(res.status() == 200);
    }

    SECTION("Client respects max redirect limit") {
        http::client client;
        client.max_redirects(2);

        // This endpoint tries to redirect 5 times
        auto res = client.get(base_url + "/redirect/5");

        // After 2 redirects, we should get the 3rd redirect response (not followed)
        REQUIRE(!res.ok()); // Should NOT be ok because it's a redirect response
        REQUIRE(res.is_redirect());
    }

    SECTION("Client can disable redirect following") {
        http::client client;
        client.follow_redirects(false);

        auto res = client.get(base_url + "/redirect/1");

        // Should get the redirect response, not follow it
        REQUIRE(!res.ok());
        REQUIRE(res.is_redirect());
    }
}

// Test redirect handling with async_client (async callbacks)
TEST_CASE("HTTP Async Client redirect handling", "[http][client][redirect][async]") {

    // Create test server fixture
    thinger::http::test::TestServerFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    SECTION("Client follows redirects by default") {
        http::async_client client;
        std::promise<bool> promise;
        auto future = promise.get_future();

        client.get(base_url + "/redirect/2", [&promise](http::client_response& res) {
            promise.set_value(res.ok() && res.status() == 200);
        });

        client.wait();
        REQUIRE(future.get() == true);
    }

    SECTION("Client respects max redirect limit") {
        http::async_client client;
        client.max_redirects(2);

        std::promise<bool> promise;
        auto future = promise.get_future();

        client.get(base_url + "/redirect/5", [&promise](http::client_response& res) {
            // After 2 redirects, we should get the 3rd redirect response (not followed)
            promise.set_value(!res.ok() && res.is_redirect());
        });

        client.wait();
        REQUIRE(future.get() == true);
    }

    SECTION("Client can disable redirect following") {
        http::async_client client;
        client.follow_redirects(false);

        std::promise<bool> promise;
        auto future = promise.get_future();

        client.get(base_url + "/redirect/1", [&promise](http::client_response& res) {
            promise.set_value(!res.ok() && res.is_redirect());
        });

        client.wait();
        REQUIRE(future.get() == true);
    }

    SECTION("Multiple async_clients handle redirects independently") {
        http::async_client client1;
        http::async_client client2;

        client1.max_redirects(1);
        client2.max_redirects(5);

        std::promise<bool> promise1, promise2;
        auto future1 = promise1.get_future();
        auto future2 = promise2.get_future();

        client1.get(base_url + "/redirect/3", [&promise1](http::client_response& res) {
            // Client1 should stop after 1 redirect
            promise1.set_value(res.is_redirect());
        });

        client2.get(base_url + "/redirect/3", [&promise2](http::client_response& res) {
            // Client2 should follow all redirects
            promise2.set_value(res.ok());
        });

        client1.wait();
        client2.wait();

        REQUIRE(future1.get() == true);
        REQUIRE(future2.get() == true);
    }
}

#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/util/logger.hpp>
#include "../fixtures/test_server_fixture.hpp"
#include <chrono>

using namespace thinger;
using namespace std::chrono_literals;

// Test synchronous API for http::client (the only synchronous client)
// For async/async_client tests, see client_async_test.cpp
TEST_CASE("HTTP Client synchronous API", "[http][client][sync]") {

    // Create test server fixture
    thinger::http::test::TestServerFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    SECTION("Synchronous GET returns response directly") {
        http::client client;

        auto response = client.get(base_url + "/get");

        REQUIRE(response.ok());
        REQUIRE(response.status() == 200);
        REQUIRE(!response.body().empty());

        // Parse JSON response
        auto json = response.json();
        REQUIRE(json.contains("method"));
        REQUIRE(json["method"] == "GET");
    }

    SECTION("Synchronous POST with body") {
        http::client client;

        std::string post_data = R"({"test": "data", "number": 42})";

        http::headers_map empty_headers;
        auto response = client.post(base_url + "/post", post_data, "application/json", empty_headers);

        REQUIRE(response.ok());
        REQUIRE(response.status() == 200);

        // Check echo response
        auto json = response.json();
        REQUIRE(json.contains("body"));
        auto body_json = nlohmann::json::parse(json["body"].template get<std::string>());
        REQUIRE(body_json["test"] == "data");
        REQUIRE(body_json["number"] == 42);
    }

    SECTION("Synchronous GET with custom headers") {
        http::client client;

        std::map<std::string, std::string> headers = {
            {"X-Custom-Header", "test-value"},
            {"Authorization", "Bearer token123"}
        };

        auto response = client.get(base_url + "/headers", headers);

        REQUIRE(response.ok());

        // Check that headers were sent
        auto json = response.json();
        REQUIRE(json.contains("headers"));
        auto received_headers = json["headers"];
        REQUIRE(received_headers.contains("X-Custom-Header"));
        REQUIRE(received_headers["X-Custom-Header"] == "test-value");
    }

    SECTION("Synchronous POST with custom headers") {
        http::client client;

        std::map<std::string, std::string> headers = {
            {"X-API-Key", "secret123"}
        };

        std::string body = R"({"message": "hello"})";
        auto response = client.post(base_url + "/post", body, "application/json", headers);

        REQUIRE(response.ok());

        auto json = response.json();
        REQUIRE(json.contains("headers"));
        auto received_headers = json["headers"];
        REQUIRE(received_headers.contains("X-API-Key"));
        REQUIRE(received_headers["X-API-Key"] == "secret123");
    }

    SECTION("Error handling - 404 Not Found") {
        http::client client;

        auto response = client.get(base_url + "/status/404");

        REQUIRE_FALSE(response.ok());
        REQUIRE(response.status() == 404);
        REQUIRE(response.is_client_error());
    }

    SECTION("Error handling - 500 Internal Server Error") {
        http::client client;

        auto response = client.get(base_url + "/status/500");

        REQUIRE_FALSE(response.ok());
        REQUIRE(response.status() == 500);
        REQUIRE(response.is_server_error());
    }

    SECTION("Timeout handling") {
        http::client client;
        client.timeout(1s);

        // This should timeout (requesting 5 second delay with 1 second timeout)
        auto response = client.get(base_url + "/delay/5");

        REQUIRE_FALSE(response.ok());
    }

    SECTION("Multiple synchronous requests in sequence") {
        http::client client;

        auto r1 = client.get(base_url + "/get");
        REQUIRE(r1.ok());
        REQUIRE(r1.status() == 200);

        http::headers_map no_headers;
        auto r2 = client.post(base_url + "/post", "test data", "text/plain", no_headers);
        REQUIRE(r2.ok());
        REQUIRE(r2.status() == 200);

        auto r3 = client.get(base_url + "/status/201");
        REQUIRE(r3.status() == 201);

        auto r4 = client.get(base_url + "/headers");
        REQUIRE(r4.ok());
    }

    SECTION("Empty POST body") {
        http::client client;

        auto response = client.post(base_url + "/post");

        REQUIRE(response.ok());
        REQUIRE(response.status() == 200);
    }

    SECTION("Large response handling") {
        http::client client;

        auto response = client.get(base_url + "/large");

        REQUIRE(response.ok());
        REQUIRE(response.body().size() > 10000);
    }
}

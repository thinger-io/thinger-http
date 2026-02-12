#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/client/async_client.hpp>
#include <thinger/util/types.hpp>
#include "../fixtures/ssl_test_server_fixture.hpp"
#include <chrono>
#include <future>
#include <atomic>

using namespace thinger;
using namespace std::chrono_literals;

// SSL integration tests using local HTTPS server with self-signed certificate
// This improves test reliability and coverage

TEST_CASE("HTTPS Client Basic Requests", "[ssl][integration]") {
    // Create SSL test server fixture
    thinger::http::test::SSLTestServerFixture fixture;
    auto& base_url = fixture.base_url;

    SECTION("GET request to HTTPS endpoint (verify_ssl=false for self-signed)") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);  // Self-signed certificate

        auto response = client.get(base_url + "/get");

        REQUIRE(response.ok());
        REQUIRE(response.status() == 200);
        REQUIRE(!response.body().empty());

        auto json = response.json();
        REQUIRE(json.contains("method"));
        REQUIRE(json["method"] == "GET");
        REQUIRE(json["secure"] == true);  // Verify SSL was used
    }

    SECTION("POST request to HTTPS endpoint") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);

        std::string post_data = R"({"test": "ssl_data", "secure": true})";
        http::headers_map headers;

        auto response = client.post(base_url + "/post", post_data, "application/json", headers);

        REQUIRE(response.ok());
        REQUIRE(response.status() == 200);

        auto json = response.json();
        REQUIRE(json.contains("json"));
        REQUIRE(json["json"]["test"] == "ssl_data");
        REQUIRE(json["json"]["secure"] == true);
        REQUIRE(json["secure"] == true);  // Server confirms SSL
    }

    SECTION("HTTPS request with custom headers") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);

        std::map<std::string, std::string> headers = {
            {"X-Custom-Header", "ssl-test-value"},
            {"Authorization", "Bearer test-token"}
        };

        auto response = client.get(base_url + "/headers", headers);

        REQUIRE(response.ok());

        auto json = response.json();
        REQUIRE(json.contains("headers"));
        REQUIRE(json["headers"].contains("X-Custom-Header"));
        REQUIRE(json["headers"]["X-Custom-Header"] == "ssl-test-value");
    }
}

TEST_CASE("HTTPS Client SSL Configuration", "[ssl][integration]") {
    thinger::http::test::SSLTestServerFixture fixture;
    auto& base_url = fixture.base_url;

    SECTION("SSL verification is enabled by default") {
        http::client client;
        REQUIRE(client.get_verify_ssl() == true);
    }

    SECTION("SSL verification can be disabled") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);

        REQUIRE(client.get_verify_ssl() == false);

        auto response = client.get(base_url + "/get");
        REQUIRE(response.ok());
    }

    SECTION("Request with custom User-Agent header") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);
        client.user_agent("TestClient/1.0");

        auto response = client.get(base_url + "/user-agent");

        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json.contains("user-agent"));
        REQUIRE(json["user-agent"] == "TestClient/1.0");
    }
}

TEST_CASE("HTTPS Client Error Handling", "[ssl][integration]") {
    thinger::http::test::SSLTestServerFixture fixture;
    auto& base_url = fixture.base_url;

    SECTION("Connection failure to non-existent HTTPS host") {
        http::client client;
        client.timeout(3s);
        client.verify_ssl(false);

        auto response = client.get("https://this-host-definitely-does-not-exist-12345.com/test");

        REQUIRE_FALSE(response.ok());
    }

    SECTION("HTTPS request with short timeout") {
        http::client client;
        client.timeout(1s);
        client.verify_ssl(false);

        // Request a delayed response that will exceed timeout
        auto response = client.get(base_url + "/delay/5");

        REQUIRE_FALSE(response.ok());
    }

    SECTION("HTTPS 404 error handling") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);

        auto response = client.get(base_url + "/status/404");

        REQUIRE_FALSE(response.ok());
        REQUIRE(response.status() == 404);
        REQUIRE(response.is_client_error());
    }

    SECTION("HTTPS 500 error handling") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);

        auto response = client.get(base_url + "/status/500");

        REQUIRE_FALSE(response.ok());
        REQUIRE(response.status() == 500);
        REQUIRE(response.is_server_error());
    }
}

TEST_CASE("HTTPS Client Redirects", "[ssl][integration]") {
    thinger::http::test::SSLTestServerFixture fixture;
    auto& base_url = fixture.base_url;

    SECTION("Follow HTTPS redirect") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);
        client.follow_redirects(true);

        auto response = client.get(base_url + "/redirect/1");

        REQUIRE(response.ok());
        REQUIRE(response.status() == 200);
    }

    SECTION("Disable redirect following") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);
        client.follow_redirects(false);

        auto response = client.get(base_url + "/redirect/1");

        // Should get redirect status, not follow it
        REQUIRE(response.status() == 302);
    }

    SECTION("Multiple redirects") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);
        client.follow_redirects(true);
        client.max_redirects(5);

        auto response = client.get(base_url + "/redirect/3");

        REQUIRE(response.ok());
        REQUIRE(response.status() == 200);
    }
}

TEST_CASE("HTTPS Async Client", "[ssl][integration][async]") {
    thinger::http::test::SSLTestServerFixture fixture;
    auto& base_url = fixture.base_url;

    SECTION("Async GET request to HTTPS endpoint with callback") {
        http::async_client client;
        client.timeout(10s);
        client.verify_ssl(false);

        std::promise<bool> promise;
        auto future = promise.get_future();

        client.get(base_url + "/get", [&promise](http::client_response& res) {
            bool success = res.ok() && res.status() == 200 && !res.body().empty();
            promise.set_value(success);
        });

        client.wait();

        REQUIRE(future.get() == true);
    }

    SECTION("Async POST request to HTTPS endpoint with callback") {
        http::async_client client;
        client.timeout(10s);
        client.verify_ssl(false);

        std::promise<bool> promise;
        auto future = promise.get_future();

        std::string post_data = R"({"async": true})";

        client.post(base_url + "/post", [&promise](http::client_response& res) {
            bool success = false;
            if (res.ok()) {
                auto json = res.json();
                success = json.contains("json") && json["json"]["async"] == true;
            }
            promise.set_value(success);
        }, post_data, "application/json");

        client.wait();

        REQUIRE(future.get() == true);
    }

    SECTION("Async request using coroutine pattern") {
        http::async_client client;
        client.timeout(10s);
        client.verify_ssl(false);

        std::atomic<bool> success{false};

        client.run([&client, &success, &base_url]() -> awaitable<void> {
            auto res = co_await client.get(base_url + "/get");
            if (res.ok() && res.status() == 200) {
                success = true;
            }
        });

        client.wait();

        REQUIRE(success == true);
    }
}

TEST_CASE("HTTPS Client with Request Builder", "[ssl][integration][builder]") {
    thinger::http::test::SSLTestServerFixture fixture;
    auto& base_url = fixture.base_url;

    SECTION("Request builder GET to HTTPS endpoint") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);

        auto response = client.request(base_url + "/headers")
            .header("X-Builder-Test", "value123")
            .get();

        REQUIRE(response.ok());
        REQUIRE(response.status() == 200);

        auto json = response.json();
        REQUIRE(json["headers"]["X-Builder-Test"] == "value123");
    }

    SECTION("Request builder POST to HTTPS endpoint") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);

        auto response = client.request(base_url + "/post")
            .header("Content-Type", "application/json")
            .body(R"({"builder": "test"})")
            .post();

        REQUIRE(response.ok());

        auto json = response.json();
        REQUIRE(json["json"]["builder"] == "test");
    }

    SECTION("Request builder with multiple headers") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);

        auto response = client.request(base_url + "/headers")
            .header("X-Header-One", "value1")
            .header("X-Header-Two", "value2")
            .header("X-Header-Three", "value3")
            .get();

        REQUIRE(response.ok());

        auto json = response.json();
        REQUIRE(json["headers"]["X-Header-One"] == "value1");
        REQUIRE(json["headers"]["X-Header-Two"] == "value2");
        REQUIRE(json["headers"]["X-Header-Three"] == "value3");
    }
}

TEST_CASE("HTTPS Client Response Properties", "[ssl][integration]") {
    thinger::http::test::SSLTestServerFixture fixture;
    auto& base_url = fixture.base_url;

    SECTION("Response contains expected headers") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);

        auto response = client.get(base_url + "/response-headers?X-Test-Header=hello");

        REQUIRE(response.ok());
        REQUIRE(response.header("X-Test-Header") == "hello");
    }

    SECTION("Response content-type header") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);

        auto response = client.get(base_url + "/json");

        REQUIRE(response.ok());
        std::string content_type = response.header("Content-Type");
        REQUIRE(content_type.find("application/json") != std::string::npos);
    }

    SECTION("Binary response handling") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);

        // Request binary data (PNG image)
        auto response = client.get(base_url + "/image/png");

        REQUIRE(response.ok());
        REQUIRE(!response.body().empty());
        // PNG magic bytes
        REQUIRE(static_cast<unsigned char>(response.body()[0]) == 0x89);
        REQUIRE(response.body()[1] == 'P');
        REQUIRE(response.body()[2] == 'N');
        REQUIRE(response.body()[3] == 'G');
    }
}

TEST_CASE("HTTPS Connection Reuse", "[ssl][integration]") {
    thinger::http::test::SSLTestServerFixture fixture;
    auto& base_url = fixture.base_url;

    SECTION("Multiple requests reuse SSL connection") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);

        // First request
        auto r1 = client.get(base_url + "/get");
        REQUIRE(r1.ok());
        size_t pool_size_after_first = client.pool_size();

        // Second request to same host should reuse connection
        auto r2 = client.get(base_url + "/headers");
        REQUIRE(r2.ok());
        size_t pool_size_after_second = client.pool_size();

        // Pool should have one connection (reused)
        REQUIRE(pool_size_after_second == pool_size_after_first);

        // Third request
        auto r3 = client.get(base_url + "/user-agent");
        REQUIRE(r3.ok());
    }

    SECTION("Clear connections") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);

        auto r1 = client.get(base_url + "/get");
        REQUIRE(r1.ok());
        REQUIRE(client.pool_size() > 0);

        client.clear_connections();
        REQUIRE(client.pool_size() == 0);
    }
}

TEST_CASE("HTTPS Server SSL Verification", "[ssl][integration]") {
    thinger::http::test::SSLTestServerFixture fixture;
    auto& base_url = fixture.base_url;

    SECTION("Server reports request is secure") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);

        auto response = client.get(base_url + "/get");

        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["secure"] == true);
    }

    SECTION("POST to secure server preserves security flag") {
        http::client client;
        client.timeout(10s);
        client.verify_ssl(false);

        http::headers_map headers;
        auto response = client.post(base_url + "/post", R"({"data":"test"})", "application/json", headers);

        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["secure"] == true);
    }
}

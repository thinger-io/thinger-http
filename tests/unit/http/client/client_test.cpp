#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/client/async_client.hpp>
#include <type_traits>

using namespace thinger;

// Helper trait to get the client type name for test naming
template<typename T>
struct client_type_name {
    static constexpr const char* value = "Unknown";
};

template<>
struct client_type_name<http::client> {
    static constexpr const char* value = "Standalone Client";
};

template<>
struct client_type_name<http::async_client> {
    static constexpr const char* value = "Async Client";
};

// Template test cases that work for both client types
TEMPLATE_TEST_CASE("HTTP Client construction and destruction", "[http][client][unit]",
                   http::client, http::async_client) {

    SECTION("Default construction") {
        TestType client;
        // Both clients should construct cleanly
        REQUIRE(true);
    }

    SECTION("Multiple instances can coexist") {
        TestType client1;
        TestType client2;
        TestType client3;

        // All clients should coexist
        REQUIRE(true);
    }

    SECTION("Destruction is clean") {
        {
            TestType client;
            // Client goes out of scope and should destruct cleanly
        }
        // If we get here without crashes, destruction worked
        REQUIRE(true);
    }
}

TEMPLATE_TEST_CASE("HTTP Client configuration", "[http][client][unit]",
                   http::client, http::async_client) {

    TestType client;

    SECTION("Timeout configuration with fluent API") {
        client.timeout(std::chrono::seconds(60));
        REQUIRE(client.get_timeout() == std::chrono::seconds(60));

        client.timeout(std::chrono::seconds(120));
        REQUIRE(client.get_timeout() == std::chrono::seconds(120));
    }

    SECTION("Redirect configuration with fluent API") {
        client.max_redirects(10);
        REQUIRE(client.get_max_redirects() == 10);

        client.follow_redirects(false);
        REQUIRE(client.get_follow_redirects() == false);

        client.follow_redirects(true);
        REQUIRE(client.get_follow_redirects() == true);
    }

    SECTION("User agent configuration with fluent API") {
        client.user_agent("TestAgent/1.0");
        REQUIRE(client.get_user_agent() == "TestAgent/1.0");

        client.user_agent("CustomAgent/2.0");
        REQUIRE(client.get_user_agent() == "CustomAgent/2.0");
    }

    SECTION("Other configuration options with fluent API") {
        client.auto_decompress(false);
        REQUIRE(client.get_auto_decompress() == false);

        client.auto_decompress(true);
        REQUIRE(client.get_auto_decompress() == true);

        client.verify_ssl(false);
        REQUIRE(client.get_verify_ssl() == false);

        client.verify_ssl(true);
        REQUIRE(client.get_verify_ssl() == true);
    }

    SECTION("Fluent API chaining") {
        client.timeout(std::chrono::seconds(30))
              .max_redirects(5)
              .follow_redirects(true)
              .user_agent("ChainedAgent/1.0")
              .auto_decompress(true)
              .verify_ssl(false);

        REQUIRE(client.get_timeout() == std::chrono::seconds(30));
        REQUIRE(client.get_max_redirects() == 5);
        REQUIRE(client.get_follow_redirects() == true);
        REQUIRE(client.get_user_agent() == "ChainedAgent/1.0");
        REQUIRE(client.get_auto_decompress() == true);
        REQUIRE(client.get_verify_ssl() == false);
    }
}

TEMPLATE_TEST_CASE("HTTP Request building", "[http][client][unit]",
                   http::client, http::async_client) {

    TestType client;

    SECTION("Create GET request") {
        auto req = client.create_request(http::method::GET, "http://example.com");
        REQUIRE(req != nullptr);
        REQUIRE(req->get_method() == http::method::GET);
        REQUIRE(req->get_url() == "http://example.com/");
    }

    SECTION("Create POST request") {
        auto req = client.create_request(http::method::POST, "http://example.com");
        REQUIRE(req != nullptr);
        req->set_content("test body", "application/json");
        REQUIRE(req->get_method() == http::method::POST);
        REQUIRE(req->get_body() == "test body");
        REQUIRE(req->get_header("Content-Type") == "application/json");
    }

    SECTION("Create PUT request") {
        auto req = client.create_request(http::method::PUT, "http://example.com");
        REQUIRE(req != nullptr);
        req->set_content("update body", "text/plain");
        REQUIRE(req->get_method() == http::method::PUT);
        REQUIRE(req->get_header("Content-Type") == "text/plain");
    }

    SECTION("Create DELETE request") {
        auto req = client.create_request(http::method::DELETE, "http://example.com");
        REQUIRE(req != nullptr);
        REQUIRE(req->get_method() == http::method::DELETE);
    }

    SECTION("Request with custom headers") {
        auto req = client.create_request(http::method::GET, "http://example.com");
        REQUIRE(req != nullptr);

        // Add custom headers
        req->add_header("Authorization", "Bearer token123");
        req->add_header("Custom-Header", "custom-value");
        req->add_header("X-Request-ID", "12345");

        // Verify all custom headers are present
        REQUIRE(req->has_header("Authorization"));
        REQUIRE(req->get_header("Authorization") == "Bearer token123");
        REQUIRE(req->has_header("Custom-Header"));
        REQUIRE(req->get_header("Custom-Header") == "custom-value");
        REQUIRE(req->has_header("X-Request-ID"));
        REQUIRE(req->get_header("X-Request-ID") == "12345");
    }

    SECTION("Multiple requests can be created") {
        auto req1 = client.create_request(http::method::GET, "http://example1.com");
        auto req2 = client.create_request(http::method::GET, "http://example2.com");
        auto req3 = client.create_request(http::method::POST, "http://example3.com");
        req3->set_content("{}", "application/json");

        REQUIRE(req1 != nullptr);
        REQUIRE(req2 != nullptr);
        REQUIRE(req3 != nullptr);

        // Each request should be independent
        REQUIRE(req1->get_url() == "http://example1.com/");
        REQUIRE(req2->get_url() == "http://example2.com/");
        REQUIRE(req3->get_url() == "http://example3.com/");
    }
}

// Tests specific to async_client
TEST_CASE("Async Client specific features", "[http][client][async][unit]") {

    SECTION("Service name") {
        http::async_client client;
        REQUIRE(client.get_service_name() == "http_async_client");
    }

    SECTION("Running state and pending requests") {
        http::async_client client;
        REQUIRE(client.is_running() == true);
        REQUIRE(client.pending_requests() == 0);
        REQUIRE(client.has_pending_requests() == false);
    }

    SECTION("Stop changes running state") {
        http::async_client client;
        REQUIRE(client.is_running() == true);

        client.stop();
        REQUIRE(client.is_running() == false);

        // Should be safe to stop multiple times
        client.stop();
        REQUIRE(client.is_running() == false);
    }

    SECTION("Worker auto-management") {
        // Save initial state
        bool auto_manage = asio::get_workers().is_auto_managed();
        size_t initial_clients = asio::get_workers().client_count();

        {
            http::async_client client;
            REQUIRE(client.is_running() == true);

            // Client should be registered
            REQUIRE(asio::get_workers().client_count() == initial_clients + 1);

            // If auto-manage is enabled and workers weren't running, they should start
            if (auto_manage && initial_clients == 0) {
                REQUIRE(asio::get_workers().running() == true);
            }
        }

        // After client destruction, count should return to initial
        REQUIRE(asio::get_workers().client_count() == initial_clients);
    }
}

// Tests specific to standalone client
TEST_CASE("Standalone Client specific features", "[http][client][standalone][unit]") {

    SECTION("Independent from workers") {
        // Save initial state
        bool initial_running = asio::get_workers().running();

        // workers should not be running initially
        REQUIRE(initial_running == false);

        // initial client count should be zero
        size_t initial_clients = asio::get_workers().client_count();
        REQUIRE(initial_clients == 0);

        {
            http::client client;

            // Creating a standalone client shouldn't register with workers
            REQUIRE(asio::get_workers().client_count() == initial_clients);

            // Workers state shouldn't change
            REQUIRE(asio::get_workers().running() == initial_running);
        }

        // After destruction, everything should remain the same
        REQUIRE(asio::get_workers().client_count() == initial_clients);
        REQUIRE(asio::get_workers().running() == initial_running);
    }

    SECTION("Synchronous methods return directly") {
        http::client client;

        // The new API returns responses directly without run()
        // Methods: get(), post(), put(), patch(), del(), head(), options()
        // Can't test without network, just verify it compiles
        REQUIRE(true);
    }

    SECTION("io_context is managed internally") {
        http::client client;

        // io_context is now private - the client manages it internally
        // The synchronous methods handle io_context.run() automatically
        REQUIRE(true);
    }
}

#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/util/logger.hpp>
#include <thinger/asio/workers.hpp>
#include <thread>
#include <chrono>
#include <atomic>
#include "../fixtures/server_listen.hpp"

using namespace thinger;

TEST_CASE("Basic HTTP Server Test", "[server]") {
    // Small delay between test sections to allow OS to release resources
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    SECTION("Server can start and stop") {
        // Create server instance
        http::server server;

        // Create a simple endpoint
        std::atomic<bool> endpoint_called(false);
        server.get("/test", [&endpoint_called](http::response& res) {
            endpoint_called = true;
            res.send("Hello from test!");
        });

        // Start server on OS-assigned port
        REQUIRE_FALSE(server.listen("127.0.0.1", 0));
        REQUIRE(http::test::listening_port(server) != 0);
    }
    
    SECTION("Server can handle multiple endpoints") {
        // Create server instance
        http::server server;

        // Add multiple endpoints
        server.get("/", [](http::response& res) {
            res.send("Root endpoint");
        });

        server.get("/api/test", [](http::response& res) {
            res.json({{"message", "Test API"}});
        });

        server.post("/api/echo", [](http::request& req, http::response& res) {
            res.json({{"echo", req.get_http_request()->get_body()}});
        });

        // Start server on OS-assigned port
        REQUIRE_FALSE(server.listen("127.0.0.1", 0));
    }
    
    SECTION("Server fails to start on invalid address") {
        // Create server instance
        http::server server;
        
        // Try to start on invalid IP address (one attempt, to fail fast in tests)
        asio::listener_options options;
        options.endpoint = asio::tcp_endpoint{"999.999.999.999", 8080};
        options.max_listening_attempts = 1;

        // Should fail due to invalid IP
        REQUIRE(server.listen(std::move(options)));
        REQUIRE_FALSE(server.is_listening());
    }
    
    SECTION("Server fails to start on port already in use") {
        // Create first server instance in its own scope
        {
            http::server server1;

            // Start first server on OS-assigned port
            REQUIRE_FALSE(server1.listen("127.0.0.1", 0));
            auto port = http::test::listening_port(server1);

            // Create second server instance in nested scope
            {
                http::server server2;

                // Try to start second server on same port (one attempt, to fail fast in
                // tests) - should fail
                asio::listener_options options;
                options.endpoint = asio::tcp_endpoint{"127.0.0.1", port};
                options.max_listening_attempts = 1;
                REQUIRE(server2.listen(std::move(options)));
                REQUIRE(server2.listening_on().empty());

                // server2 should be destroyed cleanly here even though it failed to start
            }

            // server1 is still running here
            REQUIRE(server1.is_listening());
        }
        // Both servers destroyed here
    }
    
    SECTION("Server cannot listen twice on the same endpoint") {
        // Create server instance
        http::server server;

        // Start server first time
        REQUIRE_FALSE(server.listen("127.0.0.1", 0));
        auto port = http::test::listening_port(server);

        // The same endpoint again fails, leaving the listener open as it is
        REQUIRE(server.listen("127.0.0.1", port) == boost::asio::error::address_in_use);
        REQUIRE(server.listening_on().size() == 1);

        // Port 0 is always a new endpoint: a second listener
        REQUIRE_FALSE(server.listen("127.0.0.1", 0));
        REQUIRE(server.listening_on().size() == 2);
    }
}
TEST_CASE("listening_on() is empty while the server is not listening", "[server]") {
    http::server server;
    REQUIRE(server.listening_on().empty());
    REQUIRE_FALSE(server.listen("127.0.0.1", 0));
    auto port = http::test::listening_port(server);
    REQUIRE(port != 0);
    REQUIRE(server.listening_on() == std::vector<std::string>{"http://127.0.0.1:" + std::to_string(port)});
    server.stop();
    REQUIRE(server.listening_on().empty());
}

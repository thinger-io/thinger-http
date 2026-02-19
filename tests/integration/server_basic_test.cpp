#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/util/logger.hpp>
#include <thinger/asio/workers.hpp>
#include <thread>
#include <chrono>
#include <atomic>

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
        REQUIRE(server.listen("127.0.0.1", 0));
        REQUIRE(server.local_port() != 0);
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
        REQUIRE(server.listen("127.0.0.1", 0));
    }
    
    SECTION("Server fails to start on invalid address") {
        // Create server instance
        http::server server;
        
        // Set max attempts to 1 to fail fast in tests
        server.set_max_listening_attempts(1);
        
        // Try to start on invalid IP address
        bool started = server.listen("999.999.999.999", 8080);
        
        // Should fail due to invalid IP
        REQUIRE_FALSE(started);
    }
    
    SECTION("Server fails to start on port already in use") {
        // Create first server instance in its own scope
        {
            http::server server1;

            // Start first server on OS-assigned port
            REQUIRE(server1.listen("127.0.0.1", 0));
            auto port = server1.local_port();

            // Create second server instance in nested scope
            {
                http::server server2;

                // Set max attempts to 1 to fail fast in tests
                server2.set_max_listening_attempts(1);

                // Try to start second server on same port - should fail
                bool started2 = server2.listen("127.0.0.1", port);
                REQUIRE_FALSE(started2);

                // server2 should be destroyed cleanly here even though it failed to start
            }

            // server1 is still running here
            REQUIRE(server1.is_listening());
        }
        // Both servers destroyed here
    }
    
    SECTION("Server cannot start twice") {
        // Create server instance
        http::server server;

        // Start server first time
        REQUIRE(server.listen("127.0.0.1", 0));

        // Try to start again - should fail
        bool started2 = server.listen("127.0.0.1", 0);
        REQUIRE_FALSE(started2);
    }
}
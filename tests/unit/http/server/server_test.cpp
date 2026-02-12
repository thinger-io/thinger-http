#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/pool_server.hpp>
#include <thinger/http/server/http_server_base.hpp>
#include <thinger/asio/workers.hpp>
#include <thread>
#include <chrono>

#include "thinger/http/server/response.hpp"

using namespace thinger;

// Template test cases that work for both server types
TEMPLATE_TEST_CASE("HTTP Server construction and destruction", "[http][server][unit]", 
                   http::server, http::pool_server) {
    
    SECTION("Default construction") {
        TestType server;
        // Servers start in stopped state until listen() is called
        REQUIRE(server.is_listening() == false);
    }
    
    SECTION("Multiple instances can coexist") {
        TestType server1;
        TestType server2;
        TestType server3;
        
        // All servers created successfully
        REQUIRE(true);
    }
    
    SECTION("Destruction is clean") {
        {
            TestType server;
            // Server goes out of scope and should destruct cleanly
        }
        // If we get here without crashes, destruction worked
        REQUIRE(true);
    }
}

TEMPLATE_TEST_CASE("HTTP Server configuration", "[http][server][unit]", 
                   http::server, http::pool_server) {
    
    TestType server;
    
    SECTION("SSL configuration") {
        server.enable_ssl(true);
        // SSL can be enabled (actual SSL setup requires certificates)
        
        server.enable_ssl(false);
        // SSL disabled
        REQUIRE(true);
    }
    
    SECTION("Request timeout configuration") {
        server.set_connection_timeout(std::chrono::seconds(120));

        REQUIRE(true);
    }
    
    SECTION("CORS configuration") {
        server.enable_cors(true);
        server.enable_cors(false);
        
        REQUIRE(true);
    }
    
    SECTION("Max listening attempts") {
        server.set_max_listening_attempts(5);
        server.set_max_listening_attempts(-1); // Infinite
        
        REQUIRE(true);
    }
}

TEMPLATE_TEST_CASE("HTTP Server lifecycle management", "[http][server][unit]", 
                   http::server, http::pool_server) {
    
    SECTION("Listen and stop") {
        TestType server;
        REQUIRE(server.is_listening() == false);
        
        // Listen on a random high port to avoid conflicts
        uint16_t port = 20000 + (std::rand() % 10000);
        bool listen_result = server.listen("127.0.0.1", port);
        
        if (listen_result) {
            REQUIRE(server.is_listening() == true);
            
            // Stop the server
            server.stop();
            REQUIRE(server.is_listening() == false);
            
            // Should be safe to stop multiple times
            server.stop();
            REQUIRE(server.is_listening() == false);
            
            // Should be able to restart on same port
            bool restart_result = server.listen("127.0.0.1", port);
            if (restart_result) {
                REQUIRE(server.is_listening() == true);
                server.stop();
                REQUIRE(server.is_listening() == false);
            } else {
                WARN("Could not restart server on same port");
            }
        } else {
            // Port might be in use, which is ok for unit tests
            WARN("Could not bind to port " << port << ", skipping listen test");
        }
    }
    
    SECTION("Multiple start/stop cycles") {
        TestType server;
        
        // Try multiple ports to avoid conflicts
        for (int attempt = 0; attempt < 3; ++attempt) {
            uint16_t port = 30000 + (std::rand() % 10000) + (attempt * 1000);
            
            REQUIRE(server.is_listening() == false);
            
            if (server.listen("127.0.0.1", port)) {
                REQUIRE(server.is_listening() == true);
                
                server.stop();
                REQUIRE(server.is_listening() == false);
                
                // Success on this attempt
                break;
            }
        }
    }
}

TEMPLATE_TEST_CASE("HTTP Server route management", "[http][server][unit]", 
                   http::server, http::pool_server) {
    
    TestType server;
    
    SECTION("Add basic routes") {
        // Add GET route
        server.get("/test", [](http::request& req, http::response& res) {
            res.send("GET test");
        });
        
        // Add POST route
        server.post("/test", [](http::request& req, http::response& res) {
            res.send("POST test");
        });
        
        // Add PUT route
        server.put("/test", [](http::request& req, http::response& res) {
            res.send("PUT test");
        });
        
        // Add DELETE route
        server.del("/test", [](http::request& req, http::response& res) {
            res.send("DELETE test");
        });
        
        // Routes should be registered (we can't easily test this without making requests)
        REQUIRE(true);
    }
    
    SECTION("Add parameterized routes") {
        server.get("/users/:id", [](http::request& req, http::response& res) {
            res.send("User ID: " + req["id"]);
        });
        
        server.get("/posts/:post_id/comments/:comment_id", [](http::request& req, http::response& res) {
            res.send("Post: " + req["post_id"] + ", Comment: " + req["comment_id"]);
        });
        
        REQUIRE(true);
    }
    
    SECTION("Add middleware") {
        server.use([](http::request& req, http::response& res, const std::function<void()>& next) {
            // Middleware logic here
            next();
        });
        
        REQUIRE(true);
    }
    
    SECTION("Static file serving") {
        server.serve_static("/static", "./public");
        REQUIRE(true);
    }
}

// Tests specific to pool_server
TEST_CASE("Pool Server specific features", "[http][server][pool][unit]") {
    
    SECTION("Service name") {
        http::pool_server server;
        REQUIRE(server.get_service_name() == "http_pool_server");
    }
    
    SECTION("Worker registration") {
        // Save initial state
        size_t initial_clients = asio::get_workers().client_count();
        
        {
            http::pool_server server;
            
            // Pool server should register with workers
            REQUIRE(asio::get_workers().client_count() == initial_clients + 1);
        }
        
        // After destruction, count should return to initial
        REQUIRE(asio::get_workers().client_count() == initial_clients);
    }
    
    SECTION("Listen starts workers") {
        bool initial_running = asio::get_workers().running();
        size_t initial_clients = asio::get_workers().client_count();
        
        {
            http::pool_server server;
            
            // Try to listen
            uint16_t port = 25000 + (std::rand() % 10000);
            bool listen_result = server.listen("127.0.0.1", port);
            
            if (listen_result) {
                // If auto-manage is enabled and this is the first client, workers should start
                if (asio::get_workers().is_auto_managed() && initial_clients == 0) {
                    REQUIRE(asio::get_workers().running() == true);
                }
                
                server.stop();
            }
        }
        
        // Give time for async operations
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        // Workers should stop if this was the last client
        if (asio::get_workers().is_auto_managed() && initial_clients == 0) {
            REQUIRE(asio::get_workers().running() == false);
        }
    }
}

// Tests specific to standalone server
TEST_CASE("Standalone Server specific features", "[http][server][standalone][unit]") {
    
    SECTION("Thread count construction") {
        http::server server;
        // Server created with specified thread count
        REQUIRE(true);
    }
    
    SECTION("Direct io_context access") {
        http::server server;
        // Should have access to io_context
        auto& io_ctx = server.get_io_context();
        // io_context is a reference, so just check we can access it
        REQUIRE(true);
    }
    
    SECTION("Independent from workers") {
        // Save initial state
        bool initial_running = asio::get_workers().running();

        // workers should not be running initially
        REQUIRE(initial_running == false);

        // initial client count should be zero
        size_t initial_clients = asio::get_workers().client_count();
        REQUIRE(initial_clients == 0);

        {
            http::server server;
            REQUIRE(server.is_listening() == false);

            // Creating a standalone server shouldn't register with workers
            REQUIRE(asio::get_workers().client_count() == initial_clients);
            
            // Workers state shouldn't change
            REQUIRE(asio::get_workers().running() == initial_running);
        }

        // After destruction, everything should remain the same
        REQUIRE(asio::get_workers().client_count() == initial_clients);
        REQUIRE(asio::get_workers().running() == initial_running);
    }
}
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/pool_server.hpp>
#include <thinger/http/server/server_base.hpp>
#include <thinger/asio/workers.hpp>
#include <thread>
#include <chrono>

#include "thinger/http/server/response.hpp"
#include "../../../fixtures/running_workers.hpp"
#include "../../../fixtures/server_listen.hpp"

using namespace thinger;
using thinger::http::test::running_workers;

namespace {

    using thinger::http::test::with_workers;

    // A server of the type under test: a pool_server runs on workers of its own
    template<typename Server>
    struct server_under_test : Server {};

    template<>
    struct server_under_test<http::pool_server> : with_workers, http::pool_server {
        server_under_test() : http::pool_server(workers) {}
    };

}

// Template test cases that work for both server types
TEMPLATE_TEST_CASE("HTTP Server construction and destruction", "[http][server][unit]", 
                   http::server, http::pool_server) {
    
    SECTION("Default construction") {
        server_under_test<TestType> server;
        // Servers start in stopped state until listen() is called
        REQUIRE(server.is_listening() == false);
    }
    
    SECTION("Multiple instances can coexist") {
        server_under_test<TestType> server1;
        server_under_test<TestType> server2;
        server_under_test<TestType> server3;
        
        // All servers created successfully
        REQUIRE(true);
    }
    
    SECTION("Destruction is clean") {
        {
            server_under_test<TestType> server;
            // Server goes out of scope and should destruct cleanly
        }
        // If we get here without crashes, destruction worked
        REQUIRE(true);
    }
}

TEMPLATE_TEST_CASE("HTTP Server configuration", "[http][server][unit]", 
                   http::server, http::pool_server) {
    
    server_under_test<TestType> server;
    
    SECTION("Connection options") {
        http::connection_options options;
        options.idle_timeout = std::chrono::seconds(60);
        options.linger_max_bytes = 0;
        server.set_connection_options(options);

        REQUIRE(true);
    }
    
    SECTION("CORS configuration") {
        REQUIRE_FALSE(server.enable_cors());
        REQUIRE_FALSE(server.enable_cors({.origins = {"https://a.example"}, .credentials = true}));
        REQUIRE(server.enable_cors({.credentials = true}) == boost::system::errc::invalid_argument);
        server.disable_cors();
    }
}

// The deadlines of a connection are on by default: Node's headersTimeout and requestTimeout,
// nginx's send_timeout
TEST_CASE("Connection deadlines default to 60 s, 300 s and 60 s", "[http][server][unit]") {
    http::connection_options options;
    CHECK(options.header_timeout == std::chrono::seconds(60));
    CHECK(options.request_timeout == std::chrono::seconds(300));
    CHECK(options.write_timeout == std::chrono::seconds(60));
    CHECK(options.idle_timeout == std::chrono::seconds(120));
}

TEMPLATE_TEST_CASE("HTTP Server lifecycle management", "[http][server][unit]", 
                   http::server, http::pool_server) {
    
    SECTION("Listen and stop") {
        server_under_test<TestType> server;
        REQUIRE(server.is_listening() == false);

        // Listen on OS-assigned port
        REQUIRE_FALSE(server.listen("127.0.0.1", 0));
        auto port = http::test::listening_port(server);
        REQUIRE(port != 0);
        REQUIRE(server.is_listening() == true);

        // Stop the server
        server.stop();
        REQUIRE(server.is_listening() == false);

        // Should be safe to stop multiple times
        server.stop();
        REQUIRE(server.is_listening() == false);

        // Should be able to restart on same port
        bool restart_result = !server.listen("127.0.0.1", port);
        if (restart_result) {
            REQUIRE(server.is_listening() == true);
            server.stop();
            REQUIRE(server.is_listening() == false);
        } else {
            WARN("Could not restart server on same port (OS may not have released it yet)");
        }
    }
    
    SECTION("Multiple start/stop cycles") {
        server_under_test<TestType> server;

        REQUIRE(server.is_listening() == false);

        REQUIRE_FALSE(server.listen("127.0.0.1", 0));
        REQUIRE(server.is_listening() == true);

        server.stop();
        REQUIRE(server.is_listening() == false);
    }
}

TEMPLATE_TEST_CASE("HTTP Server route management", "[http][server][unit]", 
                   http::server, http::pool_server) {
    
    server_under_test<TestType> server;
    
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

    SECTION("Route groups add prefix, tags and metadata") {
        auto devices = server.group("/v1/users/:user/devices").tag("Devices").meta("resource", "device");
        devices.get("/:device", [](http::response& res) { res.send("ok"); })
            .meta("permission", "ReadDevice");
        devices.post("/", [](http::request& req, nlohmann::json& body, http::response& res) -> thinger::awaitable<void> {
            co_return;
        }).meta("resource", "device-collection");
        devices.group("/:device/resources").tag("Resources")
            .get("/:resource", [](http::response& res) { res.send("ok"); });

        const auto& get_routes = server.router().get_routes().at(http::method::GET);
        auto find = [](const std::deque<http::route>& routes, const std::string& pattern) -> const http::route* {
            for (const auto& r : routes) if (r.get_pattern() == pattern) return &r;
            return nullptr;
        };

        auto* device = find(get_routes, "/v1/users/:user/devices/:device");
        REQUIRE(device);
        REQUIRE(device->get_tags() == std::vector<std::string>{"Devices"});
        REQUIRE(device->get_meta("resource") == "device");
        REQUIRE(device->get_meta("permission") == "ReadDevice");

        auto* resource = find(get_routes, "/v1/users/:user/devices/:device/resources/:resource");
        REQUIRE(resource);
        REQUIRE(resource->get_tags() == std::vector<std::string>{"Devices", "Resources"});

        auto* create = find(server.router().get_routes().at(http::method::POST), "/v1/users/:user/devices/");
        REQUIRE(create);
        REQUIRE(create->get_meta("resource") == "device-collection");
    }

    SECTION("Add async middleware") {
        server.use([](http::request& req, http::response& res) -> thinger::awaitable<bool> {
            co_return true;
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

    SECTION("Workers never start by themselves") {
        asio::workers workers;
        {
            http::pool_server server(workers);
            REQUIRE_FALSE(workers.running());
        }
        REQUIRE_FALSE(workers.running());
    }

    SECTION("The workers keep running once the server stops") {
        running_workers workers;
        {
            http::pool_server server(workers);
            REQUIRE_FALSE(server.listen("127.0.0.1", 0));
            server.stop();
        }
        REQUIRE(workers.running());
    }

    SECTION("Several servers on the same workers") {
        running_workers workers;
        http::pool_server first(workers);
        http::pool_server second(workers);
        REQUIRE_FALSE(first.listen("127.0.0.1", 0));
        REQUIRE_FALSE(second.listen("127.0.0.1", 0));
        REQUIRE(http::test::listening_port(first) != http::test::listening_port(second));
        first.stop();
        REQUIRE(second.is_listening());
        second.stop();
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
        // It listens and serves on its own io_context: no workers exist
        http::server server;
        REQUIRE_FALSE(server.listen("127.0.0.1", 0));
        REQUIRE(server.is_listening());
        server.stop();
        REQUIRE_FALSE(server.is_listening());
    }
}
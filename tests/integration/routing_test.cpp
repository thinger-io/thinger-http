#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/client/client.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <future>

using namespace thinger;
using namespace std::chrono_literals;

// Fixture: starts server after routes are configured
struct RoutingTestFixture {
    http::server server;
    uint16_t port = 0;
    std::string base_url;
    std::thread server_thread;
    bool server_started = false;

    RoutingTestFixture() = default;

    void start_server() {
        if (server_started) return;
        REQUIRE(server.listen("0.0.0.0", 0));
        port = server.local_port();
        base_url = "http://localhost:" + std::to_string(port);
        server_started = true;

        std::promise<void> ready;
        server_thread = std::thread([this, &ready]() {
            ready.set_value();
            server.wait();
        });
        ready.get_future().wait();
    }

    ~RoutingTestFixture() {
        if (server_started) {
            server.stop();
            if (server_thread.joinable()) server_thread.join();
        }
    }
};

// ============================================================================
// Custom regex route matching via HTTP
// ============================================================================

TEST_CASE("Routing: custom regex parameter via HTTP",
          "[routing][regex][integration]") {
    RoutingTestFixture fixture;
    auto& server = fixture.server;

    // Route with numeric-only custom regex
    server.get("/users/:id([0-9]+)", [](http::request& req, http::response& res) {
        res.json({{"user_id", req["id"]}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Numeric ID matches") {
        auto response = client.get(fixture.base_url + "/users/42");
        REQUIRE(response.ok());
        REQUIRE(response.json()["user_id"] == "42");
    }

    SECTION("Non-numeric ID returns 404") {
        auto response = client.get(fixture.base_url + "/users/alice");
        REQUIRE(response.status() == 404);
    }
}

TEST_CASE("Routing: multiple custom regex parameters via HTTP",
          "[routing][regex][integration]") {
    RoutingTestFixture fixture;
    auto& server = fixture.server;

    server.get("/api/:version([0-9]+)/:resource([a-z]+)",
               [](http::request& req, http::response& res) {
        res.json({{"version", req["version"]}, {"resource", req["resource"]}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Both params match") {
        auto response = client.get(fixture.base_url + "/api/2/users");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["version"] == "2");
        REQUIRE(json["resource"] == "users");
    }

    SECTION("Non-matching version returns 404") {
        auto response = client.get(fixture.base_url + "/api/v2/users");
        REQUIRE(response.status() == 404);
    }
}

// ============================================================================
// Request + JSON response callback via HTTP
// ============================================================================

TEST_CASE("Routing: request+json response callback via HTTP",
          "[routing][callback][integration]") {
    RoutingTestFixture fixture;
    auto& server = fixture.server;

    server.put("/items/:id", [](http::request& req, nlohmann::json& json, http::response& res) {
        json["updated_id"] = req["id"];
        json["callback_type"] = "request_json_response";
        res.json(json);
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("PUT with JSON body triggers request+json callback") {
        auto request = std::make_shared<http::http_request>();
        request->set_method(http::method::PUT);
        request->set_url(fixture.base_url + "/items/99");
        request->set_content(R"({"name":"widget"})", "application/json");
        auto response = client.send(request);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["callback_type"] == "request_json_response");
        REQUIRE(json["updated_id"] == "99");
        REQUIRE(json["name"] == "widget");
    }

    SECTION("PUT with empty body gets empty json") {
        auto request = std::make_shared<http::http_request>();
        request->set_method(http::method::PUT);
        request->set_url(fixture.base_url + "/items/1");
        request->set_content("", "application/json");
        auto response = client.send(request);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["callback_type"] == "request_json_response");
        REQUIRE(json["updated_id"] == "1");
    }

    SECTION("PUT with invalid JSON returns 400") {
        auto request = std::make_shared<http::http_request>();
        request->set_method(http::method::PUT);
        request->set_url(fixture.base_url + "/items/1");
        request->set_content("{invalid}", "application/json");
        auto response = client.send(request);
        REQUIRE(response.status() == 400);
    }
}

// ============================================================================
// Not found handler invocation
// ============================================================================

TEST_CASE("Routing: not found handler is invoked",
          "[routing][notfound][integration]") {
    RoutingTestFixture fixture;
    auto& server = fixture.server;

    server.get("/exists", [](http::response& res) {
        res.json({{"found", true}});
    });

    server.set_not_found_handler([](http::request& req, http::response& res) {
        res.json({{"error", "custom_not_found"}, {"path", req.get_http_request()->get_path()}},
                 http::http_response::status::not_found);
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Existing route returns 200") {
        auto response = client.get(fixture.base_url + "/exists");
        REQUIRE(response.ok());
    }

    SECTION("Non-existent route triggers custom not found handler") {
        auto response = client.get(fixture.base_url + "/nope");
        REQUIRE(response.status() == 404);
        auto json = response.json();
        REQUIRE(json["error"] == "custom_not_found");
        REQUIRE(json["path"] == "/nope");
    }
}

// ============================================================================
// 404 vs 405 error distinction
// ============================================================================

TEST_CASE("Routing: 404 for no matching path",
          "[routing][errors][integration]") {
    RoutingTestFixture fixture;
    auto& server = fixture.server;

    server.get("/only-get", [](http::response& res) {
        res.json({{"ok", true}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("GET to existing route succeeds") {
        auto response = client.get(fixture.base_url + "/only-get");
        REQUIRE(response.ok());
    }

    SECTION("GET to non-existent path returns 404") {
        auto response = client.get(fixture.base_url + "/does-not-exist");
        REQUIRE(response.status() == 404);
    }
}

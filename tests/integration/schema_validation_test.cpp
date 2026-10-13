#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/client/client.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <thread>

#include "server_test_helpers.hpp"

using namespace thinger;

struct SchemaTestFixture {
    http::server server;
    // Set by the server thread once listening, before the test reads base_url
    std::atomic<uint16_t> port{0};
    std::string base_url;
    std::thread server_thread;

    SchemaTestFixture() {
        setup_endpoints();
        start_server();
    }

    ~SchemaTestFixture() {
        server.stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

private:
    void setup_endpoints() {
        // POST with schema validation (json + response callback)
        server.post("/api/users", [](nlohmann::json& json, http::response& res) {
            res.json({{"created", true}, {"name", json["name"]}});
        }).schema({
            {"type", "object"},
            {"required", {"name", "email"}},
            {"properties", {
                {"name", {{"type", "string"}}},
                {"email", {{"type", "string"}}},
                {"age", {{"type", "integer"}, {"minimum", 0}}}
            }}
        });

        // POST with schema validation (request + json + response callback)
        server.post("/api/items", [](http::request& req, nlohmann::json& json, http::response& res) {
            res.json({{"item", json["title"]}, {"method", "POST"}});
        }).schema({
            {"type", "object"},
            {"required", {"title"}},
            {"properties", {
                {"title", {{"type", "string"}}},
                {"count", {{"type", "integer"}}}
            }}
        });

        // POST without schema (no validation)
        server.post("/api/raw", [](nlohmann::json& json, http::response& res) {
            res.json({{"received", true}});
        });

        // PUT with schema
        server.put("/api/users/:id", [](nlohmann::json& json, http::response& res) {
            res.json({{"updated", true}});
        }).schema({
            {"type", "object"},
            {"properties", {
                {"name", {{"type", "string"}}}
            }}
        });

        // PATCH with schema
        server.patch("/api/users/:id", [](nlohmann::json& json, http::response& res) {
            res.json({{"patched", true}});
        }).schema({
            {"type", "object"},
            {"properties", {
                {"name", {{"type", "string"}}},
                {"email", {{"type", "string"}}}
            }}
        });

        // Schema with enum validation
        server.post("/api/status", [](nlohmann::json& json, http::response& res) {
            res.json({{"status", json["status"]}});
        }).schema({
            {"type", "object"},
            {"required", {"status"}},
            {"properties", {
                {"status", {{"type", "string"}, {"enum", {"active", "inactive", "pending"}}}}
            }}
        });

        // Schema with nested object
        server.post("/api/address", [](nlohmann::json& json, http::response& res) {
            res.json({{"city", json["address"]["city"]}});
        }).schema({
            {"type", "object"},
            {"required", {"address"}},
            {"properties", {
                {"address", {
                    {"type", "object"},
                    {"required", {"city", "country"}},
                    {"properties", {
                        {"city", {{"type", "string"}}},
                        {"country", {{"type", "string"}}}
                    }}
                }}
            }}
        });
    }

    void start_server() {
        server_thread = std::thread([this]() {
            server.start("127.0.0.1", 0, [this]() {
                auto listening = server.local_port();
                base_url = "http://127.0.0.1:" + std::to_string(listening);
                port.store(listening, std::memory_order_release);
            });
        });
        while (port.load(std::memory_order_acquire) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
};

TEST_CASE("Schema validation - valid request", "[server][schema]") {
    SchemaTestFixture fixture;
    http::client client;

    auto res = client.post(fixture.base_url + "/api/users",
        R"({"name":"Alice","email":"alice@example.com"})", "application/json");
    REQUIRE(res.ok());
    auto json = res.json();
    REQUIRE(json["created"] == true);
    REQUIRE(json["name"] == "Alice");
}

TEST_CASE("Schema validation - valid with optional field", "[server][schema]") {
    SchemaTestFixture fixture;
    http::client client;

    auto res = client.post(fixture.base_url + "/api/users",
        R"({"name":"Bob","email":"bob@test.com","age":30})", "application/json");
    REQUIRE(res.ok());
    REQUIRE(res.json()["name"] == "Bob");
}

TEST_CASE("Schema validation - missing required field", "[server][schema]") {
    SchemaTestFixture fixture;
    http::client client;

    auto res = client.post(fixture.base_url + "/api/users",
        R"({"name":"Alice"})", "application/json");  // missing "email"
    REQUIRE(res.status_code() == 400);
    auto json = res.json();
    REQUIRE(json.contains("error"));
    REQUIRE(json["error"].contains("message"));
}

TEST_CASE("Schema validation - wrong type", "[server][schema]") {
    SchemaTestFixture fixture;
    http::client client;

    auto res = client.post(fixture.base_url + "/api/users",
        R"({"name":123,"email":"test@test.com"})", "application/json");  // name should be string
    REQUIRE(res.status_code() == 400);
    REQUIRE(res.json().contains("error"));
}

TEST_CASE("Schema validation - no schema means no validation", "[server][schema]") {
    SchemaTestFixture fixture;
    http::client client;

    auto res = client.post(fixture.base_url + "/api/raw",
        R"({"anything":"goes"})", "application/json");
    REQUIRE(res.ok());
    REQUIRE(res.json()["received"] == true);
}

TEST_CASE("Schema validation - request+json+response callback", "[server][schema]") {
    SchemaTestFixture fixture;
    http::client client;

    SECTION("valid") {
        auto res = client.post(fixture.base_url + "/api/items",
            R"({"title":"Widget"})", "application/json");
        REQUIRE(res.ok());
        REQUIRE(res.json()["item"] == "Widget");
    }

    SECTION("missing required") {
        auto res = client.post(fixture.base_url + "/api/items",
            R"({"count":5})", "application/json");  // missing "title"
        REQUIRE(res.status_code() == 400);
    }
}

TEST_CASE("Schema validation - empty body with required fields", "[server][schema]") {
    SchemaTestFixture fixture;
    http::client client;

    auto res = client.post(fixture.base_url + "/api/users", "", "application/json");
    REQUIRE(res.status_code() == 400);
}

TEST_CASE("Schema validation - enum constraint", "[server][schema]") {
    SchemaTestFixture fixture;
    http::client client;

    SECTION("valid enum value") {
        auto res = client.post(fixture.base_url + "/api/status",
            R"({"status":"active"})", "application/json");
        REQUIRE(res.ok());
        REQUIRE(res.json()["status"] == "active");
    }

    SECTION("invalid enum value") {
        auto res = client.post(fixture.base_url + "/api/status",
            R"({"status":"deleted"})", "application/json");
        REQUIRE(res.status_code() == 400);
    }
}

TEST_CASE("Schema validation - nested object", "[server][schema]") {
    SchemaTestFixture fixture;
    http::client client;

    SECTION("valid nested") {
        auto res = client.post(fixture.base_url + "/api/address",
            R"({"address":{"city":"Madrid","country":"Spain"}})", "application/json");
        REQUIRE(res.ok());
        REQUIRE(res.json()["city"] == "Madrid");
    }

    SECTION("missing nested required") {
        auto res = client.post(fixture.base_url + "/api/address",
            R"({"address":{"city":"Madrid"}})", "application/json");  // missing country
        REQUIRE(res.status_code() == 400);
    }

    SECTION("wrong nested type") {
        auto res = client.post(fixture.base_url + "/api/address",
            R"({"address":"not an object"})", "application/json");
        REQUIRE(res.status_code() == 400);
    }
}

TEST_CASE("Schema validation - invalid JSON still returns 400", "[server][schema]") {
    SchemaTestFixture fixture;
    http::client client;

    auto res = client.post(fixture.base_url + "/api/users",
        "not json at all", "application/json");
    REQUIRE(res.status_code() == 400);
}

TEST_CASE("Schema validation - PUT with schema", "[server][schema]") {
    SchemaTestFixture fixture;
    http::client client;

    SECTION("valid") {
        auto res = client.put(fixture.base_url + "/api/users/123",
            R"({"name":"Updated"})", "application/json");
        REQUIRE(res.ok());
        REQUIRE(res.json()["updated"] == true);
    }

    SECTION("wrong type") {
        auto res = client.put(fixture.base_url + "/api/users/123",
            R"({"name":456})", "application/json");
        REQUIRE(res.status_code() == 400);
    }
}

TEST_CASE("Schema validation - PATCH with schema", "[server][schema]") {
    SchemaTestFixture fixture;
    http::client client;

    SECTION("valid") {
        auto res = client.patch(fixture.base_url + "/api/users/123",
            R"({"name":"Patched"})", "application/json");
        REQUIRE(res.ok());
        REQUIRE(res.json()["patched"] == true);
    }

    SECTION("wrong type") {
        auto res = client.patch(fixture.base_url + "/api/users/123",
            R"({"name":789})", "application/json");
        REQUIRE(res.status_code() == 400);
    }
}

TEST_CASE("Schema validation - callbacks not taking the JSON body", "[server][schema]") {
    // .schema() validates the body whatever the signature of the callback: those reading it
    // from the request get it read and validated first, with the same answers
    const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name"}},
        {"properties", {{"name", {{"type", "string"}}}}}
    };
    std::atomic<int> calls{0};
    http::server server;
    server.post("/sync", [&calls](http::request& req, http::response& res) {
        ++calls;
        res.json({{"name", nlohmann::json::parse(req.body())["name"]}});
    }).schema(schema);
    server.post("/sync-response", [&calls](http::response& res) {
        ++calls;
        res.json({{"called", true}});
    }).schema(schema);
    server.post("/coroutine", [&calls](http::request& req, http::response& res) -> thinger::awaitable<void> {
        ++calls;
        // already read: read_body() has nothing left to read
        co_await req.read_body();
        res.json({{"name", nlohmann::json::parse(req.body())["name"]}});
    }).schema(schema);
    server.post("/coroutine-response", [&calls](http::response& res) -> thinger::awaitable<void> {
        ++calls;
        res.json({{"called", true}});
        co_return;
    }).schema(schema);
    server.post("/json", [](nlohmann::json& body, http::response& res) { res.json(body); }).schema(schema);
    server_test::running_server running(server);
    auto base = "http://127.0.0.1:" + std::to_string(running.port);
    http::client client;

    // The answer of the route taking the JSON body, for the same body
    auto invalid = client.post(base + "/json", R"({"name":42})", "application/json");
    REQUIRE(invalid.status() == 400);

    for (std::string path : {"/sync", "/sync-response", "/coroutine", "/coroutine-response"}) {
        DYNAMIC_SECTION(path) {
            auto valid = client.post(base + path, R"({"name":"Alice"})", "application/json");
            REQUIRE(valid.status() == 200);
            if (path == "/sync" || path == "/coroutine") REQUIRE(valid.json()["name"] == "Alice");
            REQUIRE(calls == 1);

            auto wrong_type = client.post(base + path, R"({"name":42})", "application/json");
            REQUIRE(wrong_type.status() == 400);
            REQUIRE(wrong_type.body() == invalid.body());

            REQUIRE(client.post(base + path, R"({})", "application/json").status() == 400);
            REQUIRE(client.post(base + path, R"({"name":)", "application/json").status() == 400);
            REQUIRE(client.post(base + path, "name=Alice", "text/plain").status() == 415);
            REQUIRE(calls == 1);
        }
    }
}

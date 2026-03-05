#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/client/client.hpp>
#include <nlohmann/json.hpp>
#include <thread>

using namespace thinger;

struct SchemaTestFixture {
    http::server server;
    uint16_t port = 0;
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
                port = server.local_port();
                base_url = "http://127.0.0.1:" + std::to_string(port);
            });
        });
        while (port == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
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

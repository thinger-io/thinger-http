#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/openapi.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>

using namespace thinger;
using nlohmann::json;

TEST_CASE("OpenAPI path conversion", "[openapi][unit]") {
    using http::openapi_generator;
    REQUIRE(openapi_generator::to_openapi_path("/users") == "/users");
    REQUIRE(openapi_generator::to_openapi_path("/users/:user/devices/:device") == "/users/{user}/devices/{device}");
    REQUIRE(openapi_generator::to_openapi_path("/items/:id([0-9]+)/tags/:tag") == "/items/{id}/tags/{tag}");
    REQUIRE(openapi_generator::to_openapi_path("/files/:path(.+)") == "/files/{path}");
}

TEST_CASE("OpenAPI document from documented routes", "[openapi][unit]") {
    http::server server;
    server.schema_component("Device", {
        {"type", "object"},
        {"required", {"name"}},
        {"properties", {{"name", {{"type", "string"}}}}}
    });

    server.openapi().title("Test API").version("2.0.0").description("For tests").server("https://api.example.com", "Production");

    auto devices = server.group("/v1/users/:user/devices").tag("Devices");

    devices.get("/:device([a-z0-9_-]{1,32})", [](http::request& req, http::response& res) { res.send("ok"); })
        .summary("Get device")
        .description("Returns a device")
        .operation_id("getDevice")
        .path_param("device", "Device id")
        .query_param("fields", "Fields to return", {{"type", "string"}}, false)
        .returns(200, "The device", {{"$ref", "#/components/schemas/Device"}}, {{"name", "sensor"}})
        .returns(404, "Not found")
        .meta("permission", "Device:Read");

    devices.post("/", [](http::request& req, json& body, http::response& res) -> thinger::awaitable<void> { co_return; })
        .summary("Create device")
        .schema({{"$ref", "#/components/schemas/Device"}})
        .example({{"name", "a"}})
        .example({{"name", "b"}})
        .deprecated();

    // JSON body without a schema, and an undocumented route
    server.put("/settings", [](json& body, http::response& res) -> thinger::awaitable<void> { co_return; });
    server.get("/plain/:id", [](http::response& res) { res.send("ok"); });

    // Left out of the document
    server.get("/internal", [](http::response& res) { res.send("ok"); }).hidden();
    server.serve_static("/static", ".");
    server.serve_openapi();
    server.enable_cors();

    auto doc = server.openapi().generate();

    REQUIRE(doc["openapi"] == "3.1.0");
    REQUIRE(doc["info"] == json{{"title", "Test API"}, {"version", "2.0.0"}, {"description", "For tests"}});
    REQUIRE(doc["servers"] == json::array({{{"url", "https://api.example.com"}, {"description", "Production"}}}));
    REQUIRE(doc["tags"] == json::array({{{"name", "Devices"}}}));
    REQUIRE(doc["components"]["schemas"]["Device"]["required"] == json::array({"name"}));

    SECTION("Fully documented operation") {
        json expected = {
            {"summary", "Get device"},
            {"description", "Returns a device"},
            {"tags", {"Devices"}},
            {"operationId", "getDevice"},
            {"parameters", {
                {{"name", "user"}, {"in", "path"}, {"required", true}, {"schema", {{"type", "string"}}}},
                {{"name", "device"}, {"in", "path"}, {"required", true}, {"description", "Device id"},
                 {"schema", {{"type", "string"}, {"pattern", "^(?:[a-z0-9_-]{1,32})$"}}}},
                {{"name", "fields"}, {"in", "query"}, {"required", false}, {"description", "Fields to return"},
                 {"schema", {{"type", "string"}}}}
            }},
            {"responses", {
                {"200", {{"description", "The device"}, {"content", {{"application/json", {
                    {"schema", {{"$ref", "#/components/schemas/Device"}}},
                    {"example", {{"name", "sensor"}}}
                }}}}}},
                {"404", {{"description", "Not found"}}}
            }}
        };
        REQUIRE(doc["paths"]["/v1/users/{user}/devices/{device}"]["get"] == expected);
    }

    SECTION("Request body with schema and several examples") {
        auto& op = doc["paths"]["/v1/users/{user}/devices/"]["post"];
        REQUIRE(op["deprecated"] == true);
        REQUIRE(op["requestBody"]["required"] == true);
        auto& media = op["requestBody"]["content"]["application/json"];
        REQUIRE(media["schema"] == json{{"$ref", "#/components/schemas/Device"}});
        REQUIRE(media["examples"]["example1"]["value"]["name"] == "a");
        REQUIRE(media["examples"]["example2"]["value"]["name"] == "b");
    }

    SECTION("JSON body handler without schema and undocumented route") {
        REQUIRE(doc["paths"]["/settings"]["put"]["requestBody"]["content"]["application/json"]["schema"] == json::object());
        auto& plain = doc["paths"]["/plain/{id}"]["get"];
        REQUIRE(plain["parameters"][0]["name"] == "id");
        REQUIRE(plain["responses"]["200"]["description"] == "Successful response");
        REQUIRE_FALSE(plain.contains("requestBody"));
    }

    SECTION("Hidden, static, OpenAPI and CORS routes are left out") {
        REQUIRE_FALSE(doc["paths"].contains("/internal"));
        REQUIRE_FALSE(doc["paths"].contains("/openapi.json"));
        REQUIRE_FALSE(doc["paths"].contains("/static/{path}"));
        for (const auto& [path, item] : doc["paths"].items()) {
            REQUIRE_FALSE(item.contains("options"));
        }
    }
}

TEST_CASE("OpenAPI hooks", "[openapi][unit]") {
    http::server server;
    server.get("/devices/:id", [](http::response& res) { res.send("ok"); }).meta("permission", "Device:Read");
    server.get("/public", [](http::response& res) { res.send("ok"); });

    server.openapi()
        .on_operation([](const http::route& r, const std::string& method, json& operation) {
            if (r.has_meta("permission")) {
                operation["x-permission"] = r.get_meta("permission");
                operation["security"] = json::array({{{"bearer", json::array()}}});
            }
        })
        .on_document([](json& doc) {
            doc["components"]["securitySchemes"]["bearer"] = {{"type", "http"}, {"scheme", "bearer"}};
        });

    auto doc = server.openapi().generate();
    REQUIRE(doc["paths"]["/devices/{id}"]["get"]["x-permission"] == "Device:Read");
    REQUIRE(doc["paths"]["/devices/{id}"]["get"]["security"][0].contains("bearer"));
    REQUIRE_FALSE(doc["paths"]["/public"]["get"].contains("x-permission"));
    REQUIRE(doc["components"]["securitySchemes"]["bearer"]["scheme"] == "bearer");
}

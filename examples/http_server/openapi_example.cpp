// OpenAPI example
//
// The OpenAPI document is generated from the routes: paths and parameters from the
// patterns, request bodies from the validation schemas, and the rest from the route
// documentation. A hook turns the application's route metadata (the permission each
// route requires) into an x-permission extension and a security requirement.
//
//   http://localhost:8091/openapi.json   OpenAPI 3.1 document
//   http://localhost:8091/docs           Swagger UI

#include <thinger/http_server.hpp>
#include <iostream>

using namespace thinger;

int main() {
    http::server server;

    // Shared schema, referenced from routes and validated at runtime
    server.schema_component("Device", {
        {"type", "object"},
        {"required", {"name"}},
        {"properties", {
            {"name", {{"type", "string"}, {"minLength", 1}}},
            {"description", {{"type", "string"}}}
        }}
    });

    auto devices = server.group("/v1/users/:user/devices").tag("Devices");

    devices.get("/", [](http::request& req, http::response& res) {
        res.json(nlohmann::json::array({{{"name", "sensor"}}}));
    })
    .summary("List devices")
    .operation_id("listDevices")
    .path_param("user", "User name")
    .query_param("limit", "Maximum number of devices", {{"type", "integer"}, {"minimum", 1}})
    .returns(200, "Devices of the user", {{"type", "array"}, {"items", {{"$ref", "#/components/schemas/Device"}}}})
    .meta("permission", "Device:List");

    devices.post("/", [](http::request& req, nlohmann::json& body, http::response& res) -> thinger::awaitable<void> {
        res.json(body, http::http_response::status::created);
        co_return;
    })
    .schema({{"$ref", "#/components/schemas/Device"}})
    .example({{"name", "sensor"}, {"description", "Living room"}})
    .summary("Create a device")
    .operation_id("createDevice")
    .path_param("user", "User name")
    .returns(201, "Device created", {{"$ref", "#/components/schemas/Device"}})
    .returns(400, "Invalid device")
    .meta("permission", "Device:Create");

    devices.get("/:device([a-zA-Z0-9_-]{1,32})", [](http::request& req, http::response& res) {
        res.json({{"name", req["device"]}});
    })
    .summary("Get a device")
    .operation_id("getDevice")
    .path_param("user", "User name")
    .path_param("device", "Device identifier")
    .returns(200, "The device", {{"$ref", "#/components/schemas/Device"}}, {{"name", "sensor"}})
    .returns(404, "Device not found")
    .meta("permission", "Device:Read");

    // The application decides what its metadata means in the document
    server.openapi()
        .title("Devices API")
        .version("1.0.0")
        .description("Example API documented from its routes")
        .server("http://localhost:8091")
        .on_operation([](const http::route& route, const std::string&, nlohmann::json& operation) {
            if (route.has_meta("permission")) {
                operation["x-permission"] = route.get_meta("permission");
                operation["security"] = nlohmann::json::array({{{"bearerAuth", nlohmann::json::array()}}});
            }
        })
        .on_document([](nlohmann::json& document) {
            document["components"]["securitySchemes"]["bearerAuth"] = {{"type", "http"}, {"scheme", "bearer"}};
        });

    server.serve_openapi("/openapi.json");

    // Swagger UI loaded from a CDN
    server.get("/docs", [](http::response& res) {
        res.html(R"(<!doctype html>
<html>
<head>
  <title>Devices API</title>
  <link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist@5/swagger-ui.css">
</head>
<body>
  <div id="swagger-ui"></div>
  <script src="https://unpkg.com/swagger-ui-dist@5/swagger-ui-bundle.js"></script>
  <script>SwaggerUIBundle({url: "/openapi.json", dom_id: "#swagger-ui"});</script>
</body>
</html>)");
    }).hidden();

    std::cout << "Listening on http://localhost:8091 (docs at /docs)" << std::endl;
    server.start(8091);
    return 0;
}

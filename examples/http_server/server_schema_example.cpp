#include <thinger/http_server.hpp>
#include <iostream>

using namespace thinger;

// This example demonstrates JSON Schema validation on route handlers.
// Invalid requests are automatically rejected with 400 Bad Request.

int main(int argc, char* argv[]) {
    http::server server;

    // POST /api/users - Create a user with required fields
    server.post("/api/users", [](nlohmann::json& json, http::response& res) {
        // json is already validated — guaranteed to have name and email as strings
        res.json({
            {"created", true},
            {"name", json["name"]},
            {"email", json["email"]}
        }, http::http_response::status::created);
    }).schema({
        {"type", "object"},
        {"required", {"name", "email"}},
        {"properties", {
            {"name",  {{"type", "string"}, {"minLength", 1}}},
            {"email", {{"type", "string"}}},
            {"age",   {{"type", "integer"}, {"minimum", 0}, {"maximum", 150}}}
        }},
        {"additionalProperties", false}
    });

    // PUT /api/users/:id - Update a user (partial update allowed)
    server.put("/api/users/:id", [](http::request& req, nlohmann::json& json, http::response& res) {
        res.json({
            {"updated", true},
            {"id", req["id"]},
            {"fields", json}
        });
    }).schema({
        {"type", "object"},
        {"properties", {
            {"name",  {{"type", "string"}, {"minLength", 1}}},
            {"email", {{"type", "string"}}},
            {"age",   {{"type", "integer"}, {"minimum", 0}}}
        }}
    });

    // POST /api/orders - Nested object with enum validation
    server.post("/api/orders", [](nlohmann::json& json, http::response& res) {
        res.json({
            {"order_id", "ORD-001"},
            {"status", json["status"]},
            {"items_count", json["items"].size()}
        }, http::http_response::status::created);
    }).schema({
        {"type", "object"},
        {"required", {"items", "status", "shipping"}},
        {"properties", {
            {"status", {
                {"type", "string"},
                {"enum", {"pending", "confirmed", "shipped"}}
            }},
            {"items", {
                {"type", "array"},
                {"minItems", 1},
                {"items", {
                    {"type", "object"},
                    {"required", {"product", "quantity"}},
                    {"properties", {
                        {"product",  {{"type", "string"}}},
                        {"quantity", {{"type", "integer"}, {"minimum", 1}}}
                    }}
                }}
            }},
            {"shipping", {
                {"type", "object"},
                {"required", {"city", "country"}},
                {"properties", {
                    {"city",    {{"type", "string"}}},
                    {"country", {{"type", "string"}}},
                    {"zip",     {{"type", "string"}}}
                }}
            }}
        }}
    });

    // Endpoint without schema — accepts any JSON
    server.post("/api/raw", [](nlohmann::json& json, http::response& res) {
        res.json({{"received", true}, {"data", json}});
    });

    uint16_t port = 8090;
    if (argc > 1) port = std::stoi(argv[1]);

    std::cout << "Schema Validation Server starting on port " << port << std::endl;
    std::cout << "\nTry these requests:\n" << std::endl;

    std::cout << "# Valid user creation:" << std::endl;
    std::cout << R"(curl -X POST http://localhost:)" << port
              << R"(/api/users -H "Content-Type: application/json" -d '{"name":"Alice","email":"alice@example.com","age":30}')" << std::endl;

    std::cout << "\n# Missing required field (returns 400):" << std::endl;
    std::cout << R"(curl -X POST http://localhost:)" << port
              << R"(/api/users -H "Content-Type: application/json" -d '{"name":"Alice"}')" << std::endl;

    std::cout << "\n# Wrong type (returns 400):" << std::endl;
    std::cout << R"(curl -X POST http://localhost:)" << port
              << R"(/api/users -H "Content-Type: application/json" -d '{"name":123,"email":"test@test.com"}')" << std::endl;

    std::cout << "\n# Valid order with nested objects:" << std::endl;
    std::cout << R"(curl -X POST http://localhost:)" << port
              << R"(/api/orders -H "Content-Type: application/json" -d '{"status":"pending","items":[{"product":"Widget","quantity":3}],"shipping":{"city":"Madrid","country":"Spain"}}')" << std::endl;

    std::cout << std::endl;

    server.start("0.0.0.0", port);
    return 0;
}

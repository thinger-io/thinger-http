#include <thinger/http_server.hpp>
#include <iostream>

using namespace thinger;

// This example demonstrates request bodies in other types than JSON: the same handler and
// schema receive device data sent as JSON, MessagePack, CBOR, UBJSON or a urlencoded form,
// decoded by Content-Type. Bodies of types a route does not accept are answered with 415.

int main(int argc, char* argv[]) {
    http::server server;

    // Application default for routes without their own list (JSON only if not set)
    server.accept_body_types({"json", "msgpack", "cbor"});

    // Built-in decoders can be removed, and others added (e.g. CSV rows as arrays of strings)
    server.body_decoders().remove("ubjson");
    server.body_decoders().add("csv", {"text/csv"},
        [](std::string_view body, const http::media_type&, const http::body_limits&) {
            nlohmann::json row = nlohmann::json::array();
            size_t start = 0;
            while (start <= body.size()) {
                size_t end = std::min(body.find(',', start), body.size());
                row.push_back(std::string(body.substr(start, end - start)));
                start = end + 1;
            }
            return row;
        });

    // Limits of decoded bodies (the body size is limited by set_max_body_size)
    server.set_body_limits({.max_depth = 32});

    // Device data: the application types, plus forms
    server.post("/devices/:device/data", [](http::request& req, nlohmann::json& data, http::response& res) {
        // data is decoded and validated, whatever the type it was sent in;
        // reply in the type the client prefers (Accept), JSON by default
        res.encode({{"device", req["device"]}, {"received", data}});
    }).schema({
        {"type", "object"},
        {"required", {"temperature"}},
        {"properties", {
            // forms carry strings: accept both
            {"temperature", {{"type", {"number", "string"}}}},
            {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}}
        }}
    }).accepts({"json", "msgpack", "cbor", "form"});

    // Raw text, as a JSON string
    server.post("/devices/:device/log", [](nlohmann::json& line, http::response& res) {
        res.json({{"logged", line}});
    }).accepts({"text"});

    // Custom decoder
    server.post("/rows", [](nlohmann::json& row, http::response& res) {
        res.json({{"columns", row.size()}, {"row", row}});
    }).accepts({"csv"});

    // The OpenAPI document lists the media types of every route
    server.serve_openapi("/openapi.json");

    uint16_t port = 8091;
    if (argc > 1) port = std::stoi(argv[1]);
    std::string url = "http://localhost:" + std::to_string(port);

    std::cout << "Body decoders server starting on port " << port << std::endl;
    std::cout << "\nTry these requests:\n" << std::endl;

    std::cout << "# JSON:" << std::endl;
    std::cout << "curl " << url << R"(/devices/d1/data -H "Content-Type: application/json" -d '{"temperature":21.5,"tags":["a","b"]}')" << std::endl;

    std::cout << "\n# Form (repeated fields are arrays):" << std::endl;
    std::cout << "curl " << url << "/devices/d1/data -d 'temperature=21.5&tags=a&tags=b'" << std::endl;

    std::cout << "\n# MessagePack ({\"temperature\":21}), answered in MessagePack:" << std::endl;
    std::cout << "printf '\\x81\\xabtemperature\\x15' | curl " << url
              << "/devices/d1/data -H 'Content-Type: application/msgpack' -H 'Accept: application/msgpack' --data-binary @- | xxd" << std::endl;

    std::cout << "\n# CBOR ({\"temperature\":21}):" << std::endl;
    std::cout << "printf '\\xa1\\x6btemperature\\x15' | curl " << url
              << "/devices/d1/data -H 'Content-Type: application/cbor' --data-binary @-" << std::endl;

    std::cout << "\n# Not accepted by the route (returns 415):" << std::endl;
    std::cout << "curl " << url << "/devices/d1/data -H 'Content-Type: text/plain' -d 'temperature=21'" << std::endl;

    std::cout << "\n# Text and CSV:" << std::endl;
    std::cout << "curl " << url << "/devices/d1/log -H 'Content-Type: text/plain' -d 'rebooted'" << std::endl;
    std::cout << "curl " << url << "/rows -H 'Content-Type: text/csv' -d 'a,b,c'" << std::endl;

    std::cout << "\n# OpenAPI:" << std::endl;
    std::cout << "curl " << url << "/openapi.json" << std::endl;
    std::cout << std::endl;

    server.start("0.0.0.0", port);
    return 0;
}

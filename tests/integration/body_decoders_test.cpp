#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/util/types.hpp>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <boost/asio/use_future.hpp>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

using namespace thinger;
using json = nlohmann::json;

// Request bodies decoded by Content-Type: JSON, MessagePack, CBOR, UBJSON, forms, text and
// binary, accepted per application and per route

namespace {

    const json device = {
        {"name", "sensor-1"},
        {"location", "lab"},
        {"tags", {"a", "b"}}
    };

    const json device_schema = {
        {"type", "object"},
        {"required", {"name", "tags"}},
        {"properties", {
            {"name", {{"type", "string"}, {"minLength", 1}}},
            {"location", {{"type", "string"}}},
            {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}}
        }},
        {"additionalProperties", false}
    };

    std::string bytes(const std::vector<uint8_t>& data) {
        return {data.begin(), data.end()};
    }

    // The device in every format, with its Content-Type
    std::vector<std::pair<std::string, std::string>> device_bodies() {
        return {
            {"application/json", device.dump()},
            {"application/json; charset=utf-8", device.dump()},
            {"application/msgpack", bytes(json::to_msgpack(device))},
            {"application/x-msgpack", bytes(json::to_msgpack(device))},
            {"application/cbor", bytes(json::to_cbor(device))},
            {"application/ubjson", bytes(json::to_ubjson(device))},
            {"application/x-www-form-urlencoded", "name=sensor-1&location=lab&tags=a&tags=b"},
        };
    }

    std::shared_ptr<http::http_request> make_request(http::method method, const std::string& path,
                                                     const std::string& body = "",
                                                     const std::string& content_type = "") {
        auto request = http::http_request::create_http_request(method, "http://localhost" + path);
        if (!content_type.empty()) {
            request->set_content(body, content_type);
        } else if (!body.empty()) {
            request->set_content(body);
        }
        return request;
    }

    std::shared_ptr<http::http_response> run_dispatch(http::server& server, std::shared_ptr<http::http_request> request) {
        boost::asio::io_context ioc;
        auto result = co_spawn(ioc, server.dispatch(std::move(request)), boost::asio::use_future);
        ioc.run();
        return result.get();
    }

    std::shared_ptr<http::http_response> post(http::server& server, const std::string& path,
                                              const std::string& body, const std::string& content_type) {
        return run_dispatch(server, make_request(http::method::POST, path, body, content_type));
    }

    // Bodies received by the handlers
    struct received_bodies {
        std::mutex mutex;
        std::vector<json> values;

        void add(const json& value) {
            std::lock_guard lock(mutex);
            values.push_back(value);
        }

        size_t size() {
            std::lock_guard lock(mutex);
            return values.size();
        }
    };

    void add_device_routes(http::server& server, received_bodies& received) {
        // Every built-in structured format, synchronous handler
        server.post("/devices", [&received](json& body, http::response& res) {
            received.add(body);
            res.json(body, http::http_response::status::created);
        }).schema(device_schema).accepts({"json", "msgpack", "cbor", "ubjson", "form"});

        // Coroutine with the request and the validated body
        server.put("/devices/:id", [&received](http::request& req, json& body, http::response& res) -> awaitable<void> {
            received.add(body);
            res.json({{"id", req["id"]}, {"device", body}});
            co_return;
        }).schema(device_schema).accepts({"JSON", "application/msgpack", "application/x-msgpack", "cbor", "ubjson", "form"});

        // Application default: JSON only
        server.post("/json-only", [&received](json& body, http::response& res) {
            received.add(body);
            res.json(body);
        });
    }

}

TEST_CASE("The same object reaches the handler from every format", "[server][body][integration]") {
    http::server server;
    received_bodies received;
    add_device_routes(server, received);

    SECTION("In-memory dispatch") {
        for (const auto& [content_type, body] : device_bodies()) {
            INFO(content_type);
            auto response = post(server, "/devices", body, content_type);
            REQUIRE(response->get_status_code() == 201);
            REQUIRE(json::parse(response->get_content()) == device);

            auto put = run_dispatch(server, make_request(http::method::PUT, "/devices/7", body, content_type));
            REQUIRE(put->get_status_code() == 200);
            REQUIRE(json::parse(put->get_content()) == json{{"id", "7"}, {"device", device}});
        }
        REQUIRE(received.size() == 2 * device_bodies().size());
        for (const auto& value : received.values) REQUIRE(value == device);
    }

    SECTION("Over the network") {
        uint16_t port = 0;
        std::thread server_thread([&]() {
            server.start("127.0.0.1", 0, [&]() { port = server.local_port(); });
        });
        while (port == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::string base_url = "http://127.0.0.1:" + std::to_string(port);

        http::client client;
        for (const auto& [content_type, body] : device_bodies()) {
            INFO(content_type);
            auto response = client.post(base_url + "/devices", body, content_type);
            REQUIRE(response.status() == 201);
            REQUIRE(response.json() == device);
        }

        // Rejected before the body is read: the connection stays usable
        auto rejected = client.post(base_url + "/json-only", bytes(json::to_cbor(device)), "application/cbor");
        REQUIRE(rejected.status() == 415);
        auto accepted = client.post(base_url + "/json-only", device.dump(), "application/json");
        REQUIRE(accepted.status() == 200);

        server.stop();
        server_thread.join();
        REQUIRE(received.size() == device_bodies().size() + 1);
    }

    SECTION("Each one is validated with the same schema") {
        json invalid = {{"name", ""}, {"tags", {"a", "b"}}};
        for (const auto& [content_type, body] : std::vector<std::pair<std::string, std::string>>{
                 {"application/json", invalid.dump()},
                 {"application/msgpack", bytes(json::to_msgpack(invalid))},
                 {"application/cbor", bytes(json::to_cbor(invalid))},
                 {"application/ubjson", bytes(json::to_ubjson(invalid))},
                 {"application/x-www-form-urlencoded", "name=&tags=a&tags=b"},
             }) {
            INFO(content_type);
            auto response = post(server, "/devices", body, content_type);
            REQUIRE(response->get_status_code() == 400);
            auto error = json::parse(response->get_content());
            REQUIRE(error["error"]["context"].dump().find("name") != std::string::npos);
        }
        // A single value of a repeated form field is a string, not an array
        REQUIRE(post(server, "/devices", "name=x&tags=a", "application/x-www-form-urlencoded")->get_status_code() == 400);
        REQUIRE(received.size() == 0);
    }
}

TEST_CASE("Body types not accepted are answered with 415", "[server][body][integration]") {
    http::server server;
    received_bodies received;
    add_device_routes(server, received);

    SECTION("JSON only by default") {
        REQUIRE(post(server, "/json-only", device.dump(), "application/json")->get_status_code() == 200);
        REQUIRE(post(server, "/json-only", device.dump(), "application/problem+json")->get_status_code() == 200);
        for (const auto& content_type : {"application/msgpack", "application/cbor", "application/ubjson", "text/plain",
                                         "application/x-www-form-urlencoded", "application/octet-stream", "image/png",
                                         "multipart/form-data; boundary=x"}) {
            INFO(content_type);
            auto response = post(server, "/json-only", device.dump(), content_type);
            REQUIRE(response->get_status_code() == 415);
        }
        REQUIRE(post(server, "/json-only", device.dump(), "text/plain")->get_content() == "Unsupported Media Type: text/plain");
        REQUIRE(received.size() == 2);
    }

    SECTION("Without Content-Type the body is JSON") {
        auto response = post(server, "/json-only", device.dump(), "");
        REQUIRE(response->get_status_code() == 200);
        REQUIRE(json::parse(response->get_content()) == device);
        // And so it is rejected by a route not accepting JSON
        server.post("/cbor-only", [](json&, http::response& res) { res.send("ok"); }).accepts({"cbor"});
        REQUIRE(post(server, "/cbor-only", device.dump(), "")->get_status_code() == 415);
    }

    SECTION("Invalid or ambiguous Content-Type") {
        REQUIRE(post(server, "/json-only", "{}", "json")->get_status_code() == 415);
        REQUIRE(post(server, "/json-only", "{}", "application/json; charset=utf-16")->get_status_code() == 415);
        auto request = make_request(http::method::POST, "/json-only", "{}", "application/json");
        request->add_header("Content-Type", "application/cbor");
        REQUIRE(run_dispatch(server, request)->get_status_code() == 415);
        auto same = make_request(http::method::POST, "/json-only", "{}", "application/json");
        same->add_header("Content-Type", "application/json");
        REQUIRE(run_dispatch(server, same)->get_status_code() == 200);
    }

    SECTION("An empty body is null, whatever its type") {
        server.post("/optional", [](json& body, http::response& res) { res.json({{"null", body.is_null()}}); });
        auto response = post(server, "/optional", "", "text/plain");
        REQUIRE(response->get_status_code() == 200);
        REQUIRE(json::parse(response->get_content())["null"] == true);
    }

    SECTION("Through the error formatter") {
        server.set_error_formatter([](const http::http_error& error, http::http_response& response) {
            response.set_content(json{{"status", static_cast<int>(error.status)}, {"message", error.message}}.dump(),
                                 "application/json");
        });
        auto response = post(server, "/json-only", "x", "application/cbor");
        REQUIRE(response->get_status_code() == 415);
        REQUIRE(json::parse(response->get_content()) ==
                json{{"status", 415}, {"message", "Unsupported Media Type: application/cbor"}});

        response = post(server, "/devices", "\xa1", "application/cbor");
        REQUIRE(response->get_status_code() == 400);
        REQUIRE(json::parse(response->get_content()) == json{{"status", 400}, {"message", "Invalid CBOR"}});
    }
}

TEST_CASE("Accepted body types per application and per route", "[server][body][integration]") {
    http::server server;
    std::atomic<int> calls{0};

    SECTION("Application default") {
        server.accept_body_types({"json", "MsgPack"});
        server.post("/default", [&calls](json& body, http::response& res) { calls++; res.json(body); });
        server.post("/own", [&calls](json& body, http::response& res) { calls++; res.json(body); }).accepts({"cbor"});

        REQUIRE(server.get_body_types() == std::vector<std::string>{"json", "msgpack"});
        REQUIRE(post(server, "/default", bytes(json::to_msgpack(device)), "application/msgpack")->get_status_code() == 200);
        REQUIRE(post(server, "/default", bytes(json::to_cbor(device)), "application/cbor")->get_status_code() == 415);
        // The route list replaces the application one
        REQUIRE(post(server, "/own", bytes(json::to_cbor(device)), "application/cbor")->get_status_code() == 200);
        REQUIRE(post(server, "/own", device.dump(), "application/json")->get_status_code() == 415);
        REQUIRE(calls == 2);
    }

    SECTION("Removed decoders are no longer accepted") {
        server.body_decoders().remove("ubjson");
        server.post("/devices", [&calls](json&, http::response& res) { calls++; res.send("ok"); })
            .accepts({"json", "ubjson"});
        REQUIRE(post(server, "/devices", bytes(json::to_ubjson(device)), "application/ubjson")->get_status_code() == 415);
        REQUIRE(post(server, "/devices", device.dump(), "application/json")->get_status_code() == 200);
        REQUIRE(calls == 1);
    }

    SECTION("Custom decoders") {
        server.body_decoders().add("csv", {"text/csv"}, [](std::string_view body, const http::media_type&,
                                                           const http::body_limits&) {
            json row = json::array();
            size_t start = 0;
            while (start <= body.size()) {
                size_t end = std::min(body.find(',', start), body.size());
                row.push_back(std::string(body.substr(start, end - start)));
                start = end + 1;
            }
            if (row.size() > 3) throw std::runtime_error("too many columns");
            return row;
        });
        server.post("/rows", [](json& body, http::response& res) { res.json(body); }).accepts({"csv"});
        auto response = post(server, "/rows", "a,b,c", "text/csv; header=absent");
        REQUIRE(response->get_status_code() == 200);
        REQUIRE(json::parse(response->get_content()) == json{"a", "b", "c"});
        response = post(server, "/rows", "a,b,c,d", "text/csv");
        REQUIRE(response->get_status_code() == 400);
        REQUIRE(response->get_content() == "Invalid csv body");
    }

    SECTION("Text and binary only when a route asks for them") {
        server.post("/text", [](json& body, http::response& res) { res.json({{"text", body}}); }).accepts({"text"});
        server.post("/binary", [](json& body, http::response& res) {
            res.json({{"binary", body.is_binary()}, {"size", body.get_binary().size()}});
        }).accepts({"binary"});

        auto response = post(server, "/text", "hello", "text/plain; charset=utf-8");
        REQUIRE(json::parse(response->get_content()) == json{{"text", "hello"}});
        REQUIRE(post(server, "/text", "caf\xe9", "text/plain")->get_status_code() == 400);
        response = post(server, "/binary", std::string("\x00\xff\x01", 3), "application/octet-stream");
        REQUIRE(json::parse(response->get_content()) == json{{"binary", true}, {"size", 3}});
        REQUIRE(post(server, "/binary", "x", "text/plain")->get_status_code() == 415);
    }

    SECTION("Routes reading the body themselves") {
        // Any type unless they list theirs; then those, decoder or not
        server.post("/raw", [](http::request& req, http::response& res) -> awaitable<void> {
            co_await req.read_body();
            res.send(req.body());
        });
        server.post("/image", [](http::request& req, http::response& res) -> awaitable<void> {
            co_await req.read_body();
            res.send(std::to_string(req.body().size()));
        }).accepts({"image/png", "image/jpeg"});

        REQUIRE(post(server, "/raw", "anything", "application/x-custom")->get_content() == "anything");
        REQUIRE(post(server, "/image", "png!", "image/png")->get_content() == "4");
        REQUIRE(post(server, "/image", "gif!", "image/gif")->get_status_code() == 415);
        // Requests without body are not checked
        REQUIRE(run_dispatch(server, make_request(http::method::POST, "/image"))->get_status_code() == 200);
    }
}

TEST_CASE("Invalid and abusive bodies are rejected safely", "[server][body][integration]") {
    http::server server;
    received_bodies received;
    add_device_routes(server, received);

    SECTION("Corrupt bodies: 400") {
        for (const auto& [content_type, body, message] : std::vector<std::tuple<std::string, std::string, std::string>>{
                 {"application/json", "{\"name\":", "Invalid JSON"},
                 {"application/msgpack", "\x82\xa4name", "Invalid MessagePack"},
                 {"application/cbor", "\xa1\x64na", "Invalid CBOR"},
                 {"application/ubjson", "{i\x04name", "Invalid UBJSON"},
                 {"application/x-www-form-urlencoded", "name=%G1", "Invalid form data: malformed percent-encoding"},
                 {"application/msgpack", "\x81\xa4name\xa2\xc3\x28", "Invalid MessagePack: string not valid UTF-8"},
             }) {
            INFO(content_type);
            auto response = post(server, "/devices", body, content_type);
            REQUIRE(response->get_status_code() == 400);
            REQUIRE(response->get_content() == message);
        }
        REQUIRE(received.size() == 0);
    }

    SECTION("Deep nesting: 400") {
        std::string deep_json = std::string(10000, '[') + std::string(10000, ']');
        REQUIRE(post(server, "/devices", deep_json, "application/json")->get_content() ==
                "Invalid JSON: nested deeper than 128 levels");
        REQUIRE(post(server, "/devices", std::string(100000, '\x91'), "application/msgpack")->get_status_code() == 400);
        REQUIRE(post(server, "/devices", std::string(100000, '\x81'), "application/cbor")->get_status_code() == 400);
        REQUIRE(post(server, "/devices", std::string(100000, '['), "application/ubjson")->get_status_code() == 400);

        server.set_body_limits({.max_depth = 2});
        REQUIRE(post(server, "/json-only", "[[1]]", "application/json")->get_status_code() == 200);
        REQUIRE(post(server, "/json-only", "[[[1]]]", "application/json")->get_status_code() == 400);
        REQUIRE(server.get_body_limits().max_depth == 2);
    }

    SECTION("Huge declared lengths: 400") {
        REQUIRE(post(server, "/devices", "\xdd\xff\xff\xff\xff\xc0", "application/msgpack")->get_status_code() == 400);
        REQUIRE(post(server, "/devices", "\xdb\xff\xff\xff\xff" "abc", "application/msgpack")->get_status_code() == 400);
        REQUIRE(post(server, "/devices", std::string("\x9b\xff\xff\xff\xff\xff\xff\xff\xff\xf6", 10), "application/cbor")
                    ->get_status_code() == 400);
        auto response = post(server, "/devices", std::string("[$Z#l\x7f\xff\xff\xff", 9), "application/ubjson");
        REQUIRE(response->get_status_code() == 400);
        REQUIRE(response->get_content() == "Invalid UBJSON: more values than bytes");
    }

    SECTION("The body size limit applies before decoding: 413") {
        server.set_max_body_size(64);
        json big = device;
        big["location"] = std::string(100, 'x');
        for (const auto& [content_type, body] : std::vector<std::pair<std::string, std::string>>{
                 {"application/json", big.dump()},
                 {"application/msgpack", bytes(json::to_msgpack(big))},
                 {"application/cbor", bytes(json::to_cbor(big))},
                 {"application/x-www-form-urlencoded", "name=a&tags=b&location=" + std::string(100, 'x')},
             }) {
            INFO(content_type);
            REQUIRE(post(server, "/devices", body, content_type)->get_status_code() == 413);
        }
        REQUIRE(received.size() == 0);
    }
}

TEST_CASE("OpenAPI lists the accepted body types", "[server][body][openapi][integration]") {
    http::server server;
    received_bodies received;
    add_device_routes(server, received);
    server.post("/upload", [](http::request&, http::response& res) -> awaitable<void> {
        res.send("ok");
        co_return;
    }).accepts({"image/png"});
    server.post("/text", [](json& body, http::response& res) { res.json(body); }).accepts({"text", "binary"});

    auto document = server.openapi().generate();
    auto content = [&](const std::string& path, const std::string& method) {
        return document["paths"][path][method]["requestBody"]["content"];
    };

    auto devices = content("/devices", "post");
    std::vector<std::string> types;
    for (const auto& [type, media] : devices.items()) {
        types.push_back(type);
        REQUIRE(media["schema"] == device_schema);
    }
    REQUIRE(types == std::vector<std::string>{"application/cbor", "application/json", "application/msgpack",
                                              "application/ubjson", "application/x-msgpack",
                                              "application/x-www-form-urlencoded"});
    REQUIRE(content("/devices/{id}", "put").size() == 6);
    REQUIRE(content("/json-only", "post").size() == 1);
    REQUIRE(content("/json-only", "post").contains("application/json"));
    REQUIRE(content("/upload", "post").size() == 1);
    REQUIRE(content("/upload", "post").contains("image/png"));
    REQUIRE(content("/text", "post").contains("text/plain"));
    REQUIRE(content("/text", "post").contains("application/octet-stream"));

    // The application default is documented for routes without their own list
    server.accept_body_types({"json", "cbor"});
    document = server.openapi().generate();
    REQUIRE(content("/json-only", "post").size() == 2);
    REQUIRE(content("/json-only", "post").contains("application/cbor"));

    // A generator for a virtual host, with the application settings
    server.host("api.example.com").post("/values", [](json& body, http::response& res) { res.json(body); });
    auto host_document = http::openapi_generator(server.host("api.example.com").router(), &server.get_body_settings()).generate();
    REQUIRE(host_document["paths"]["/values"]["post"]["requestBody"]["content"].size() == 2);
    // Without them: the defaults (JSON only)
    host_document = http::openapi_generator(server.host("api.example.com").router()).generate();
    REQUIRE(host_document["paths"]["/values"]["post"]["requestBody"]["content"].size() == 1);
}

TEST_CASE("Responses are encoded as the Accept header prefers", "[server][body][integration]") {
    http::server server;
    server.get("/device", [](http::response& res) { res.encode(device); });

    auto get = [&](const std::string& accept) {
        auto request = make_request(http::method::GET, "/device");
        request->remove_headers("Accept");
        if (!accept.empty()) request->add_header("Accept", accept);
        return run_dispatch(server, request);
    };

    auto response = get("");
    REQUIRE(response->get_header("Content-Type") == "application/json");
    REQUIRE(response->get_header("Vary") == "Accept");
    REQUIRE(json::parse(response->get_content()) == device);

    response = get("application/msgpack");
    REQUIRE(response->get_header("Content-Type") == "application/msgpack");
    REQUIRE(json::from_msgpack(response->get_content()) == device);

    response = get("application/cbor, application/json;q=0.5");
    REQUIRE(response->get_header("Content-Type") == "application/cbor");
    REQUIRE(json::from_cbor(response->get_content()) == device);

    response = get("application/ubjson");
    REQUIRE(json::from_ubjson(response->get_content()) == device);

    response = get("text/html");
    REQUIRE(response->get_header("Content-Type") == "application/json");
}

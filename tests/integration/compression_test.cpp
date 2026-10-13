#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/util/compression.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <future>

#include "server_test_helpers.hpp"

using namespace thinger;
using namespace std::chrono_literals;

// Generate a large JSON payload (>200 bytes to trigger compression)
static nlohmann::json make_large_json() {
    nlohmann::json data;
    data["message"] = std::string(1000, 'A');
    data["numbers"] = nlohmann::json::array();
    for (int i = 0; i < 100; i++) data["numbers"].push_back(i);
    return data;
}

struct CompressionTestFixture {
    http::server server;
    uint16_t port = 0;
    std::string base_url;
    std::thread server_thread;

    CompressionTestFixture() {
        // POST /echo - echoes back the request body as JSON
        server.post("/echo", [](http::request& req, http::response& res) {
            auto data = req.json();
            res.json(data);
        });

        // GET /large - returns a large JSON payload
        server.get("/large", [](http::request& req, http::response& res) {
            res.json(make_large_json());
        });

        // GET /small - returns a small JSON payload (<200 bytes)
        server.get("/small", [](http::request& req, http::response& res) {
            res.json({{"ok", true}});
        });

        start_server();
    }

    ~CompressionTestFixture() {
        server.stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

private:
    void start_server() {
        REQUIRE(server.listen("0.0.0.0", 0));
        port = server.local_port();
        base_url = "http://localhost:" + std::to_string(port);

        std::promise<void> ready;
        server_thread = std::thread([this, &ready]() {
            ready.set_value();
            server.wait();
        });
        ready.get_future().wait();
    }
};

TEST_CASE("Server compresses response when client sends Accept-Encoding", "[compression][integration]") {
    CompressionTestFixture fixture;
    http::client client;
    client.timeout(10s);

    // Client auto-sends Accept-Encoding: gzip, deflate (auto_decompress=true by default)
    // and auto-decompresses the response
    auto response = client.get(fixture.base_url + "/large");
    REQUIRE(response.ok());

    auto json = response.json();
    auto expected = make_large_json();
    REQUIRE(json == expected);
}

TEST_CASE("Server decompresses gzip request body", "[compression][integration]") {
    CompressionTestFixture fixture;
    http::client client;
    client.timeout(10s);

    nlohmann::json payload = {{"key", "value"}, {"number", 42}};
    std::string json_str = payload.dump();

    // Compress the body with gzip
    std::string compressed = util::gzip::compress(json_str).value();

    // Send compressed body with Content-Encoding: gzip
    auto response = client.request(fixture.base_url + "/echo")
        .header("Content-Encoding", "gzip")
        .body(compressed, "application/json")
        .post();

    REQUIRE(response.ok());
    auto result = response.json();
    REQUIRE(result["key"] == "value");
    REQUIRE(result["number"] == 42);
}

TEST_CASE("Full round-trip: gzip compressed request + compressed response", "[compression][integration]") {
    CompressionTestFixture fixture;
    http::client client;
    client.timeout(10s);

    // Build a large payload so the response also gets compressed
    nlohmann::json payload = make_large_json();
    std::string json_str = payload.dump();

    // Compress request body with gzip
    std::string compressed = util::gzip::compress(json_str).value();

    // Client auto-sends Accept-Encoding and auto-decompresses response
    auto response = client.request(fixture.base_url + "/echo")
        .header("Content-Encoding", "gzip")
        .body(compressed, "application/json")
        .post();

    REQUIRE(response.ok());
    auto result = response.json();
    REQUIRE(result == payload);
}

TEST_CASE("Full round-trip with deflate", "[compression][integration]") {
    CompressionTestFixture fixture;
    http::client client;
    client.timeout(10s);

    nlohmann::json payload = make_large_json();
    std::string json_str = payload.dump();

    // Compress request body with deflate
    std::string compressed = util::deflate::compress(json_str).value();

    auto response = client.request(fixture.base_url + "/echo")
        .header("Content-Encoding", "deflate")
        .body(compressed, "application/json")
        .post();

    REQUIRE(response.ok());
    auto result = response.json();
    REQUIRE(result == payload);
}

TEST_CASE("Small body is not compressed", "[compression][integration]") {
    CompressionTestFixture fixture;
    http::client client;
    client.timeout(10s);
    // Disable auto-decompress so we can inspect raw response
    client.auto_decompress(false);

    // GET /small returns a tiny payload (<200 bytes)
    auto response = client.get(fixture.base_url + "/small");
    REQUIRE(response.ok());

    // Should NOT have Content-Encoding since body is too small
    REQUIRE_FALSE(response.has_header("Content-Encoding"));

    auto json = response.json();
    REQUIRE(json["ok"] == true);
}

TEST_CASE("No Accept-Encoding means no compression", "[compression][integration]") {
    CompressionTestFixture fixture;
    http::client client;
    client.timeout(10s);
    // Disable auto-decompress (which also prevents sending Accept-Encoding)
    client.auto_decompress(false);

    auto response = client.get(fixture.base_url + "/large");
    REQUIRE(response.ok());

    // No Accept-Encoding sent, so server should not compress
    REQUIRE_FALSE(response.has_header("Content-Encoding"));

    auto json = response.json();
    auto expected = make_large_json();
    REQUIRE(json == expected);
}

TEST_CASE("Compression is negotiated and announced with Vary", "[compression][integration]") {
    http::server server;
    server.get("/large", [](http::response& res) { res.json(make_large_json()); });
    server.get("/small", [](http::response& res) { res.json({{"ok", true}}); });
    server.get("/no-transform", [](http::response& res) {
        res.header("Cache-Control", "no-transform");
        res.json(make_large_json());
    });
    server.get("/encoded", [](http::response& res) {
        res.encode(make_large_json());   // Vary: Accept
    });
    server_test::running_server running(server);

    // The head of the answer to a raw request, lowercase
    auto head = [&](const std::string& path, const std::string& headers, const std::string& method = "GET") {
        auto result = server_test::raw_exchange(running.port, method + " " + path + " HTTP/1.1\r\nHost: x\r\n"
                                                + headers + "Connection: close\r\n\r\n");
        auto head = result.data.substr(0, result.data.find("\r\n\r\n") + 2);
        for (auto& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        REQUIRE(head.starts_with("http/1.1 200"));
        return head;
    };
    auto has = [](const std::string& head, const std::string& line) {
        return head.find("\r\n" + line + "\r\n") != std::string::npos;
    };

    SECTION("Compressed or not, compressible responses vary on Accept-Encoding") {
        auto compressed = head("/large", "Accept-Encoding: gzip\r\n");
        REQUIRE(has(compressed, "content-encoding: gzip"));
        REQUIRE(has(compressed, "vary: accept-encoding"));

        auto identity = head("/large", "");
        REQUIRE(identity.find("content-encoding") == std::string::npos);
        REQUIRE(has(identity, "vary: accept-encoding"));

        auto head_request = head("/large", "Accept-Encoding: gzip\r\n", "HEAD");
        REQUIRE(head_request.find("content-encoding") == std::string::npos);
        REQUIRE(has(head_request, "vary: accept-encoding"));

        // too small to be compressed: one representation
        REQUIRE(head("/small", "Accept-Encoding: gzip\r\n").find("vary") == std::string::npos);
    }

    SECTION("Codings and their quality") {
        REQUIRE(has(head("/large", "Accept-Encoding: GZip\r\n"), "content-encoding: gzip"));
        REQUIRE(has(head("/large", "Accept-Encoding: deflate, gzip\r\n"), "content-encoding: gzip"));
        REQUIRE(has(head("/large", "Accept-Encoding: gzip;q=0, deflate\r\n"), "content-encoding: deflate"));
        REQUIRE(has(head("/large", "Accept-Encoding: gzip;q=0.2, deflate;q=0.9\r\n"), "content-encoding: deflate"));
        REQUIRE(has(head("/large", "Accept-Encoding: gzip;q=0\r\nAccept-Encoding: deflate\r\n"), "content-encoding: deflate"));
        REQUIRE(head("/large", "Accept-Encoding: gzip;q=0\r\n").find("content-encoding") == std::string::npos);
        REQUIRE(head("/large", "Accept-Encoding: br\r\n").find("content-encoding") == std::string::npos);
        REQUIRE(head("/large", "Accept-Encoding: identity\r\n").find("content-encoding") == std::string::npos);
    }

    SECTION("Vary keeps the fields already there, and no-transform is respected") {
        auto encoded = head("/encoded", "Accept-Encoding: gzip\r\n");
        REQUIRE(has(encoded, "vary: accept, accept-encoding"));
        REQUIRE(has(encoded, "content-encoding: gzip"));

        auto no_transform = head("/no-transform", "Accept-Encoding: gzip\r\n");
        REQUIRE(no_transform.find("content-encoding") == std::string::npos);
        REQUIRE(no_transform.find("vary") == std::string::npos);
    }
}

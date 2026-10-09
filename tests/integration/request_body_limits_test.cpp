#include "server_test_helpers.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/util/types.hpp>
#include <thinger/util/compression.hpp>
#include <atomic>
#include <chrono>

using namespace thinger;
using namespace std::chrono_literals;
using namespace server_test;

// Request bodies are bounded by the maximum body size everywhere: read_body() on deferred
// routes, and compressed bodies once decompressed

TEST_CASE("read_body() is bounded on deferred routes", "[server][body][deferred][413][integration]") {
    http::server server;
    server.set_max_body_size(1024);
    std::atomic<int> handler_calls{0};
    server.post("/deferred", [&](http::request& req, http::response& res) -> thinger::awaitable<void> {
        handler_calls++;
        if (!co_await req.read_body()) {
            res.error(http::http_response::status::payload_too_large, "too large");
            co_return;
        }
        res.send("size=" + std::to_string(req.body().size()));
    });
    running_server running(server);

    SECTION("Huge Content-Length is rejected without allocating or waiting for it") {
        auto result = raw_exchange(running.port,
            "POST /deferred HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1000000000000\r\n\r\nabc");
        INFO("response: " << result.data);
        REQUIRE(handler_calls == 1);
        REQUIRE(result.data.starts_with("HTTP/1.1 413"));
        REQUIRE(result.closed);
    }

    SECTION("Content-Length over the limit") {
        auto result = raw_exchange(running.port,
            "POST /deferred HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2000\r\n\r\n" + std::string(2000, 'x'));
        INFO("response: " << result.data);
        REQUIRE(result.data.starts_with("HTTP/1.1 413"));
    }

    SECTION("Body within the limit") {
        auto result = raw_exchange(running.port,
            "POST /deferred HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Length: 1000\r\n\r\n" + std::string(1000, 'x'));
        INFO("response: " << result.data);
        REQUIRE(result.data.starts_with("HTTP/1.1 200"));
        REQUIRE(result.data.find("size=1000") != std::string::npos);
    }
}

TEST_CASE("Compressed request bodies are validated and bounded", "[server][body][compression][integration]") {
    http::server server;
    server.set_max_body_size(64 * 1024);
    server.post("/size", [](http::request& req, http::response& res) {
        res.send(std::to_string(req.body().size()));
    });

    auto compressed_request = [](const std::string& encoding, const std::string& body) {
        auto request = make_request(http::method::POST, "/size", body);
        request->add_header("Content-Encoding", encoding);
        return request;
    };
    http::dispatch_options options;
    options.timeout = 5s;

    const std::string original(1000, 'a');
    auto gzip = util::gzip::compress(original);
    auto deflate = util::deflate::compress(original);
    REQUIRE(gzip);
    REQUIRE(deflate);

    SECTION("Valid bodies are decompressed") {
        auto result = run_dispatch(server, compressed_request("gzip", *gzip), options);
        REQUIRE(result->get_content() == "1000");
        result = run_dispatch(server, compressed_request("deflate", *deflate), options);
        REQUIRE(result->get_content() == "1000");
    }

    SECTION("Truncated gzip body") {
        auto result = run_dispatch(server, compressed_request("gzip", gzip->substr(0, gzip->size() / 2)), options);
        REQUIRE(result->get_status() == http::http_response::status::bad_request);
    }

    SECTION("Truncated deflate body") {
        auto result = run_dispatch(server, compressed_request("deflate", deflate->substr(0, deflate->size() / 2)), options);
        REQUIRE(result->get_status() == http::http_response::status::bad_request);
    }

    SECTION("Not compressed at all") {
        auto result = run_dispatch(server, compressed_request("gzip", "plain text"), options);
        REQUIRE(result->get_status() == http::http_response::status::bad_request);
    }

    SECTION("Decompressed size over the maximum body size") {
        auto bomb = util::gzip::compress(std::string(1024 * 1024, 'a'));
        REQUIRE(bomb->size() < 64 * 1024);
        auto result = run_dispatch(server, compressed_request("gzip", *bomb), options);
        REQUIRE(result->get_status() == http::http_response::status::payload_too_large);
    }
}

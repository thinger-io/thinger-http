#include "server_test_helpers.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/util/types.hpp>
#include <thinger/util/compression.hpp>
#include "../fixtures/compression_bomb.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

using namespace thinger;
using namespace std::chrono_literals;
using namespace server_test;

// The maximum body size holds more than 4 GiB on any platform
static_assert(std::is_same_v<std::remove_cv_t<decltype(http::default_max_body_size)>, std::uint64_t>);
static_assert(std::is_same_v<decltype(&http::request::set_max_body_size), void (http::request::*)(std::uint64_t)>);
static_assert(std::is_same_v<decltype(std::declval<const http::request&>().get_max_body_size()), std::uint64_t>);
static_assert(std::is_same_v<decltype(&http::http_application::set_max_body_size), void (http::http_application::*)(std::uint64_t)>);

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

// A head announcing 5 GiB: the check of the size happens at the first read, before the body
// arrives, so a few bytes are enough
TEST_CASE("Body limits over 4 GiB", "[server][body][deferred][413][integration]") {
    constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
    constexpr auto five_gib = 5 * gib;
    auto handler = [](http::request& req, http::response& res) -> thinger::awaitable<void> {
        uint8_t buffer[4];
        size_t bytes = co_await req.read_some(buffer, sizeof(buffer));
        if (req.get_body_error() != http::body_error::none) co_return;   // answered by the server
        res.send("read=" + std::to_string(bytes) + " limit=" + std::to_string(req.get_max_body_size()));
    };
    http::server server;
    server.post("/six", handler).max_body_size(6 * gib);
    server.post("/four", handler).max_body_size(4 * gib);
    running_server running(server);

    auto head = [&](const std::string& path) {
        return "POST " + path + " HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(five_gib) + "\r\n\r\nabcd";
    };

    SECTION("5 GiB with a maximum of 6 GiB is not rejected") {
        auto result = raw_exchange(running.port, head("/six"));
        INFO("response: " << result.data);
        REQUIRE(result.data.starts_with("HTTP/1.1 200"));
        REQUIRE(result.data.find("read=4 limit=" + std::to_string(6 * gib)) != std::string::npos);
    }

    SECTION("5 GiB with a maximum of 4 GiB is rejected") {
        auto result = raw_exchange(running.port, head("/four"));
        INFO("response: " << result.data);
        REQUIRE(result.data.starts_with("HTTP/1.1 413"));
        REQUIRE(result.closed);
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

    SECTION("Coding names are case-insensitive, and x-gzip is gzip") {
        for (auto encoding : {"GZIP", "x-gzip", "X-Gzip"}) {
            CAPTURE(encoding);
            auto result = run_dispatch(server, compressed_request(encoding, *gzip), options);
            REQUIRE(result->get_content() == "1000");
        }
        auto result = run_dispatch(server, compressed_request("Deflate", *deflate), options);
        REQUIRE(result->get_content() == "1000");
    }

    SECTION("gzip members, concatenated, and several codings") {
        auto result = run_dispatch(server, compressed_request("gzip", *gzip + *gzip), options);
        REQUIRE(result->get_content() == "2000");
        result = run_dispatch(server, compressed_request("deflate, gzip", *util::gzip::compress(*deflate)), options);
        REQUIRE(result->get_content() == "1000");
    }

    SECTION("Bytes after the compressed data: 400") {
        auto result = run_dispatch(server, compressed_request("gzip", *gzip + "garbage"), options);
        REQUIRE(result->get_status() == http::http_response::status::bad_request);
        result = run_dispatch(server, compressed_request("deflate", *deflate + "x"), options);
        REQUIRE(result->get_status() == http::http_response::status::bad_request);
    }

    SECTION("Codings not supported are left to the handler") {
        auto result = run_dispatch(server, compressed_request("br", "0123"), options);
        REQUIRE(result->get_content() == "4");
        result = run_dispatch(server, compressed_request("gzip, br", *gzip), options);
        REQUIRE(result->get_content() == std::to_string(gzip->size()));
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

    SECTION("Decompressed size exactly at the maximum body size, and one byte over") {
        for (const std::string encoding : {"gzip", "deflate"}) {
            CAPTURE(encoding);
            auto compress = encoding == "gzip" ? util::gzip::compress : util::deflate::compress;
            auto exact = compress(std::string(64 * 1024, 'a'));
            auto over = compress(std::string(64 * 1024 + 1, 'a'));
            auto result = run_dispatch(server, compressed_request(encoding, *exact), options);
            REQUIRE(result->get_content() == std::to_string(64 * 1024));
            result = run_dispatch(server, compressed_request(encoding, *over), options);
            REQUIRE(result->get_status() == http::http_response::status::payload_too_large);
        }
    }

    SECTION("A bomb of a few KB per MiB stops inflating at the limit") {
        // Its broken checksum is at the end: past the limit it is too large, not invalid
        for (auto format : {compression_bomb::format::gzip, compression_bomb::format::deflate}) {
            auto bomb = compression_bomb::zeros(format, 32);
            REQUIRE(bomb.size() < 64 * 1024);
            bomb.back() = static_cast<char>(bomb.back() ^ 0x55);
            auto encoding = format == compression_bomb::format::gzip ? "gzip" : "deflate";
            CAPTURE(encoding);
            auto result = run_dispatch(server, compressed_request(encoding, bomb), options);
            REQUIRE(result->get_status() == http::http_response::status::payload_too_large);
        }
    }
}

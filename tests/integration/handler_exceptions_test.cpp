#include "server_test_helpers.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/util/types.hpp>
#include <stdexcept>
#include <atomic>
#include <chrono>

using namespace thinger;
using namespace std::chrono_literals;
using namespace server_test;

// Exceptions thrown by handlers: answered with 500 (in the configured error format), or
// the connection closed if the response was already started

namespace {
    const std::string get_close = "GET /ok HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
}

TEST_CASE("Handler exceptions are answered with 500 over a connection", "[server][exceptions][integration]") {
    http::server server;
    server.set_error_formatter([](const http::http_error& error, http::http_response& response) {
        response.set_content(nlohmann::json{{"failed", error.message}, {"code", static_cast<int>(error.status)}}.dump(),
                             "application/json");
    });
    server.get("/ok", [](http::response& res) { res.send("fine"); });
    server.get("/throw-sync", [](http::request&, http::response&) {
        throw std::runtime_error("sync failure");
    });
    server.get("/throw-coro", [](http::request&, http::response&) -> thinger::awaitable<void> {
        throw std::runtime_error("coroutine failure");
        co_return;
    });
    server.post("/throw-json", [](nlohmann::json&, http::response&) -> thinger::awaitable<void> {
        throw std::runtime_error("json failure");
        co_return;
    });
    server.get("/throw-after-response", [](http::response& res) {
        res.send("sent");
        throw std::runtime_error("late failure");
    });
    server.get("/throw-mid-chunked", [](http::response& res) {
        res.start_chunked("text/plain");
        res.write_chunk("partial");
        throw std::runtime_error("streaming failure");
    });
    running_server running(server);

    SECTION("Synchronous handler, followed by a pipelined request") {
        auto result = raw_exchange(running.port, "GET /throw-sync HTTP/1.1\r\nHost: localhost\r\n\r\n" + get_close);
        INFO("response: " << result.data);
        REQUIRE(result.data.starts_with("HTTP/1.1 500"));
        REQUIRE(result.data.find("\"code\":500") != std::string::npos);
        REQUIRE(result.data.find("fine") != std::string::npos);
        REQUIRE(count(result.data, "HTTP/1.1 ") == 2);
    }

    SECTION("Coroutine handler, followed by a pipelined request") {
        auto result = raw_exchange(running.port, "GET /throw-coro HTTP/1.1\r\nHost: localhost\r\n\r\n" + get_close);
        INFO("response: " << result.data);
        REQUIRE(result.data.starts_with("HTTP/1.1 500"));
        REQUIRE(result.data.find("\"code\":500") != std::string::npos);
        REQUIRE(result.data.find("fine") != std::string::npos);
    }

    SECTION("JSON handler with a body, followed by a pipelined request") {
        std::string body = R"({"a":1})";
        auto result = raw_exchange(running.port,
            "POST /throw-json HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body + get_close);
        INFO("response: " << result.data);
        REQUIRE(result.data.starts_with("HTTP/1.1 500"));
        REQUIRE(result.data.find("fine") != std::string::npos);
    }

    SECTION("Exception after responding closes the connection") {
        auto result = raw_exchange(running.port, "GET /throw-after-response HTTP/1.1\r\nHost: localhost\r\n\r\n" + get_close);
        INFO("response: " << result.data);
        REQUIRE(result.data.starts_with("HTTP/1.1 200"));
        REQUIRE(result.data.find("sent") != std::string::npos);
        REQUIRE(count(result.data, "HTTP/1.1 ") == 1);
        REQUIRE(result.closed);
    }

    SECTION("Exception in the middle of a chunked response closes the connection") {
        auto result = raw_exchange(running.port, "GET /throw-mid-chunked HTTP/1.1\r\nHost: localhost\r\n\r\n" + get_close);
        INFO("response: " << result.data);
        REQUIRE(result.data.starts_with("HTTP/1.1 200"));
        REQUIRE(result.data.find("partial") != std::string::npos);
        REQUIRE(count(result.data, "HTTP/1.1 ") == 1);
        REQUIRE(result.closed);
    }
}

TEST_CASE("Handler exceptions in memory dispatch use the error formatter", "[server][exceptions][dispatch][integration]") {
    http::server server;
    server.set_error_formatter([](const http::http_error& error, http::http_response& response) {
        response.set_content(nlohmann::json{{"failed", static_cast<int>(error.status)}}.dump(), "application/json");
    });
    server.get("/throw", [](http::response&) { throw std::runtime_error("failure"); });

    auto result = run_dispatch(server, make_request(http::method::GET, "/throw"));
    REQUIRE(result->get_status() == http::http_response::status::internal_server_error);
    REQUIRE(result->get_content() == R"({"failed":500})");
}

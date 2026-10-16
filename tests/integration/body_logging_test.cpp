#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/util/logger.hpp>
#include <spdlog/sinks/ringbuffer_sink.h>

#include <algorithm>
#include <string>
#include <vector>

#include "server_test_helpers.hpp"

// The logs of the HTTP messages never show what may be a credential: bodies, WebSocket
// messages and header values are left out by default, and the credential headers are
// redacted even when the bodies are logged (an opt-in)

using namespace thinger;
using server_test::running_server;

namespace {

    const std::string token = "token-c1d2e3f4";
    const std::string request_cookie = "request-cookie-a9b8c7";
    const std::string response_cookie = "response-cookie-5f6e7d";
    const std::string request_body = "request-body-password=hunter2-and-more-request-body-text";
    const std::string response_body = "response-body-BEGIN-PRIVATE-KEY-and-more-response-body-text";
    const std::string ws_payload = "websocket-payload-secret-4a5b6c";

    // Every message of the library logger, at trace level, while alive
    struct log_capture {
        std::shared_ptr<spdlog::logger> previous = logging::get_logger();
        std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink =
            std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(4096);

        log_capture() {
            auto logger = std::make_shared<spdlog::logger>("http", sink);
            logger->set_pattern("%v");
            logger->set_level(spdlog::level::trace);
            logging::set_logger(logger);
        }
        ~log_capture() { logging::set_logger(previous); }

        std::vector<std::string> lines() const { return sink->last_formatted(); }

        bool contains(const std::string& text) const {
            auto all = lines();
            return std::any_of(all.begin(), all.end(),
                               [&](const std::string& line) { return line.find(text) != std::string::npos; });
        }

        std::string dump() const {
            std::string out;
            for (const auto& line : lines()) out += line;
            return out;
        }
    };

    void routes(http::server& server) {
        server.post("/login", [](http::request& req, http::response& res) {
            res.header("Set-Cookie", "session=" + response_cookie + "; HttpOnly");
            res.header("X-Visible", "visible-value");
            res.send(req.body() == request_body ? response_body : std::string("unexpected body"));
        });
        server.get("/data", [](http::request&, http::response& res) { res.send("ok"); });
        server.get("/echo", [](http::request&, http::response& res) {
            res.upgrade_websocket([](std::shared_ptr<http::websocket_connection> ws) {
                ws->on_message([ws](std::string message, bool binary) {
                    if (binary) ws->send_binary(std::move(message));
                    else ws->send_text(std::move(message));
                });
            });
        });
    }

    http::client_response login(http::client& client, uint16_t port) {
        return client.request("http://127.0.0.1:" + std::to_string(port) + "/login")
            .header("Authorization", "Bearer " + token)
            .header("Cookie", "id=" + request_cookie)
            .body(request_body, "text/plain")
            .post();
    }

    void require_no_secret(const log_capture& logs) {
        INFO("captured logs:\n" << logs.dump());
        for (const auto& line : logs.lines()) {
            REQUIRE(line.find(token) == std::string::npos);
            REQUIRE(line.find(request_cookie) == std::string::npos);
            REQUIRE(line.find(response_cookie) == std::string::npos);
        }
    }

}

TEST_CASE("HTTP logs show no body, header value or credential by default", "[logging][integration]") {
    http::server server;
    routes(server);
    running_server running(server);
    log_capture logs;

    http::client client;
    auto res = login(client, running.port);
    REQUIRE(res.status() == 200);
    REQUIRE(res.body() == response_body);

    INFO("captured logs:\n" << logs.dump());
    require_no_secret(logs);
    for (const auto& line : logs.lines()) {
        REQUIRE(line.find("request-body") == std::string::npos);
        REQUIRE(line.find("response-body") == std::string::npos);
        REQUIRE(line.find("visible-value") == std::string::npos);
    }

    // The request lines, the status, the header names and the body sizes are logged
    REQUIRE(logs.contains("[CLIENT->] POST /login"));
    REQUIRE(logs.contains("[SERVER REQUEST] POST /login"));
    REQUIRE(logs.contains("[SERVER RESPONSE] HTTP/1.1 200 OK"));
    REQUIRE(logs.contains("[HTTP CLIENT RESPONSE] HTTP/1.1 200 OK"));
    REQUIRE(logs.contains("  Authorization"));
    REQUIRE(logs.contains("  Set-Cookie"));
    REQUIRE(logs.contains("Body: " + std::to_string(request_body.size()) + " bytes"));
    REQUIRE(logs.contains("Body: " + std::to_string(response_body.size()) + " bytes"));
}

TEST_CASE("WebSocket messages are not logged, only their size", "[logging][websocket][integration]") {
    http::server server;
    routes(server);
    running_server running(server);
    log_capture logs;

    http::client client;
    auto ws = client.websocket("ws://127.0.0.1:" + std::to_string(running.port) + "/echo");
    REQUIRE(ws);
    REQUIRE(ws->send_text(ws_payload));
    auto [received, binary] = ws->receive();
    REQUIRE_FALSE(binary);
    REQUIRE(received == ws_payload);
    ws->close();

    INFO("captured logs:\n" << logs.dump());
    for (const auto& line : logs.lines()) {
        REQUIRE(line.find(ws_payload) == std::string::npos);
        REQUIRE(line.find("websocket-payload") == std::string::npos);
    }
    REQUIRE(logs.contains("websocket text message received: " + std::to_string(ws_payload.size()) + " bytes"));
}

TEST_CASE("Logged bodies are truncated and credential headers stay redacted", "[logging][integration]") {
    constexpr size_t max_length = 16;
    http::server server;
    routes(server);
    server.log_bodies(true, max_length);
    REQUIRE(server.get_body_logging().enabled);
    running_server running(server);
    log_capture logs;

    http::client client;
    client.log_bodies(true, max_length);
    auto res = login(client, running.port);
    REQUIRE(res.status() == 200);
    REQUIRE(res.body() == response_body);

    INFO("captured logs:\n" << logs.dump());
    require_no_secret(logs);

    // The start of each body, and how much is left out
    REQUIRE(logs.contains("  " + request_body.substr(0, max_length) + "... (" +
                          std::to_string(request_body.size() - max_length) + " more bytes)"));
    REQUIRE(logs.contains("  " + response_body.substr(0, max_length) + "... (" +
                          std::to_string(response_body.size() - max_length) + " more bytes)"));
    for (const auto& line : logs.lines()) {
        REQUIRE(line.find(request_body.substr(0, max_length + 1)) == std::string::npos);
        REQUIRE(line.find(response_body.substr(0, max_length + 1)) == std::string::npos);
    }

    // Header values, but those of the credential headers
    REQUIRE(logs.contains("  X-Visible: visible-value"));
    REQUIRE(logs.contains("  Authorization: [redacted]"));
    REQUIRE(logs.contains("  Cookie: [redacted]"));
    REQUIRE(logs.contains("  Set-Cookie: [redacted]"));
}

TEST_CASE("Logged bodies cannot forge log lines", "[logging][unit]") {
    log_capture logs;
    http::http_request request;
    request.set_method(http::method::POST);
    request.set_uri("/forge");
    request.add_header("Proxy-Authorization", "Basic " + token);
    request.set_content("line\r\nforged line\x01");

    request.log("TEST", http::body_logging{true, 1024});
    INFO("captured logs:\n" << logs.dump());
    require_no_secret(logs);
    REQUIRE(logs.contains("  Proxy-Authorization: [redacted]"));
    REQUIRE(logs.contains("  line\\r\\nforged line\\x01"));

    // Without the opt-in: the size alone
    log_capture quiet;
    request.log("TEST");
    INFO("captured logs:\n" << quiet.dump());
    REQUIRE(quiet.contains("Body: 18 bytes"));
    REQUIRE_FALSE(quiet.contains("forged"));
}

TEST_CASE("Credentials in the query string are redacted from the request lines", "[logging][integration]") {
    http::server server;
    routes(server);
    running_server running(server);
    log_capture logs;

    http::client client;
    auto res = client.get("http://127.0.0.1:" + std::to_string(running.port) + "/data?authorization=" + token + "&count=10");
    REQUIRE(res.status() == 200);

    require_no_secret(logs);
    REQUIRE(logs.contains("[CLIENT->] GET /data?authorization=[redacted]&count=10"));
    REQUIRE(logs.contains("[SERVER REQUEST] GET /data?authorization=[redacted]&count=10"));
}

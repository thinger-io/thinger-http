#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/util/types.hpp>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <thread>

using namespace thinger;
using namespace std::chrono_literals;

namespace {

    std::shared_ptr<http::http_request> make_request(http::method method, const std::string& path,
                                                     const std::string& body = "") {
        auto request = http::http_request::create_http_request(method, "http://localhost" + path);
        if (!body.empty()) request->set_content(body, "application/json");
        return request;
    }

    std::shared_ptr<http::http_response> run_dispatch(http::server& server, std::shared_ptr<http::http_request> request,
                                                      http::dispatch_options options = {}) {
        boost::asio::io_context ioc;
        auto result = co_spawn(ioc, server.dispatch(std::move(request), std::move(options)), boost::asio::use_future);
        ioc.run();
        return result.get();
    }

    // Routes producing every kind of error
    void add_routes(http::server& server) {
        server.use([](http::request& req, http::response& res) -> awaitable<bool> {
            if (req.header("X-Fail") == "throw") throw std::runtime_error("middleware failure");
            if (req.header("X-Fail") == "stop") co_return false;
            co_return true;
        });

        server.get("/forbidden", [](http::response& res) {
            res.error(http::http_response::status::forbidden, "not yours");
        });
        server.post("/devices", [](nlohmann::json& body, http::response& res) {
            res.json(body, http::http_response::status::created);
        }).schema({{"type", "object"}, {"required", {"name"}},
                   {"properties", {{"name", {{"type", "string"}}}}}});
        server.get("/throws", [](http::response& res) -> awaitable<void> {
            throw std::runtime_error("handler failure");
            co_return;
        });
        server.get("/ws", [](http::request& req, http::response& res) {
            res.upgrade_websocket([](std::shared_ptr<http::websocket_connection>) {});
        });
        server.get("/slow", [](http::response& res) -> awaitable<void> {
            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 5s);
            co_await timer.async_wait(use_nothrow_awaitable);
            res.send("late");
        });
        server.set_basic_auth("/private", "test", "user", "pass");
        server.get("/private", [](http::response& res) { res.send("secret"); });
        server.set_max_body_size(16);
    }

    nlohmann::json json_body(const std::shared_ptr<http::http_response>& response) {
        REQUIRE(response->get_header("Content-Type") == "application/json");
        return nlohmann::json::parse(response->get_content());
    }

}

TEST_CASE("Default error format", "[server][errors][integration]") {
    http::server server;
    add_routes(server);

    SECTION("response::error() sends the message as text/plain") {
        auto response = run_dispatch(server, make_request(http::method::GET, "/forbidden"));
        REQUIRE(response->get_status_code() == 403);
        REQUIRE(response->get_header("Content-Type") == "text/plain");
        REQUIRE(response->get_content() == "not yours");
    }

    SECTION("404 and 405 have an empty body") {
        for (auto [method, status] : {std::pair{http::method::GET, 404}, std::pair{http::method::PATCH, 405}}) {
            auto response = run_dispatch(server, make_request(method, "/missing"));
            REQUIRE(response->get_status_code() == status);
            REQUIRE(response->get_content().empty());
            REQUIRE(response->get_header("Content-Length") == "0");
        }
    }

    SECTION("Invalid JSON is text, schema validation errors are JSON") {
        auto invalid = run_dispatch(server, make_request(http::method::POST, "/devices", "{not json"));
        REQUIRE(invalid->get_status_code() == 400);
        REQUIRE(invalid->get_content() == "Invalid JSON");

        auto rejected = run_dispatch(server, make_request(http::method::POST, "/devices", R"({"name":1})"));
        REQUIRE(rejected->get_status_code() == 400);
        auto body = json_body(rejected);
        REQUIRE(body["error"]["message"].is_string());
        REQUIRE(body["error"]["context"].is_array());
        REQUIRE(body["error"].size() == 2);
    }

    SECTION("413 and 401") {
        auto large = run_dispatch(server, make_request(http::method::POST, "/devices", R"({"name":"a very long name"})"));
        REQUIRE(large->get_status_code() == 413);
        REQUIRE(large->get_content() == "Payload Too Large");

        auto unauthorized = run_dispatch(server, make_request(http::method::GET, "/private"));
        REQUIRE(unauthorized->get_status_code() == 401);
        REQUIRE(unauthorized->get_content() == "Authentication required");
        REQUIRE(unauthorized->get_header("WWW-Authenticate") == "Basic realm=\"test\"");
    }
}

TEST_CASE("Custom error format", "[server][errors][integration]") {
    http::server server;
    add_routes(server);

    std::vector<http::http_error> seen;
    server.set_error_formatter([&seen](const http::http_error& error, http::http_response& response) {
        seen.push_back(error);
        nlohmann::json body = {{"error", {
            {"message", error.message.empty() ? http::http_response::get_reason_phrase(error.status) : error.message},
            {"status", static_cast<int>(error.status)}}}};
        if (!error.details.is_null()) body["error"]["details"] = error.details;
        response.set_content(body.dump(), "application/json");
    });

    auto expect_error = [&](std::shared_ptr<http::http_request> request, int status, const std::string& message,
                            http::dispatch_options options = {}) {
        auto response = run_dispatch(server, std::move(request), std::move(options));
        REQUIRE(response->get_status_code() == status);
        auto body = json_body(response);
        REQUIRE(body["error"]["status"] == status);
        REQUIRE(body["error"]["message"] == message);
        return body;
    };

    SECTION("response::error()") {
        expect_error(make_request(http::method::GET, "/forbidden"), 403, "not yours");
    }

    SECTION("404 and 405") {
        expect_error(make_request(http::method::GET, "/missing"), 404, "Not Found");
        expect_error(make_request(http::method::PATCH, "/missing"), 405, "Method Not Allowed");
    }

    SECTION("400 invalid JSON") {
        expect_error(make_request(http::method::POST, "/devices", "{not json"), 400, "Invalid JSON");
    }

    SECTION("400 schema validation, with the details") {
        auto response = run_dispatch(server, make_request(http::method::POST, "/devices", R"({"name":1})"));
        REQUIRE(response->get_status_code() == 400);
        auto body = json_body(response);
        REQUIRE(body["error"]["status"] == 400);
        REQUIRE(body["error"]["details"]["context"].is_array());
        REQUIRE_FALSE(seen.empty());
        REQUIRE(seen.back().details.contains("context"));
    }

    SECTION("413") {
        expect_error(make_request(http::method::POST, "/devices", R"({"name":"a very long name"})"), 413, "Payload Too Large");
    }

    SECTION("500 from middlewares and handlers") {
        auto throwing = make_request(http::method::GET, "/forbidden");
        throwing->add_header("X-Fail", "throw");
        expect_error(throwing, 500, "Internal Server Error");

        auto stopping = make_request(http::method::GET, "/forbidden");
        stopping->add_header("X-Fail", "stop");
        expect_error(stopping, 500, "Internal Server Error");

        expect_error(make_request(http::method::GET, "/throws"), 500, "Internal Server Error");
    }

    SECTION("501 and 504 of in-memory dispatch") {
        auto upgrade = make_request(http::method::GET, "/ws");
        upgrade->add_header("Upgrade", "websocket");
        upgrade->add_header("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
        expect_error(upgrade, 501, "WebSocket upgrade is not available for in-memory requests");

        expect_error(make_request(http::method::GET, "/slow"), 504, "No response within the timeout", {.timeout = 50ms});
    }

    SECTION("401 of basic auth keeps its headers") {
        auto response = run_dispatch(server, make_request(http::method::GET, "/private"));
        REQUIRE(response->get_status_code() == 401);
        REQUIRE(response->get_header("WWW-Authenticate") == "Basic realm=\"test\"");
        REQUIRE(json_body(response)["error"]["message"] == "Authentication required");
    }

    SECTION("Errors over a connection") {
        REQUIRE(server.listen("127.0.0.1", 0));
        std::thread thread([&server]() { server.wait(); });

        http::client client;
        auto base = "http://127.0.0.1:" + std::to_string(server.local_port());

        auto missing = client.get(base + "/missing");
        REQUIRE(missing.status() == 404);
        REQUIRE(nlohmann::json::parse(missing.body())["error"]["message"] == "Not Found");

        auto large = client.post(base + "/devices", R"({"name":"a very long name"})", "application/json");
        REQUIRE(large.status() == 413);
        REQUIRE(nlohmann::json::parse(large.body())["error"]["message"] == "Payload Too Large");

        server.stop();
        thread.join();
    }

    SECTION("A formatter that throws falls back to the default format") {
        server.set_error_formatter([](const http::http_error&, http::http_response&) {
            throw std::runtime_error("broken formatter");
        });
        auto response = run_dispatch(server, make_request(http::method::GET, "/forbidden"));
        REQUIRE(response->get_status_code() == 403);
        REQUIRE(response->get_content() == "not yours");
    }
}

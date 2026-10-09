#include "server_test_helpers.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/util/types.hpp>
#include <atomic>
#include <chrono>

using namespace thinger;
using namespace std::chrono_literals;
using namespace server_test;

// Route registration and responses: invalid schemas fail closed, coroutine handlers run
// on every registration method, route references stay valid, and a response is sent once

TEST_CASE("Routes with an invalid schema reject their requests", "[server][schema][integration]") {
    http::server server;
    std::atomic<int> handler_calls{0};
    server.post("/broken", [&](nlohmann::json& body, http::response& res) {
        handler_calls++;
        res.json(body);
    }).schema({{"$ref", "#/components/schemas/Missing"}});

    server.schema_component("Thing", {{"type", "object"}, {"required", {"name"}}});
    server.post("/valid", [&](nlohmann::json& body, http::response& res) {
        res.json(body);
    }).schema({{"$ref", "#/components/schemas/Thing"}});

    auto broken = run_dispatch(server, make_request(http::method::POST, "/broken", R"({"name":"x"})"));
    REQUIRE(broken->get_status() == http::http_response::status::internal_server_error);
    REQUIRE(handler_calls == 0);

    auto valid = run_dispatch(server, make_request(http::method::POST, "/valid", R"({"name":"x"})"));
    REQUIRE(valid->get_status() == http::http_response::status::ok);
    auto invalid = run_dispatch(server, make_request(http::method::POST, "/valid", R"({"other":"x"})"));
    REQUIRE(invalid->get_status() == http::http_response::status::bad_request);
}

TEST_CASE("Route references stay valid while more routes are registered", "[server][routes][integration]") {
    http::server server;
    auto& first = server.get("/first", [](http::response& res) { res.send("first"); });
    for (int i = 0; i < 1000; i++) {
        server.get("/route/" + std::to_string(i), [](http::response& res) { res.send("other"); });
    }

    // Still the route stored by the router
    const auto& routes = server.router().get_routes().at(http::method::GET);
    REQUIRE(&first == &routes.front());

    // Configuring it later affects the served route
    first.summary("First route").meta("tag", "kept");
    REQUIRE(routes.front().get_summary() == "First route");

    server.use([](http::request& req, http::response& res) -> thinger::awaitable<bool> {
        if (req.get_matched_route() && req.get_matched_route()->has_meta("tag")) {
            res.header("X-Meta", req.get_matched_route()->get_meta("tag").get<std::string>());
        }
        co_return true;
    });
    auto result = run_dispatch(server, make_request(http::method::GET, "/first"));
    REQUIRE(result->get_content() == "first");
    REQUIRE(result->get_header("X-Meta") == "kept");
}

TEST_CASE("Coroutine handlers run on every registration method", "[server][coroutine][integration]") {
    http::server server;
    http::dispatch_options options;
    options.timeout = 2s;

    server.head("/head", [](http::response& res) -> thinger::awaitable<void> {
        res.header("X-Head", "coro");
        res.send("");
        co_return;
    });
    server.head("/head-request", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
        res.header("X-Path", req.get_http_request()->get_path());
        res.send("");
        co_return;
    });
    server.options("/options", [](http::response& res) -> thinger::awaitable<void> {
        res.header("Allow", "GET, OPTIONS");
        res.send("");
        co_return;
    });
    server.options("/options-request", [](http::request&, http::response& res) -> thinger::awaitable<void> {
        res.send("options-request");
        co_return;
    });
    auto group = server.group("/group");
    group.head("/head", [](http::response& res) -> thinger::awaitable<void> {
        res.send("");
        co_return;
    });
    group.options("/options", [](http::request&, http::response& res) -> thinger::awaitable<void> {
        res.send("group-options");
        co_return;
    });

    SECTION("head") {
        auto result = run_dispatch(server, make_request(http::method::HEAD, "/head"), options);
        REQUIRE(result->get_status() == http::http_response::status::ok);
        REQUIRE(result->get_header("X-Head") == "coro");
        result = run_dispatch(server, make_request(http::method::HEAD, "/head-request"), options);
        REQUIRE(result->get_header("X-Path") == "/head-request");
    }

    SECTION("options") {
        auto result = run_dispatch(server, make_request(http::method::OPTIONS, "/options"), options);
        REQUIRE(result->get_status() == http::http_response::status::ok);
        REQUIRE(result->get_header("Allow") == "GET, OPTIONS");
        result = run_dispatch(server, make_request(http::method::OPTIONS, "/options-request"), options);
        REQUIRE(result->get_content() == "options-request");
    }

    SECTION("group head and options") {
        auto result = run_dispatch(server, make_request(http::method::HEAD, "/group/head"), options);
        REQUIRE(result->get_status() == http::http_response::status::ok);
        result = run_dispatch(server, make_request(http::method::OPTIONS, "/group/options"), options);
        REQUIRE(result->get_content() == "group-options");
    }

    SECTION("not found handler taking the response") {
        server.set_not_found_handler([](http::response& res) -> thinger::awaitable<void> {
            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 10ms);
            co_await timer.async_wait(thinger::use_awaitable);
            res.error(http::http_response::status::not_found, "coro-not-found");
        });
        auto result = run_dispatch(server, make_request(http::method::GET, "/missing"), options);
        REQUIRE(result->get_status() == http::http_response::status::not_found);
        REQUIRE(result->get_content() == "coro-not-found");
    }

    SECTION("not found handler taking the request") {
        server.set_not_found_handler([](http::request& req, http::response& res) -> thinger::awaitable<void> {
            res.send("missing " + req.get_http_request()->get_path());
            co_return;
        });
        auto result = run_dispatch(server, make_request(http::method::GET, "/missing"), options);
        REQUIRE(result->get_content() == "missing /missing");
    }

    SECTION("synchronous not found handlers still work") {
        server.set_not_found_handler([](http::response& res) { res.send("sync-not-found"); });
        auto result = run_dispatch(server, make_request(http::method::GET, "/missing"), options);
        REQUIRE(result->get_content() == "sync-not-found");
    }
}

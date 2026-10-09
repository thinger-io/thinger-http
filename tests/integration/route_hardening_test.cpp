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

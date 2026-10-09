#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/routing/route.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>

using namespace thinger::http;

// ============================================================================
// Route pattern matching and parameter extraction
// ============================================================================

TEST_CASE("Route simple pattern matching", "[route][unit]") {

    SECTION("Exact path matches") {
        route r("/api/status");
        std::smatch m;
        std::string path1 = "/api/status";
        std::string path2 = "/api/other";
        REQUIRE(r.matches(path1, m));
        REQUIRE_FALSE(r.matches(path2, m));
    }

    SECTION("Simple parameter extraction") {
        route r("/users/:name");
        std::smatch m;
        std::string path = "/users/alice";
        REQUIRE(r.matches(path, m));
        REQUIRE(m.size() == 2);
        REQUIRE(m[1].str() == "alice");

        auto& params = r.get_parameters();
        REQUIRE(params.size() == 1);
        REQUIRE(params[0] == "name");
    }

    SECTION("Multiple simple parameters") {
        route r("/users/:user/devices/:device");
        std::smatch m;
        std::string path = "/users/alice/devices/sensor1";
        REQUIRE(r.matches(path, m));
        REQUIRE(m[1].str() == "alice");
        REQUIRE(m[2].str() == "sensor1");

        auto& params = r.get_parameters();
        REQUIRE(params.size() == 2);
        REQUIRE(params[0] == "user");
        REQUIRE(params[1] == "device");
    }

    SECTION("Simple param does not match slashes") {
        route r("/users/:name");
        std::smatch m;
        std::string path = "/users/alice/extra";
        REQUIRE_FALSE(r.matches(path, m));
    }
}

TEST_CASE("Route custom regex parameter matching", "[route][unit]") {

    SECTION("Numeric-only parameter") {
        route r("/users/:id([0-9]+)");
        std::smatch m;

        std::string path1 = "/users/123";
        REQUIRE(r.matches(path1, m));
        REQUIRE(m[1].str() == "123");

        std::string path2 = "/users/abc";
        REQUIRE_FALSE(r.matches(path2, m));
        std::string path3 = "/users/12abc";
        REQUIRE_FALSE(r.matches(path3, m));

        // First parameter name is the custom regex one
        auto& params = r.get_parameters();
        REQUIRE(params[0] == "id");
    }

    SECTION("Alphanumeric parameter with length limit") {
        route r("/users/:slug([a-z0-9-]{1,10})");
        std::smatch m;

        std::string path1 = "/users/hello-123";
        REQUIRE(r.matches(path1, m));
        REQUIRE(m[1].str() == "hello-123");

        std::string path2 = "/users/this-slug-is-too-long-for-the-pattern";
        REQUIRE_FALSE(r.matches(path2, m));
    }

    SECTION("Multiple custom regex parameters") {
        route r("/api/:version([0-9]+)/:resource([a-z]+)");
        std::smatch m;

        std::string path1 = "/api/2/users";
        REQUIRE(r.matches(path1, m));
        REQUIRE(m[1].str() == "2");
        REQUIRE(m[2].str() == "users");

        std::string path2 = "/api/v2/users";
        REQUIRE_FALSE(r.matches(path2, m));

        // First two parameter names are the custom regex ones
        auto& params = r.get_parameters();
        REQUIRE(params[0] == "version");
        REQUIRE(params[1] == "resource");
    }

    SECTION("Path-matching regex with slashes") {
        route r("/files/:path(.+)");
        std::smatch m;

        std::string path = "/files/dir/subdir/file.txt";
        REQUIRE(r.matches(path, m));
        REQUIRE(m[1].str() == "dir/subdir/file.txt");
    }
}

// ============================================================================
// Route configuration setters
// ============================================================================

TEST_CASE("Route configuration", "[route][unit]") {

    SECTION("deferred_body default is false") {
        route r("/test");
        REQUIRE_FALSE(r.is_deferred_body());
    }

    SECTION("deferred_body setter") {
        route r("/test");
        auto& ref = r.deferred_body(true);
        REQUIRE(r.is_deferred_body());
        REQUIRE(&ref == &r); // returns self for chaining
    }

    SECTION("description setter returns self") {
        route r("/test");
        auto& ref = r.description("A test route");
        REQUIRE(&ref == &r);
    }

    SECTION("get_pattern returns original pattern") {
        route r("/users/:id([0-9]+)");
        REQUIRE(r.get_pattern() == "/users/:id([0-9]+)");
    }
}

// ============================================================================
// Route callback assignment
// ============================================================================

TEST_CASE("Route callback assignment", "[route][unit]") {

    SECTION("Assign response-only callback") {
        route r("/test");
        r = route_callback_response_only([](response&) {});
    }

    SECTION("Assign json+response callback") {
        route r("/test");
        r = route_callback_json_response([](nlohmann::json&, response&) {});
    }

    SECTION("Assign request+response callback") {
        route r("/test");
        r = route_callback_request_response([](request&, response&) {});
    }

    SECTION("Assign request+json+response callback") {
        route r("/test");
        r = route_callback_request_json_response([](request&, nlohmann::json&, response&) {});
    }

    SECTION("Assign awaitable callback enables deferred_body") {
        route r("/test");
        REQUIRE_FALSE(r.is_deferred_body());
        r = route_callback_awaitable([](request&, response&) -> thinger::awaitable<void> {
            co_return;
        });
        REQUIRE(r.is_deferred_body());
    }
}

// ============================================================================
// Route handle_request dispatch (using minimal request/response)
// ============================================================================

namespace {
    // Helper: create a minimal request with optional body
    auto make_request(const std::string& body = "") {
        auto http_req = std::make_shared<http_request>();
        if (!body.empty()) {
            http_req->set_content(body, "application/json");
        }
        return std::make_shared<request>(nullptr, nullptr, http_req);
    }

    // Helper: create a minimal response (sends nowhere, but exercises dispatch logic)
    auto make_response(const std::shared_ptr<http_request>& http_req) {
        return response(nullptr, nullptr, http_req);
    }

    // Helper: run the route handler to completion
    void handle(const route& r, request& req, response& res) {
        boost::asio::io_context ioc;
        thinger::co_spawn(ioc, r.handle_request_coro(req, res), [](std::exception_ptr e) {
            if (e) std::rethrow_exception(e);
        });
        ioc.run();
    }
}

TEST_CASE("Route handle_request dispatch", "[route][unit]") {

    SECTION("Response-only callback is invoked") {
        route r("/test");
        bool called = false;
        r = route_callback_response_only([&called](response&) {
            called = true;
        });

        auto req = make_request();
        auto res = make_response(req->get_http_request());
        handle(r, *req, res);
        REQUIRE(called);
    }

    SECTION("Request+response callback is invoked") {
        route r("/test");
        bool called = false;
        r = route_callback_request_response([&called](request&, response&) {
            called = true;
        });

        auto req = make_request();
        auto res = make_response(req->get_http_request());
        handle(r, *req, res);
        REQUIRE(called);
    }

    SECTION("JSON+response callback with valid body") {
        route r("/test");
        std::string received_key;
        r = route_callback_json_response([&received_key](nlohmann::json& json, response&) {
            received_key = json.value("key", "");
        });

        auto req = make_request(R"({"key":"value"})");
        auto res = make_response(req->get_http_request());
        handle(r, *req, res);
        REQUIRE(received_key == "value");
    }

    SECTION("JSON+response callback with empty body gets empty json") {
        route r("/test");
        bool called = false;
        r = route_callback_json_response([&called](nlohmann::json& json, response&) {
            called = true;
        });

        auto req = make_request();
        auto res = make_response(req->get_http_request());
        handle(r, *req, res);
        REQUIRE(called);
    }

    SECTION("Request+JSON+response callback with valid body") {
        route r("/test");
        std::string received_value;
        r = route_callback_request_json_response([&received_value](request&, nlohmann::json& json, response&) {
            received_value = json.value("data", "");
        });

        auto req = make_request(R"({"data":"hello"})");
        auto res = make_response(req->get_http_request());
        handle(r, *req, res);
        REQUIRE(received_value == "hello");
    }

    SECTION("Request+JSON+response callback with empty body") {
        route r("/test");
        bool called = false;
        r = route_callback_request_json_response([&called](request&, nlohmann::json&, response&) {
            called = true;
        });

        auto req = make_request();
        auto res = make_response(req->get_http_request());
        handle(r, *req, res);
        REQUIRE(called);
    }

    SECTION("Request+JSON+response callback with invalid JSON") {
        route r("/test");
        bool called = false;
        r = route_callback_request_json_response([&called](request&, nlohmann::json&, response&) {
            called = true;
        });

        auto req = make_request("{invalid json}");
        auto res = make_response(req->get_http_request());
        handle(r, *req, res);
        REQUIRE_FALSE(called); // callback not invoked on invalid JSON
    }

    SECTION("Awaitable callback is awaited") {
        route r("/test");
        bool called = false;
        r = route_callback_awaitable([&called](request&, response&) -> thinger::awaitable<void> {
            called = true;
            co_return;
        });

        auto req = make_request();
        auto res = make_response(req->get_http_request());
        handle(r, *req, res);
        REQUIRE(called);
    }
}

TEST_CASE("Coroutine handler signatures", "[route][coroutine][unit]") {
    SECTION("JSON body handlers are never deferred") {
        route r("/test");
        r = [](nlohmann::json&, response&) -> thinger::awaitable<void> { co_return; };
        r.deferred_body(true);
        REQUIRE_FALSE(r.is_deferred_body());

        route r2("/test");
        r2 = [](request&, nlohmann::json&, response&) -> thinger::awaitable<void> { co_return; };
        REQUIRE_FALSE(r2.is_deferred_body());
    }

    SECTION("Plain coroutines are deferred unless disabled") {
        route r("/test");
        r = [](request&, response&) -> thinger::awaitable<void> { co_return; };
        REQUIRE(r.is_deferred_body());
        r.deferred_body(false);
        REQUIRE_FALSE(r.is_deferred_body());
    }

    SECTION("Generic two-argument lambdas are (request&, response&) handlers") {
        auto generic = [](auto&, auto&) -> thinger::awaitable<void> { co_return; };
        STATIC_REQUIRE(awaitable_handler<decltype(generic)>);
        STATIC_REQUIRE_FALSE(awaitable_json_handler<decltype(generic)>);
        route r("/test");
        r = generic;
        REQUIRE(r.is_deferred_body());
    }
}

TEST_CASE("Route documentation fields", "[route][docs][unit]") {
    route r("/v1/users/:user/devices/:device([a-z0-9_-]{1,32})");
    r.summary("Get device")
     .description("Returns a device of the user")
     .tag("Devices").tags({"Devices", "Users"})
     .operation_id("getDevice")
     .deprecated()
     .path_param("user", "User id")
     .path_param("device", "Device id")
     .query_param("fields", "Fields to return", {{"type", "string"}}, false)
     .returns(200, "The device", {{"type", "object"}}, {{"device", "d1"}})
     .returns(404, "Device not found")
     .example({{"name", "sensor"}})
     .schema({{"type", "object"}});

    REQUIRE(r.get_summary() == "Get device");
    REQUIRE(r.get_description() == "Returns a device of the user");
    REQUIRE(r.get_tags() == std::vector<std::string>{"Devices", "Users"});
    REQUIRE(r.get_operation_id() == "getDevice");
    REQUIRE(r.is_deprecated());

    REQUIRE(r.get_param_docs().size() == 3);
    REQUIRE(r.get_param_docs()[0].in == "path");
    REQUIRE(r.get_param_docs()[0].required);
    REQUIRE(r.get_param_docs()[2].in == "query");
    REQUIRE_FALSE(r.get_param_docs()[2].required);

    REQUIRE(r.get_parameter_pattern("device") == "[a-z0-9_-]{1,32}");
    REQUIRE(r.get_parameter_pattern("user").empty());

    REQUIRE(r.get_responses().size() == 2);
    REQUIRE(r.get_responses().at(200).example["device"] == "d1");
    REQUIRE(r.get_responses().at(404).schema.is_null());
    REQUIRE(r.get_examples().size() == 1);
    REQUIRE(r.get_schema()["type"] == "object");
}

TEST_CASE("Route free metadata", "[route][meta][unit]") {
    route r("/test");
    REQUIRE(r.get_metadata().empty());
    REQUIRE_FALSE(r.has_meta("permission"));
    REQUIRE(r.get_meta("permission").is_null());

    auto& ref = r.meta("permission", "Device:ReadDeviceConfig").meta("resource", {{"type", "device"}});
    REQUIRE(&ref == &r);
    REQUIRE(r.has_meta("permission"));
    REQUIRE(r.get_meta("permission") == "Device:ReadDeviceConfig");
    REQUIRE(r.get_meta("resource")["type"] == "device");

    r.meta("permission", "Device:Write");
    REQUIRE(r.get_meta("permission") == "Device:Write");
}

TEST_CASE("Route patterns with special characters and mixed parameters", "[route][unit]") {
    SECTION("Literal dots match only a dot") {
        route r("/openapi.json");
        std::smatch m;
        std::string ok = "/openapi.json", other = "/openapiXjson";
        REQUIRE(r.matches(ok, m));
        REQUIRE_FALSE(r.matches(other, m));
    }

    SECTION("Catch-all parameter matches any path") {
        route r(":path(.*)");
        std::smatch m;
        std::string a = "/any/path", b = "*";
        REQUIRE(r.matches(a, m));
        REQUIRE(r.matches(b, m));
    }

    SECTION("Parameters keep their order when regex and simple ones are mixed") {
        route r("/u/:user/d/:device([a-z]+)/r/:resource");
        REQUIRE(r.get_parameters() == std::vector<std::string>{"user", "device", "resource"});
        std::smatch m;
        std::string path = "/u/alice/d/xyz/r/temp";
        REQUIRE(r.matches(path, m));
        REQUIRE(m[1] == "alice");
        REQUIRE(m[2] == "xyz");
        REQUIRE(m[3] == "temp");
    }
}

TEST_CASE("Response-only coroutine handlers are coroutines", "[route][coroutine][unit]") {
    auto handler = [](response&) -> thinger::awaitable<void> { co_return; };
    STATIC_REQUIRE(awaitable_response_handler<decltype(handler)>);
    STATIC_REQUIRE(coroutine_handler<decltype(handler)>);
    route r("/test");
    r = handler;
    REQUIRE(r.is_deferred_body());
}

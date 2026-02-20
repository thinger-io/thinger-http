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

    SECTION("auth default is PUBLIC") {
        route r("/test");
        REQUIRE(r.get_auth_level() == auth_level::PUBLIC);
    }

    SECTION("auth setter") {
        route r("/test");
        auto& ref = r.auth(auth_level::ADMIN);
        REQUIRE(r.get_auth_level() == auth_level::ADMIN);
        REQUIRE(&ref == &r);
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
        r.handle_request(*req, res);
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
        r.handle_request(*req, res);
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
        r.handle_request(*req, res);
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
        r.handle_request(*req, res);
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
        r.handle_request(*req, res);
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
        r.handle_request(*req, res);
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
        r.handle_request(*req, res);
        REQUIRE_FALSE(called); // callback not invoked on invalid JSON
    }

    SECTION("Awaitable callback invoked synchronously returns error") {
        route r("/test");
        r = route_callback_awaitable([](request&, response&) -> thinger::awaitable<void> {
            co_return;
        });

        auto req = make_request();
        auto res = make_response(req->get_http_request());
        // Should not crash — sets 500 error on response
        REQUIRE_NOTHROW(r.handle_request(*req, res));
    }
}

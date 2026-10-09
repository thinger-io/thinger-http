#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/routing/route_handler.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/util/logger.hpp>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <algorithm>
#include <random>

using namespace thinger;
using namespace thinger::http;

// Route matching by path segments: priority, constraints, wildcards, and the warnings for
// duplicate and ambiguous routes

namespace {

    void noop(response&) {}

    struct lookup {
        const route* matched = nullptr;
        std::shared_ptr<request> req;

        std::string pattern() const { return matched ? matched->get_pattern() : "<none>"; }
        const std::string& operator[](const std::string& name) const { return (*req)[name]; }
    };

    lookup find(route_handler& router, const std::string& path, method m = method::GET) {
        auto http_request = http_request::create_http_request(m, "http://localhost" + path);
        auto req = std::make_shared<request>(nullptr, nullptr, http_request);
        auto* matched = router.find_route(req);
        return {matched, req};
    }

    // Library warnings logged while alive
    struct log_capture {
        std::shared_ptr<spdlog::logger> previous = logging::get_logger();
        std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(64);

        log_capture() {
            auto logger = std::make_shared<spdlog::logger>("route_tree_test", sink);
            logger->set_level(spdlog::level::warn);
            logging::set_logger(logger);
        }
        ~log_capture() { logging::set_logger(previous); }

        size_t count(const std::string& text) const {
            auto lines = sink->last_formatted();
            return std::count_if(lines.begin(), lines.end(),
                                 [&](const std::string& line) { return line.find(text) != std::string::npos; });
        }
        size_t warnings() const { return sink->last_formatted().size(); }
    };

}

TEST_CASE("Route priority does not depend on the registration order", "[route_tree][unit]") {
    std::vector<std::string> patterns = {
        "/files/new",
        "/files/:id([0-9]+)",
        "/files/:name",
        "/files/:path(.+)/raw",
        "/files/:path(.+)",
    };
    std::sort(patterns.begin(), patterns.end());
    do {
        route_handler router;
        for (const auto& pattern : patterns) router[method::GET][pattern] = noop;

        INFO("registration order: " << patterns[0] << ", " << patterns[1] << ", " << patterns[2] << ", "
                                    << patterns[3] << ", " << patterns[4]);
        // static, then constrained, then parameter, then wildcards
        REQUIRE(find(router, "/files/new").pattern() == "/files/new");
        REQUIRE(find(router, "/files/123").pattern() == "/files/:id([0-9]+)");
        REQUIRE(find(router, "/files/abc").pattern() == "/files/:name");
        REQUIRE(find(router, "/files/a/b").pattern() == "/files/:path(.+)");
        // a wildcard with more after it before a lone one
        REQUIRE(find(router, "/files/a/b/raw").pattern() == "/files/:path(.+)/raw");
        REQUIRE(find(router, "/files/a/b/raw")["path"] == "a/b");
    } while (std::next_permutation(patterns.begin(), patterns.end()));
}

TEST_CASE("Route matching falls back to lower priority options", "[route_tree][unit]") {
    route_handler router;
    router[method::GET]["/users/me/profile"] = noop;
    router[method::GET]["/users/:id([0-9]+)/settings"] = noop;
    router[method::GET]["/users/:user/devices"] = noop;
    router[method::GET]["/users/:path(.+)"] = noop;

    SECTION("A static segment whose subtree does not match falls back to the parameter") {
        auto result = find(router, "/users/me/devices");
        REQUIRE(result.pattern() == "/users/:user/devices");
        REQUIRE(result["user"] == "me");
    }

    SECTION("A constrained segment whose subtree does not match falls back to the parameter") {
        auto result = find(router, "/users/123/devices");
        REQUIRE(result.pattern() == "/users/:user/devices");
        REQUIRE(result["user"] == "123");
    }

    SECTION("Parameters captured on abandoned options are not set") {
        auto result = find(router, "/users/123/other");
        REQUIRE(result.pattern() == "/users/:path(.+)");
        REQUIRE(result["path"] == "123/other");
        REQUIRE_FALSE(result.req->has("id"));
        REQUIRE_FALSE(result.req->has("user"));
    }

    SECTION("Most specific match") {
        REQUIRE(find(router, "/users/me/profile").pattern() == "/users/me/profile");
        REQUIRE(find(router, "/users/7/settings").pattern() == "/users/:id([0-9]+)/settings");
    }
}

TEST_CASE("Route parameter constraints apply to their segment", "[route_tree][unit]") {
    route_handler router;

    SECTION("Numeric") {
        router[method::GET]["/users/:id([0-9]+)"] = noop;
        REQUIRE(find(router, "/users/123")["id"] == "123");
        REQUIRE(find(router, "/users/12a").matched == nullptr);
        REQUIRE(find(router, "/users/").matched == nullptr);
        REQUIRE(find(router, "/users/1/2").matched == nullptr);
    }

    SECTION("Length limits") {
        router[method::GET]["/devices/:device([a-zA-Z0-9_-]{1,32})/stats"] = noop;
        REQUIRE(find(router, "/devices/" + std::string(32, 'a') + "/stats").matched != nullptr);
        REQUIRE(find(router, "/devices/" + std::string(33, 'a') + "/stats").matched == nullptr);
        REQUIRE(find(router, "/devices/sensor.1/stats").matched == nullptr);
    }

    SECTION("UUID") {
        router[method::GET]["/items/:id(" UUID_PATTERN ")"] = noop;
        REQUIRE(find(router, "/items/123e4567-e89b-12d3-a456-426614174000")["id"] == "123e4567-e89b-12d3-a456-426614174000");
        REQUIRE(find(router, "/items/123e4567-e89b-12d3-a456-42661417400").matched == nullptr);
        REQUIRE(find(router, "/items/123e4567e89b12d3a456426614174000").matched == nullptr);
    }

    SECTION("Escapes and classes") {
        router[method::GET]["/v/:version(\\d+\\.\\d+)"] = noop;
        router[method::GET]["/w/:word(\\w{2,3})"] = noop;
        REQUIRE(find(router, "/v/1.25")["version"] == "1.25");
        REQUIRE(find(router, "/v/1x25").matched == nullptr);
        REQUIRE(find(router, "/w/a_1")["word"] == "a_1");
        REQUIRE(find(router, "/w/a").matched == nullptr);
        REQUIRE(find(router, "/w/a-1").matched == nullptr);
    }

    SECTION("Alternative words") {
        router[method::GET]["/profile/:category(properties|buckets)/:id"] = noop;
        auto result = find(router, "/profile/buckets/b1");
        REQUIRE(result["category"] == "buckets");
        REQUIRE(result["id"] == "b1");
        REQUIRE(find(router, "/profile/flows/b1").matched == nullptr);
    }

    SECTION("Other regexes (std::regex)") {
        router[method::GET]["/items/:id(\\d+|new)"] = noop;
        router[method::GET]["/items/:id(\\d+|new)/:part((?:a|b)+)"] = noop;
        REQUIRE(find(router, "/items/12")["id"] == "12");
        REQUIRE(find(router, "/items/new")["id"] == "new");
        REQUIRE(find(router, "/items/old").matched == nullptr);
        REQUIRE(find(router, "/items/new/abba")["part"] == "abba");
        REQUIRE(find(router, "/items/new/abc").matched == nullptr);
    }

    SECTION("Regexes depending on their context match the whole path") {
        router[method::GET]["/a/:x(foo$)/b"] = noop;
        router[method::GET]["/r/:a([a-z]+)/:b(\\1)"] = noop;
        router[method::GET]["/w/:word(\\bab)"] = noop;
        REQUIRE(find(router, "/a/foo/b").matched == nullptr);  // as the route regex: $ is not the end
        REQUIRE(find(router, "/r/xy/xy")["b"] == "xy");
        REQUIRE(find(router, "/r/xy/xz").matched == nullptr);
        REQUIRE(find(router, "/w/ab")["word"] == "ab");
    }

    SECTION("Regex with groups: the following parameters keep their names") {
        router[method::GET]["/posts/:slug(" SLUG_PATTERN ")/:page"] = noop;
        auto result = find(router, "/posts/hello-big-world/2");
        REQUIRE(result["slug"] == "hello-big-world");
        REQUIRE(result["page"] == "2");
        REQUIRE(find(router, "/posts/-hello/2").matched == nullptr);
    }

    SECTION("Text and parameters in a segment") {
        router[method::GET]["/files/:name.:ext"] = noop;
        router[method::GET]["/files/:name"] = noop;
        auto result = find(router, "/files/archive.tar.gz");
        REQUIRE(result.pattern() == "/files/:name.:ext");
        REQUIRE(result["name"] == "archive.tar");  // greedy, as the regex
        REQUIRE(result["ext"] == "gz");
        REQUIRE(find(router, "/files/archive").pattern() == "/files/:name");
    }

    SECTION("Parameter names are those of the matched route") {
        router[method::GET]["/a/:user([a-z]+)/x"] = noop;
        router[method::GET]["/a/:owner([a-z]+)/y"] = noop;
        REQUIRE(find(router, "/a/bob/x")["user"] == "bob");
        auto result = find(router, "/a/bob/y");
        REQUIRE(result["owner"] == "bob");
        REQUIRE_FALSE(result.req->has("user"));
    }
}

TEST_CASE("Route wildcards capture the rest of the path", "[route_tree][unit]") {
    route_handler router;
    router[method::GET]["/static/:path(.+)"] = noop;
    router[method::GET]["/optional/:path(.*)"] = noop;
    router[method::GET]["/storages/:storage/files:path(.*)"] = noop;

    REQUIRE(find(router, "/static/css/site.css")["path"] == "css/site.css");
    REQUIRE(find(router, "/static/a//b/")["path"] == "a//b/");
    REQUIRE(find(router, "/static/").matched == nullptr);
    REQUIRE(find(router, "/static").matched == nullptr);

    REQUIRE(find(router, "/optional/").matched != nullptr);
    REQUIRE(find(router, "/optional/")["path"].empty());
    REQUIRE(find(router, "/optional").matched == nullptr);

    // A wildcard sharing its segment with text
    auto files = find(router, "/storages/s1/files/dir/a.txt");
    REQUIRE(files["storage"] == "s1");
    REQUIRE(files["path"] == "/dir/a.txt");
    REQUIRE(find(router, "/storages/s1/files")["path"].empty());
    REQUIRE(find(router, "/storages/s1/filesystem")["path"] == "ystem");

    SECTION("A root catch-all is the last option") {
        router[method::GET][":path(.*)"] = noop;
        REQUIRE(find(router, "/static/x").pattern() == "/static/:path(.+)");
        REQUIRE(find(router, "/other/x")["path"] == "/other/x");
        REQUIRE(find(router, "/")["path"] == "/");
    }
}

TEST_CASE("Route trailing slashes and empty segments are significant", "[route_tree][unit]") {
    route_handler router;
    router[method::GET]["/users"] = noop;
    router[method::GET]["/devices/"] = noop;
    router[method::GET]["/users/:user/devices"] = noop;
    router[method::GET]["/"] = noop;

    REQUIRE(find(router, "/users").pattern() == "/users");
    REQUIRE(find(router, "/users/").matched == nullptr);
    REQUIRE(find(router, "/devices/").pattern() == "/devices/");
    REQUIRE(find(router, "/devices").matched == nullptr);
    REQUIRE(find(router, "/users//devices").matched == nullptr);
    REQUIRE(find(router, "//users").matched == nullptr);
    REQUIRE(find(router, "/").pattern() == "/");
    REQUIRE(find(router, "/users/u1/devices?limit=10")["user"] == "u1");
}

TEST_CASE("Route matching uses the path as received, before percent-decoding", "[route_tree][unit]") {
    route_handler router;
    router[method::GET]["/files/:name"] = noop;
    router[method::GET]["/hello%20world"] = noop;
    router[method::GET]["/a.b/:id"] = noop;

    REQUIRE(find(router, "/files/a%2Fb")["name"] == "a%2Fb");
    REQUIRE(find(router, "/files/a%20b")["name"] == "a%20b");
    REQUIRE(find(router, "/hello%20world").matched != nullptr);
    // Regex characters in static text are literal
    REQUIRE(find(router, "/a.b/1").matched != nullptr);
    REQUIRE(find(router, "/axb/1").matched == nullptr);
}

TEST_CASE("Routes by method", "[route_tree][unit]") {
    route_handler router;
    router[method::GET]["/items/:id"] = noop;
    router[method::POST]["/items"] = noop;

    REQUIRE(find(router, "/items/1", method::GET).matched != nullptr);
    REQUIRE(find(router, "/items/1", method::POST).matched == nullptr);
    REQUIRE(find(router, "/items", method::POST).matched != nullptr);
    REQUIRE(find(router, "/items", method::DELETE).matched == nullptr);
}

TEST_CASE("Duplicate and ambiguous routes are warned at registration", "[route_tree][unit]") {
    log_capture log;
    route_handler router;

    SECTION("The same structure: the first one registered wins") {
        router[method::GET]["/users/:id"] = noop;
        router[method::GET]["/users/:name"] = noop;
        router[method::GET]["/files/:a([0-9]+)"] = noop;
        router[method::GET]["/files/:b([0-9]+)"] = noop;
        router[method::GET]["/static/:path(.+)"] = noop;
        router[method::GET]["/static/:rest(.+)"] = noop;
        REQUIRE(log.count("will never match") == 3);
        REQUIRE(find(router, "/users/1").pattern() == "/users/:id");
        REQUIRE(find(router, "/files/1").pattern() == "/files/:a([0-9]+)");
        REQUIRE(find(router, "/static/x").pattern() == "/static/:path(.+)");
        // still registered, for the API documentation
        REQUIRE(router.get_routes().at(method::GET).size() == 6);
    }

    SECTION("The same path on another method is not a duplicate") {
        router[method::GET]["/users/:id"] = noop;
        router[method::PUT]["/users/:id"] = noop;
        REQUIRE(log.warnings() == 0);
    }

    SECTION("Overlapping constraints decided by registration order") {
        router[method::GET]["/users/:id([0-9]+)"] = noop;
        router[method::GET]["/users/:hex([0-9a-f]+)"] = noop;
        router[method::GET]["/kinds/:kind(sensor|actuator)"] = noop;
        router[method::GET]["/kinds/:name([a-z]+)"] = noop;
        REQUIRE(log.count("may match the same paths") == 2);
        REQUIRE(find(router, "/users/12").pattern() == "/users/:id([0-9]+)");
        REQUIRE(find(router, "/users/ab").pattern() == "/users/:hex([0-9a-f]+)");
    }

    SECTION("Different constraints are tried in the order they were first registered") {
        router[method::GET]["/items/:id([0-9]+)/a"] = noop;
        router[method::GET]["/items/:n([0-9]{1,3})/b"] = noop;
        router[method::GET]["/items/:id([0-9]+)/b"] = noop;
        // [0-9]+ was registered first at that position: its node goes first
        REQUIRE(find(router, "/items/12/b").pattern() == "/items/:id([0-9]+)/b");
        REQUIRE(log.count("/items/:id([0-9]+)/b and GET /items/:n([0-9]{1,3})/b may match the same paths: "
                          "the first one takes precedence") == 1);
    }

    SECTION("Overlapping wildcards decided by registration order") {
        router[method::GET]["/files/:path(.+)"] = noop;
        router[method::GET]["/files/:path(.*)"] = noop;
        REQUIRE(log.count("may match the same paths") == 1);
    }

    SECTION("Constraints that cannot overlap") {
        router[method::GET]["/users/:id([0-9]+)"] = noop;
        router[method::GET]["/users/:name([a-z]+)"] = noop;
        router[method::GET]["/items/:id([0-9]{1,3})/a"] = noop;
        router[method::GET]["/items/:code([0-9]{2,8})/b"] = noop;
        router[method::GET]["/kinds/:kind(sensor|actuator)"] = noop;
        router[method::GET]["/kinds/:id([0-9]+)"] = noop;
        REQUIRE(log.warnings() == 0);
    }

    SECTION("Priority decided by the kind of segment") {
        router[method::GET]["/users/:id"] = noop;
        router[method::GET]["/users/me"] = noop;
        router[method::GET]["/users/:id([0-9]+)"] = noop;
        router[method::GET]["/users/:path(.+)"] = noop;
        router[method::GET]["/users/:path(.+)/raw"] = noop;
        REQUIRE(log.warnings() == 0);
    }
}

TEST_CASE("Route tree finds the routes the route regexes match", "[route_tree][unit]") {
    // Matching by segments finds a route whenever some route regex matches the path, and
    // only routes whose regex matches it
    std::vector<std::string> patterns = {
        "/v1/users/:user([a-zA-Z0-9_-]{1,32})",
        "/v1/users/:user([a-zA-Z0-9_-]{1,32})/devices",
        "/v1/users/:user/devices/:device",
        "/v1/users/:user/devices/:device/resources/:resource",
        "/v1/users/:user([a-z]+)/devices/:device([0-9]+)/:resource(.+)",
        "/v1/users/me/devices",
        "/v1/users/:user/files:path(.*)",
        "/v1/:any(.+)/stats",
        "/v1/items/:name.:ext",
        "/v1/items/:id(\\d+|new)",
        "/v2/:a/:b/:c",
        "/v2/x/:b",
        "/",
    };
    route_handler router;
    for (const auto& pattern : patterns) router[method::GET][pattern] = noop;
    const auto& routes = router.get_routes().at(method::GET);

    const std::vector<std::string> segments = {
        "", "v1", "v2", "users", "me", "alice", "devices", "resources", "files", "filesx", "stats",
        "items", "x", "123", "a.b", "new", "temp", "d-1", "%2F",
    };
    std::mt19937 random(42);
    for (int i = 0; i < 20000; ++i) {
        std::string path;
        size_t count = 1 + random() % 7;
        for (size_t j = 0; j < count; ++j) path += "/" + segments[random() % segments.size()];

        auto result = find(router, path);
        std::smatch m;
        bool any = std::any_of(routes.begin(), routes.end(), [&](const route& r) { return r.matches(path, m); });
        INFO(path << " -> " << result.pattern());
        REQUIRE((result.matched != nullptr) == any);
        if (result.matched) REQUIRE(result.matched->matches(path, m));
    }
}

TEST_CASE("Invalid route patterns are rejected at registration", "[route_tree][unit]") {
    route_handler router;
    router[method::GET]["/ok"] = noop;
    REQUIRE_THROWS(router[method::GET]["/x/:a([a-z)"]);         // unbalanced parentheses
    REQUIRE_THROWS(router[method::GET]["/x/:a([a-z]{2,1})"]);   // invalid regex
    REQUIRE(router.get_routes().at(method::GET).size() == 1);
    REQUIRE(find(router, "/ok").matched != nullptr);
}

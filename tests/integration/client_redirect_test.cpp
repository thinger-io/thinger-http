#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/client/async_client.hpp>
#include <thinger/http/util/url.hpp>
#include "../fixtures/test_server_fixture.hpp"
#include "server_test_helpers.hpp"
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>
#include <thinger/util/logger.hpp>
#include <atomic>
#include <map>
#include <memory>
#include <string>

using namespace thinger;

// Test redirect configuration (works for both clients)
TEMPLATE_TEST_CASE("HTTP Client redirect configuration", "[http][client][redirect][config]",
                   http::client, http::async_client) {

    SECTION("Client has default redirect settings") {
        http::test::client_under_test<TestType> client;
        REQUIRE(client.get_follow_redirects() == true);
        REQUIRE(client.get_max_redirects() == 5); // Default max redirects
    }

    SECTION("Can configure max redirects") {
        http::test::client_under_test<TestType> client;
        client.max_redirects(10);
        REQUIRE(client.get_max_redirects() == 10);
    }

    SECTION("Can disable redirect following") {
        http::test::client_under_test<TestType> client;
        client.follow_redirects(false);
        REQUIRE(client.get_follow_redirects() == false);
    }

    SECTION("Fluent API for redirect configuration") {
        http::test::client_under_test<TestType> client;
        client.max_redirects(3).follow_redirects(true);
        REQUIRE(client.get_max_redirects() == 3);
        REQUIRE(client.get_follow_redirects() == true);
    }
}

// Test redirect handling with synchronous client
TEST_CASE("HTTP Sync Client redirect handling", "[http][client][redirect][sync]") {

    // Create test server fixture
    thinger::http::test::TestServerFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    SECTION("Client follows redirects by default") {
        http::client client;

        auto res = client.get(base_url + "/redirect/2");

        REQUIRE(res.ok());
        REQUIRE(res.status() == 200);
    }

    SECTION("Client respects max redirect limit") {
        http::client client;
        client.max_redirects(2);

        // This endpoint tries to redirect 5 times
        auto res = client.get(base_url + "/redirect/5");

        // After 2 redirects, we should get the 3rd redirect response (not followed)
        REQUIRE(!res.ok()); // Should NOT be ok because it's a redirect response
        REQUIRE(res.is_redirect());
    }

    SECTION("Client can disable redirect following") {
        http::client client;
        client.follow_redirects(false);

        auto res = client.get(base_url + "/redirect/1");

        // Should get the redirect response, not follow it
        REQUIRE(!res.ok());
        REQUIRE(res.is_redirect());
    }
}

// Test redirect handling with async_client (async callbacks)
TEST_CASE("HTTP Async Client redirect handling", "[http][client][redirect][async]") {
    thinger::http::test::running_workers workers;

    // Create test server fixture
    thinger::http::test::TestServerFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    SECTION("Client follows redirects by default") {
        http::async_client client(workers);
        std::promise<bool> promise;
        auto future = promise.get_future();

        client.get(base_url + "/redirect/2", [&promise](http::client_response& res) {
            promise.set_value(res.ok() && res.status() == 200);
        });

        client.wait();
        REQUIRE(future.get() == true);
    }

    SECTION("Client respects max redirect limit") {
        http::async_client client(workers);
        client.max_redirects(2);

        std::promise<bool> promise;
        auto future = promise.get_future();

        client.get(base_url + "/redirect/5", [&promise](http::client_response& res) {
            // After 2 redirects, we should get the 3rd redirect response (not followed)
            promise.set_value(!res.ok() && res.is_redirect());
        });

        client.wait();
        REQUIRE(future.get() == true);
    }

    SECTION("Client can disable redirect following") {
        http::async_client client(workers);
        client.follow_redirects(false);

        std::promise<bool> promise;
        auto future = promise.get_future();

        client.get(base_url + "/redirect/1", [&promise](http::client_response& res) {
            promise.set_value(!res.ok() && res.is_redirect());
        });

        client.wait();
        REQUIRE(future.get() == true);
    }

    SECTION("Multiple async_clients handle redirects independently") {
        http::async_client client1(workers);
        http::async_client client2(workers);

        client1.max_redirects(1);
        client2.max_redirects(5);

        std::promise<bool> promise1, promise2;
        auto future1 = promise1.get_future();
        auto future2 = promise2.get_future();

        client1.get(base_url + "/redirect/3", [&promise1](http::client_response& res) {
            // Client1 should stop after 1 redirect
            promise1.set_value(res.is_redirect());
        });

        client2.get(base_url + "/redirect/3", [&promise2](http::client_response& res) {
            // Client2 should follow all redirects
            promise2.set_value(res.ok());
        });

        client1.wait();
        client2.wait();

        REQUIRE(future1.get() == true);
        REQUIRE(future2.get() == true);
    }
}

// Relative and network-path Locations, Locations the client refuses, and the body of a 307/308
// hop. A peer answers a 3xx while its query has `code`, `to`, `long` or `dots`, and otherwise echoes
// what it received as JSON; it counts every request it answers
namespace {

using thinger::http::test::transport;
using thinger::http::util::url::url_encode;

// Locations with dot segments, by name
const std::map<std::string, std::string>& dot_locations() {
    static const std::map<std::string, std::string> locations{
        {"current", "./x"}, {"parent", "../x"}, {"above-root", "../../../../x"}, {"absolute", "/a/./b/../c"}};
    return locations;
}

struct redirect_peer {
    http::server server;
    std::atomic<int> hits{0};
    std::unique_ptr<server_test::running_server> running;

    explicit redirect_peer(transport kind = transport::tcp) {
        auto handler = [this](http::request& req, http::response& res) {
            hits++;
            auto& request = *req.get_http_request();
            bool has_to = request.has_uri_parameter("to");
            if (has_to || request.has_uri_parameter("code") || request.has_uri_parameter("long") ||
                request.has_uri_parameter("dots")) {
                res.status(static_cast<http::http_response::status>(std::stoi(req.query("code", "302"))));
                if (has_to) res.header("Location", req.query("to"));
                // The server refuses ".." anywhere in a request target, so these come from a table
                if (auto dots = req.query("dots"); !dots.empty()) res.header("Location", dot_locations().at(dots));
                // A Location of `long` bytes, longer than the client's URL cap
                if (auto size = req.query("long"); !size.empty()) res.header("Location", "/" + std::string(std::stoul(size) - 1, 'a'));
                res.send("");
                return;
            }
            const auto& uri = request.get_uri();
            auto query = uri.find('?');
            nlohmann::json echo;
            echo["method"] = http::get_method(request.get_method());
            echo["path"] = uri.substr(0, query);
            echo["query"] = query == std::string::npos ? "" : uri.substr(query + 1);
            echo["body"] = request.get_body();
            echo["content_length"] = request.get_headers_with_key("Content-Length");
            echo["content_type"] = request.get_headers_with_key("Content-Type");
            echo["authorization"] = request.get_headers_with_key("Authorization");
            res.json(echo);
        };
        server.get("/:path(.*)", handler);
        server.post("/:path(.*)", handler);
        server.put("/:path(.*)", handler);
        running = std::make_unique<server_test::running_server>(server, kind);
    }

    std::string url(const std::string& path) const { return running->url(path); }

    // A URL of this peer answering `code` with `location`
    std::string redirect(const std::string& path, const std::string& location, int code = 302) const {
        return url(path + (path.find('?') == std::string::npos ? "?" : "&") + "code=" + std::to_string(code) +
                   "&to=" + url_encode(location));
    }

    // The authority of this peer (host:port), for network-path Locations
    std::string authority() const { return running->endpoint().host_header(); }
};

nlohmann::json echo_of(const http::client_response& res) {
    INFO("status " << res.status() << ", error " << res.error_code().message() << ", body " << res.body());
    REQUIRE(res.ok());
    return nlohmann::json::parse(res.body());
}

} // namespace

TEST_CASE("Client resolves relative Locations against the request", "[http][client][redirect][relative]") {
    redirect_peer peer;
    http::client client;

    SECTION("a path-absolute Location keeps the scheme, host and port") {
        auto echo = echo_of(client.get(peer.redirect("/start", "/get")));
        REQUIRE(echo["path"] == "/get");
        REQUIRE(echo["method"] == "GET");
        REQUIRE(peer.hits.load() == 2);
    }

    SECTION("a path-absolute Location carries its query") {
        auto echo = echo_of(client.get(peer.redirect("/start", "/get?x=1")));
        REQUIRE(echo["path"] == "/get");
        REQUIRE(echo["query"] == "x=1");
    }

    SECTION("a path-relative Location merges with the directory of the current path") {
        REQUIRE(echo_of(client.get(peer.redirect("/rel/a/b", "login")))["path"] == "/rel/a/login");
        REQUIRE(echo_of(client.get(peer.url("/rel/a/b?dots=current")))["path"] == "/rel/a/x");
        REQUIRE(echo_of(client.get(peer.url("/rel/a/b?dots=parent")))["path"] == "/rel/x");
    }

    SECTION("a path that ends in / is the directory itself") {
        REQUIRE(echo_of(client.get(peer.redirect("/rel/dir/", "login")))["path"] == "/rel/dir/login");
    }

    SECTION("a query-only Location keeps the path and replaces the query") {
        auto echo = echo_of(client.get(peer.redirect("/rel/a/b?old=1", "?q=1")));
        REQUIRE(echo["path"] == "/rel/a/b");
        REQUIRE(echo["query"] == "q=1");
    }

    SECTION("dot segments are removed, and .. stops at the root") {
        REQUIRE(echo_of(client.get(peer.url("/rel/a/b?dots=above-root")))["path"] == "/x");
        REQUIRE(echo_of(client.get(peer.url("/rel/a/b?dots=absolute")))["path"] == "/a/c");
    }

    SECTION("a network-path Location goes to its host and port") {
        redirect_peer other;
        auto echo = echo_of(client.get(peer.redirect("/start", "//" + other.authority() + "/get?y=2")));
        REQUIRE(echo["path"] == "/get");
        REQUIRE(echo["query"] == "y=2");
        REQUIRE(peer.hits.load() == 1);
        REQUIRE(other.hits.load() == 1);
    }

    SECTION("a network-path Location with no path goes to the root of its host") {
        redirect_peer other;
        REQUIRE(echo_of(client.get(peer.redirect("/start", "//" + other.authority())))["path"] == "/");
        REQUIRE(other.hits.load() == 1);
    }

    SECTION("a chain mixing every form is followed, within max_redirects") {
        // absolute -> path-absolute -> network-path -> path-relative -> the target
        auto to_target = "/a/hop2?code=302&to=" + url_encode("target");
        auto network = "//" + peer.authority() + to_target;
        auto path_absolute = "/hop1?code=302&to=" + url_encode(network);
        auto absolute = peer.url("/hop0?code=302&to=" + url_encode(path_absolute));
        auto start = peer.redirect("/start", absolute);

        client.max_redirects(4);
        REQUIRE(echo_of(client.get(start))["path"] == "/a/target");
        REQUIRE(peer.hits.load() == 5);

        client.max_redirects(3);
        auto res = client.get(start);
        REQUIRE_FALSE(res.has_error());
        REQUIRE(res.is_redirect());
        REQUIRE(peer.hits.load() == 9);
    }

    SECTION("Authorization follows a relative Location only to the same origin") {
        http::headers_map authorization{{"Authorization", "Bearer secret"}};
        auto same = echo_of(client.get(peer.redirect("/start", "/target"), authorization));
        REQUIRE(same["authorization"] == nlohmann::json::array({"Bearer secret"}));
        auto relative = echo_of(client.get(peer.redirect("/rel/a/b", "target"), authorization));
        REQUIRE(relative["authorization"] == nlohmann::json::array({"Bearer secret"}));

        redirect_peer other;
        auto cross = echo_of(client.get(peer.redirect("/start", "//" + other.authority() + "/target"), authorization));
        REQUIRE(cross["authorization"].empty());
    }

    SECTION("a 307 to a relative Location sends the POST and its body again") {
        auto echo = echo_of(client.post(peer.redirect("/start", "/target", 307), R"({"a":1})"));
        REQUIRE(echo["method"] == "POST");
        REQUIRE(echo["body"] == R"({"a":1})");
    }
}

TEST_CASE("Client keeps the scheme of the request for a network-path Location", "[http][client][redirect][relative][tls]") {
    redirect_peer peer(transport::tls);
    http::client client;
    peer.running->configure(client);
    auto echo = echo_of(client.get(peer.redirect("/start", "//" + peer.authority() + "/get")));
    REQUIRE(echo["path"] == "/get");
    REQUIRE(peer.hits.load() == 2);
}

TEST_CASE("Async client resolves relative Locations", "[http][client][redirect][relative][async]") {
    redirect_peer peer;
    http::test::client_under_test<http::async_client> client;
    std::promise<http::client_response> result;
    client.get(peer.url("/rel/a/b?dots=parent"), [&](http::client_response& res) { result.set_value(res); });
    client.wait();
    REQUIRE(echo_of(result.get_future().get())["path"] == "/rel/x");
}

TEST_CASE("Client keeps the Unix socket for a Location on the same host", "[http][client][redirect][relative][unix]") {
    redirect_peer peer(transport::unix);
    const auto& socket = peer.running->endpoint().unix_path;

    SECTION("the client's socket") {
        http::client client;
        client.unix_socket(socket);
        REQUIRE(echo_of(client.get(peer.redirect("/start", "/other")))["path"] == "/other");
        REQUIRE(echo_of(client.get(peer.redirect("/rel/a/b", "other")))["path"] == "/rel/a/other");
        REQUIRE(peer.hits.load() == 4);
    }

    SECTION("the request's own socket") {
        http::client client;
        auto request = client.create_request(http::method::GET, peer.redirect("/rel/a/b", "other"));
        request->set_unix_socket(socket);
        REQUIRE(echo_of(client.send(request))["path"] == "/rel/a/other");
        REQUIRE(peer.hits.load() == 2);
    }
}

TEST_CASE("Client ends the chain at a Location it cannot parse", "[http][client][redirect][refused]") {
    redirect_peer peer;
    http::client client;

    auto require_refused = [&](const http::client_response& res) {
        INFO("status " << res.status() << ", error " << res.error_code().message());
        REQUIRE(res.has_error());
        REQUIRE(res.error_code() == boost::asio::error::invalid_argument);
        REQUIRE(res.status() == 302);
        REQUIRE(peer.hits.load() == 1);  // only the 3xx: nothing is sent after it
    };

    SECTION("another scheme") {
        auto location = GENERATE(as<std::string>{}, "ftp://example.com/x", "mailto:someone@example.com");
        INFO("Location: " << location);
        require_refused(client.get(peer.redirect("/start", location)));
    }

    SECTION("an invalid scheme") {
        require_refused(client.get(peer.redirect("/start", "ht!tp://x")));
    }

    SECTION("a host the parser refuses") {
        require_refused(client.get(peer.redirect("/start", "http://[::1]:1/x")));
    }

    SECTION("a Location over the URL cap") {
        auto res = client.get(peer.url("/start?long=9000"));
        require_refused(res);
        REQUIRE(res.header("Location").size() == 9000);
    }
}

TEST_CASE("Client returns a 3xx it does not follow as a response", "[http][client][redirect][refused]") {
    redirect_peer peer;
    http::client client;

    SECTION("an empty Location") {
        auto res = client.get(peer.redirect("/start", ""));
        REQUIRE_FALSE(res.has_error());
        REQUIRE(res.status() == 302);
        REQUIRE(peer.hits.load() == 1);
    }

    SECTION("no Location") {
        auto res = client.get(peer.url("/start?code=302"));
        REQUIRE_FALSE(res.has_error());
        REQUIRE(res.status() == 302);
        REQUIRE(peer.hits.load() == 1);
    }

    SECTION("redirects disabled, whatever the Location") {
        client.follow_redirects(false);
        auto location = GENERATE(as<std::string>{}, "/get", "ftp://example.com/x");
        INFO("Location: " << location);
        auto res = client.get(peer.redirect("/start", location, 301));
        REQUIRE_FALSE(res.has_error());
        REQUIRE(res.status() == 301);
        REQUIRE(res.header("Location") == location);
        REQUIRE(peer.hits.load() == 1);
    }
}

TEST_CASE("Client refuses a URL over the cap without sending it", "[http][client][redirect][refused]") {
    redirect_peer peer;
    http::client client;
    auto url = peer.url("/get?q=");
    url.append(9000 - url.size(), 'a');
    REQUIRE(url.size() == 9000);
    REQUIRE(client.get(url).has_error());
    REQUIRE(peer.hits.load() == 0);
}

TEST_CASE("Client sends one Content-Length on a 307 or 308", "[http][client][redirect][body]") {
    SECTION("the 307 of /redirect-307-to-post") {
        thinger::http::test::TestServerFixture fixture;
        http::client client;
        auto res = client.post(fixture.base_url + "/redirect-307-to-post", R"({"a":1})");
        REQUIRE(res.ok());
        auto echo = nlohmann::json::parse(res.body());
        REQUIRE(echo["method"] == "POST");
        REQUIRE(echo["body"] == R"({"a":1})");
        REQUIRE(echo["content_length_lines"] == nlohmann::json::array({"7"}));
    }

    SECTION("a 308 of a PUT or a POST") {
        redirect_peer peer;
        http::client client;
        std::string body = "<a>1</a>";
        auto method = GENERATE(as<std::string>{}, "PUT", "POST");
        INFO("method " << method);
        auto url = peer.redirect("/start", peer.url("/target"), 308);
        auto echo = echo_of(method == "PUT" ? client.put(url, body, "application/xml")
                                            : client.post(url, body, "application/xml"));
        REQUIRE(echo["method"] == method);
        REQUIRE(echo["body"] == body);
        REQUIRE(echo["content_length"] == nlohmann::json::array({std::to_string(body.size())}));
        REQUIRE(echo["content_type"] == nlohmann::json::array({"application/xml"}));
    }

    SECTION("a 303, 301 or 302 of a POST becomes a GET with no body") {
        redirect_peer peer;
        http::client client;
        auto code = GENERATE(303, 301, 302);
        INFO("status " << code);
        auto echo = echo_of(client.post(peer.redirect("/start", peer.url("/target"), code), R"({"a":1})"));
        REQUIRE(echo["method"] == "GET");
        REQUIRE(echo["body"] == "");
        REQUIRE(echo["content_length"].empty());
    }
}

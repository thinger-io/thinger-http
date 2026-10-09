#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/util/types.hpp>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

using namespace thinger;

namespace {

    // Runs a server on a random port until destroyed (routes are registered before start())
    struct running_server {
        http::server server;
        std::thread thread;
        uint16_t port = 0;

        void start() {
            REQUIRE(server.listen("127.0.0.1", 0));
            port = server.local_port();
            thread = std::thread([this]() { server.wait(); });
        }

        ~running_server() {
            server.stop();
            if (thread.joinable()) thread.join();
        }
    };

    struct raw_response {
        int status = 0;
        std::string headers;
        std::string body;
    };

    // Send a request with the given Host header over a new connection
    raw_response request_with_host(uint16_t port, const std::string& method, const std::string& path,
                                   const std::string& host, const std::string& body = "") {
        boost::asio::io_context ioc;
        boost::asio::ip::tcp::socket sock(ioc);
        sock.connect({boost::asio::ip::make_address("127.0.0.1"), port});

        std::string raw = method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n";
        if (!body.empty()) {
            raw += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
        }
        raw += "\r\n" + body;
        boost::asio::write(sock, boost::asio::buffer(raw));

        boost::system::error_code ec;
        boost::asio::streambuf buffer;
        boost::asio::read(sock, buffer, ec);
        std::string data(boost::asio::buffers_begin(buffer.data()), boost::asio::buffers_end(buffer.data()));

        raw_response result;
        auto end_headers = data.find("\r\n\r\n");
        REQUIRE(end_headers != std::string::npos);
        result.status = std::stoi(data.substr(9, 3));
        result.headers = data.substr(0, end_headers);
        result.body = data.substr(end_headers + 4);
        return result;
    }

    std::shared_ptr<http::http_response> run_dispatch(http::server& server, http::method method, const std::string& url) {
        boost::asio::io_context ioc;
        auto request = http::http_request::create_http_request(method, url);
        auto result = co_spawn(ioc, server.dispatch(request), boost::asio::use_future);
        ioc.run();
        return result.get();
    }

}

TEST_CASE("Virtual hosts dispatch by the Host header", "[server][vhost][integration]") {
    running_server s;
    auto& server = s.server;

    server.get("/", [](http::response& res) { res.send("default"); });
    server.get("/only-default", [](http::response& res) { res.send("default-only"); });
    server.post("/items", [](http::response& res) { res.send("default-post"); });

    auto& api = server.host("api.example.com");
    api.get("/", [](http::response& res) { res.send("api"); });
    api.get("/items/:id", [](http::request& req, http::response& res) { res.send("api-item-" + req["id"]); });

    server.host("www.example.com").get("/", [](http::response& res) { res.send("www"); });

    // Exact host beats any pattern, and patterns are tried in registration order
    server.host("admin.example.com").get("/", [](http::response& res) { res.send("admin"); });
    server.host(":tenant.example.com").get("/", [](http::request& req, http::response& res) {
        res.json({{"tenant", req["tenant"]}, {"matches", req.get_host_matches()}});
    });
    server.host("*.example.com").get("/", [](http::response& res) { res.send("never: shadowed by :tenant"); });

    // Dynamic subdomains of any depth, by regex
    server.host_regex("(.+)\\.devices\\.example\\.org").get("/:path(.*)", [](http::request& req, http::response& res) {
        res.json({{"device", req.get_host_matches().at(1)}, {"path", req["path"]}});
    });

    s.start();

    SECTION("Exact hosts and the default") {
        REQUIRE(request_with_host(s.port, "GET", "/", "api.example.com").body == "api");
        REQUIRE(request_with_host(s.port, "GET", "/", "www.example.com").body == "www");
        REQUIRE(request_with_host(s.port, "GET", "/", "other.org").body == "default");
        REQUIRE(request_with_host(s.port, "GET", "/", "127.0.0.1").body == "default");
        REQUIRE(request_with_host(s.port, "GET", "/items/7", "api.example.com").body == "api-item-7");
    }

    SECTION("Host is case-insensitive and the port is ignored") {
        REQUIRE(request_with_host(s.port, "GET", "/", "API.Example.COM:8080").body == "api");
        REQUIRE(request_with_host(s.port, "GET", "/", "www.example.com.").body == "www");
    }

    SECTION("Pattern hosts get the matched part") {
        REQUIRE(request_with_host(s.port, "GET", "/", "admin.example.com").body == "admin");

        auto tenant = nlohmann::json::parse(request_with_host(s.port, "GET", "/", "Acme.example.com").body);
        REQUIRE(tenant["tenant"] == "acme");
        REQUIRE(tenant["matches"] == nlohmann::json::array({"acme.example.com", "acme"}));

        auto device = nlohmann::json::parse(request_with_host(s.port, "GET", "/a/b?x=1", "my.dev1.devices.example.org:443").body);
        REQUIRE(device["device"] == "my.dev1");
        REQUIRE(device["path"] == "a/b");
    }

    SECTION("404 and 405 are computed with the routes of the matched host") {
        // GET exists on the api host, but not this path (only on the default host)
        REQUIRE(request_with_host(s.port, "GET", "/only-default", "api.example.com").status == 404);
        // no POST route on the api host, although the default host has one
        REQUIRE(request_with_host(s.port, "POST", "/items", "api.example.com").status == 405);
        REQUIRE(request_with_host(s.port, "POST", "/items", "other.org").body == "default-post");
        // the default host does not see the routes of the api host
        REQUIRE(request_with_host(s.port, "GET", "/items/7", "other.org").status == 404);
    }
}

TEST_CASE("Virtual hosts share the global middlewares", "[server][vhost][integration]") {
    running_server s;
    auto& server = s.server;

    // Runs for every host, after route matching: sees the host and the route that matched
    server.use([](http::request& req, http::response& res) -> awaitable<bool> {
        auto* route = req.get_matched_route();
        if (route && route->get_meta("admin") == true && req.header("Authorization") != "Bearer admin") {
            res.error(http::http_response::status::forbidden, "admin only");
            co_return false;
        }
        res.header("X-Host", req.get_virtual_host() ? req.get_virtual_host()->name() : "none");
        res.header("X-Route", route ? route->get_pattern() : "none");
        co_return true;
    });

    server.get("/", [](http::response& res) { res.send("default"); });
    auto& api = server.host("api.example.com");
    api.get("/users/:id", [](http::response& res) { res.send("user"); });
    api.get("/admin", [](http::response& res) { res.send("admin"); }).meta("admin", true);

    s.start();

    auto api_response = request_with_host(s.port, "GET", "/users/1", "api.example.com");
    REQUIRE(api_response.body == "user");
    REQUIRE(api_response.headers.find("X-Host: api.example.com") != std::string::npos);
    REQUIRE(api_response.headers.find("X-Route: /users/:id") != std::string::npos);

    auto default_response = request_with_host(s.port, "GET", "/", "other.org");
    REQUIRE(default_response.body == "default");
    REQUIRE(default_response.headers.find("X-Host: *") != std::string::npos);
    REQUIRE(default_response.headers.find("X-Route: /") != std::string::npos);

    auto unmatched = request_with_host(s.port, "GET", "/missing", "api.example.com");
    REQUIRE(unmatched.status == 404);
    REQUIRE(unmatched.headers.find("X-Route: none") != std::string::npos);

    // route metadata of a virtual host route
    REQUIRE(request_with_host(s.port, "GET", "/admin", "api.example.com").status == 403);
}

TEST_CASE("Virtual hosts support the whole registration API", "[server][vhost][integration]") {
    running_server s;
    auto& server = s.server;

    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / ("vhost_static_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    std::ofstream(root / "index.html") << "<h1>static site</h1>";
    std::ofstream(root / "app.js") << "console.log(1);";

    // Static site on its own host, as the default host serves the API
    server.host("www.example.com").serve_static("/", root.string());
    server.get("/", [](http::response& res) { res.send("api"); });

    // Group with a JSON body coroutine handler and a schema
    auto& api = server.host("api.example.com");
    api.group("/v1").tag("devices").post("/devices", [](nlohmann::json& body, http::response& res) -> awaitable<void> {
        res.json({{"created", body["name"]}}, http::http_response::status::created);
        co_return;
    }).schema({{"type", "object"}, {"required", {"name"}}}).summary("Create a device");

    api.set_not_found_handler([](http::response& res) {
        res.error(http::http_response::status::not_found, "no such api resource");
    });

    s.start();

    SECTION("Static files") {
        REQUIRE(request_with_host(s.port, "GET", "/", "www.example.com").body == "<h1>static site</h1>");
        REQUIRE(request_with_host(s.port, "GET", "/app.js", "www.example.com").body == "console.log(1);");
        REQUIRE(request_with_host(s.port, "GET", "/", "api.example.org").body == "api");
    }

    SECTION("Coroutine JSON route in a group, with schema validation") {
        auto created = request_with_host(s.port, "POST", "/v1/devices", "api.example.com", R"({"name":"d1"})");
        REQUIRE(created.status == 201);
        REQUIRE(nlohmann::json::parse(created.body)["created"] == "d1");
        REQUIRE(request_with_host(s.port, "POST", "/v1/devices", "api.example.com", R"({"x":1})").status == 400);
    }

    SECTION("Fallback handler of the host") {
        auto missing = request_with_host(s.port, "GET", "/nothing", "api.example.com");
        REQUIRE(missing.status == 404);
        REQUIRE(missing.body == "no such api resource");
        REQUIRE(request_with_host(s.port, "GET", "/nothing", "other.org").body.empty());
    }

    SECTION("OpenAPI document of a host") {
        auto document = http::openapi_generator(server.host("api.example.com").router()).generate();
        REQUIRE(document["paths"].contains("/v1/devices"));
        REQUIRE_FALSE(document["paths"].contains("/"));

        auto default_document = server.openapi().generate();
        REQUIRE(default_document["paths"].contains("/"));
        REQUIRE_FALSE(default_document["paths"].contains("/v1/devices"));
    }

    fs::remove_all(root);
}

TEST_CASE("Virtual hosts in in-memory dispatch", "[server][vhost][dispatch][integration]") {
    http::server server;
    server.get("/whoami", [](http::response& res) { res.send("default"); });
    server.host("api.example.com").get("/whoami", [](http::response& res) { res.send("api"); });
    server.host("*.example.com").get("/whoami", [](http::request& req, http::response& res) {
        res.send("sub:" + req.get_host_matches().at(1));
    });

    REQUIRE(run_dispatch(server, http::method::GET, "http://api.example.com/whoami")->get_content() == "api");
    REQUIRE(run_dispatch(server, http::method::GET, "http://API.example.com:8080/whoami")->get_content() == "api");
    REQUIRE(run_dispatch(server, http::method::GET, "http://dev7.example.com/whoami")->get_content() == "sub:dev7");
    REQUIRE(run_dispatch(server, http::method::GET, "http://localhost/whoami")->get_content() == "default");
    REQUIRE(run_dispatch(server, http::method::POST, "http://api.example.com/whoami")->get_status_code() == 405);

    SECTION("host(\"*\") is the application itself, and hosts are registered once") {
        REQUIRE(&server.host("*") == &server.application());
        REQUIRE(&server.host("API.example.com") == &server.host("api.example.com"));
        REQUIRE(&server.host("*.example.com") == &server.host("*.example.com"));
    }
}

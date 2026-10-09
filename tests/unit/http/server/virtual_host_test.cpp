#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/virtual_host.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>

using namespace thinger::http;

TEST_CASE("Host names are normalized", "[virtual_host][unit]") {
    REQUIRE(virtual_host::normalize_host("example.com") == "example.com");
    REQUIRE(virtual_host::normalize_host("API.Example.COM:8080") == "api.example.com");
    REQUIRE(virtual_host::normalize_host("example.com.") == "example.com");
    REQUIRE(virtual_host::normalize_host("[::1]:8080") == "[::1]");
    REQUIRE(virtual_host::normalize_host("[2001:DB8::1]") == "[2001:db8::1]");
    REQUIRE(virtual_host::normalize_host("") == "");
}

TEST_CASE("Host patterns", "[virtual_host][unit]") {
    SECTION("Exact names are not patterns") {
        REQUIRE_FALSE(virtual_host::is_host_pattern("api.example.com"));
        REQUIRE_FALSE(virtual_host::is_host_pattern("[::1]"));
        virtual_host host("API.example.com");
        REQUIRE(host.name() == "api.example.com");
        REQUIRE_FALSE(host.is_pattern());
        std::smatch m;
        std::string name = "api.example.com";
        REQUIRE_FALSE(host.matches(name, m));
    }

    SECTION("Wildcard matches one label") {
        REQUIRE(virtual_host::is_host_pattern("*.example.com"));
        virtual_host host("*.example.com");
        REQUIRE(host.is_pattern());
        REQUIRE(host.get_parameters() == std::vector<std::string>{""});

        std::smatch m;
        std::string sub = "device1.example.com";
        REQUIRE(host.matches(sub, m));
        REQUIRE(m[1].str() == "device1");

        std::string nested = "a.b.example.com";
        std::string other = "example.com";
        std::string lookalike = "device1.exampleXcom";
        REQUIRE_FALSE(host.matches(nested, m));
        REQUIRE_FALSE(host.matches(other, m));
        REQUIRE_FALSE(host.matches(lookalike, m));
    }

    SECTION("Named labels, with an optional regex") {
        REQUIRE(virtual_host::is_host_pattern(":tenant.example.com"));
        virtual_host host(":tenant.:region([a-z]{2}-[0-9]).example.com");
        REQUIRE(host.get_parameters() == std::vector<std::string>{"tenant", "region"});

        std::smatch m;
        std::string name = "acme.eu-1.example.com";
        REQUIRE(host.matches(name, m));
        REQUIRE(m[1].str() == "acme");
        REQUIRE(m[2].str() == "eu-1");

        std::string bad_region = "acme.europe.example.com";
        REQUIRE_FALSE(host.matches(bad_region, m));
    }

    SECTION("Regular expression") {
        virtual_host host("(.+)\\.proxy\\.example\\.com", std::regex("(.+)\\.proxy\\.example\\.com", std::regex::icase));
        REQUIRE(host.is_pattern());
        REQUIRE(host.get_parameters().size() == 1);

        std::smatch m;
        std::string name = "a.b.proxy.example.com";
        REQUIRE(host.matches(name, m));
        REQUIRE(m[1].str() == "a.b");
    }
}

TEST_CASE("Routes are registered on a virtual host", "[virtual_host][unit]") {
    virtual_host host("api.example.com");
    host.get("/items", [](response& res) { res.send("items"); }).summary("List items");
    host.group("/v1").tag("v1").post("/things", [](nlohmann::json& body, response& res) -> thinger::awaitable<void> {
        co_return;
    });

    const auto& routes = host.router().get_routes();
    REQUIRE(routes.at(method::GET).size() == 1);
    REQUIRE(routes.at(method::GET)[0].get_summary() == "List items");
    REQUIRE(routes.at(method::POST)[0].get_pattern() == "/v1/things");
    REQUIRE(routes.at(method::POST)[0].get_tags() == std::vector<std::string>{"v1"});
    REQUIRE(routes.at(method::POST)[0].takes_json_body());
}

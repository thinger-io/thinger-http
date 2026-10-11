#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/proxy.hpp>
#include <thinger/util/base64.hpp>
#include <map>

using namespace thinger::http;

namespace {

    // An environment of its own, instead of the process one
    proxy_config::env_reader environment(std::map<std::string, std::string> variables) {
        return [variables = std::move(variables)](const char* name) -> std::optional<std::string> {
            auto it = variables.find(name);
            if (it == variables.end()) return std::nullopt;
            return it->second;
        };
    }

    std::string basic(const std::string& credentials) {
        return "Basic " + thinger::util::base64::encode(credentials);
    }

}

TEST_CASE("Proxy URLs", "[proxy][client][unit]") {
    SECTION("Host and port, with or without a scheme") {
        auto proxy = proxy_endpoint::parse("http://proxy.corp:3128");
        REQUIRE(proxy);
        REQUIRE(proxy->host == "proxy.corp");
        REQUIRE(proxy->port == "3128");
        REQUIRE(proxy->authorization.empty());
        REQUIRE(proxy->url() == "http://proxy.corp:3128");

        // As curl and Go: a URL without a scheme is an HTTP proxy
        proxy = proxy_endpoint::parse("  Proxy.Corp:8080  ");
        REQUIRE(proxy);
        REQUIRE(proxy->host == "proxy.corp");
        REQUIRE(proxy->port == "8080");

        // The default port of http, as Go and Node; a path is ignored
        proxy = proxy_endpoint::parse("HTTP://proxy.corp/");
        REQUIRE(proxy);
        REQUIRE(proxy->port == "80");
    }

    SECTION("IPv6 addresses") {
        auto proxy = proxy_endpoint::parse("http://[::1]:3128");
        REQUIRE(proxy);
        REQUIRE(proxy->host == "::1");
        REQUIRE(proxy->url() == "http://[::1]:3128");
        REQUIRE_FALSE(proxy_endpoint::parse("http://::1:3128"));
        REQUIRE_FALSE(proxy_endpoint::parse("http://[::1"));
    }

    SECTION("Credentials are percent-decoded for Proxy-Authorization") {
        auto proxy = proxy_endpoint::parse("http://user%40corp:p%3Ass+w%2Frd@proxy:3128");
        REQUIRE(proxy);
        REQUIRE(proxy->host == "proxy");
        REQUIRE(proxy->authorization == basic("user@corp:p:ss+w/rd"));

        proxy = proxy_endpoint::parse("http://user@proxy:3128");
        REQUIRE(proxy);
        REQUIRE(proxy->authorization == basic("user:"));

        // Not in the URL of logs
        REQUIRE(proxy->url() == "http://proxy:3128");
        REQUIRE_FALSE(proxy_endpoint::parse("http://us%zzer:pass@proxy:3128"));
    }

    SECTION("Only HTTP proxies are supported") {
        std::string error;
        REQUIRE_FALSE(proxy_endpoint::parse("https://proxy:443", &error));
        REQUIRE(error.find("HTTPS proxies") != std::string::npos);
        REQUIRE_FALSE(proxy_endpoint::parse("socks5://proxy:1080", &error));
        REQUIRE(error.find("SOCKS") != std::string::npos);
        REQUIRE_FALSE(proxy_endpoint::parse("ftp://proxy:21", &error));
    }

    SECTION("Invalid hosts and ports") {
        REQUIRE_FALSE(proxy_endpoint::parse(""));
        REQUIRE_FALSE(proxy_endpoint::parse("http://"));
        REQUIRE_FALSE(proxy_endpoint::parse("http://:3128"));
        REQUIRE_FALSE(proxy_endpoint::parse("http://proxy:0"));
        REQUIRE_FALSE(proxy_endpoint::parse("http://proxy:65536"));
        REQUIRE_FALSE(proxy_endpoint::parse("http://proxy:31a8"));
        REQUIRE_FALSE(proxy_endpoint::parse("http://pro xy:3128"));
    }

    SECTION("The key tells proxies and credentials apart") {
        auto a = proxy_endpoint::parse("http://a:1@proxy:3128");
        auto b = proxy_endpoint::parse("http://b:1@proxy:3128");
        auto c = proxy_endpoint::parse("http://proxy:3128");
        REQUIRE(a->key() != b->key());
        REQUIRE(a->key() != c->key());
        REQUIRE(c->key() == proxy_endpoint::parse("proxy:3128")->key());
    }
}

TEST_CASE("NO_PROXY lists", "[proxy][client][unit]") {
    SECTION("Domains match themselves and their subdomains, with or without a leading dot") {
        for (auto list : {"example.com", ".example.com", "*.example.com", "EXAMPLE.com."}) {
            INFO(list);
            no_proxy_list no_proxy(list);
            REQUIRE(no_proxy.matches("example.com", "80"));
            REQUIRE(no_proxy.matches("api.example.com", "443"));
            REQUIRE(no_proxy.matches("a.b.Example.COM", "443"));
            REQUIRE(no_proxy.matches("example.com.", "80"));
            REQUIRE_FALSE(no_proxy.matches("badexample.com", "80"));
            REQUIRE_FALSE(no_proxy.matches("example.com.evil", "80"));
            REQUIRE_FALSE(no_proxy.matches("com", "80"));
        }
    }

    SECTION("Comma separated, spaces trimmed, empty entries ignored") {
        no_proxy_list no_proxy(" a.com ,, b.com,\tc.com ");
        REQUIRE(no_proxy.matches("a.com", "80"));
        REQUIRE(no_proxy.matches("b.com", "80"));
        REQUIRE(no_proxy.matches("c.com", "80"));
        REQUIRE_FALSE(no_proxy.matches("d.com", "80"));
        REQUIRE(no_proxy_list("").empty());
        REQUIRE(no_proxy_list(" , ").empty());
    }

    SECTION("* matches every host") {
        REQUIRE(no_proxy_list("*").matches("anything.example", "80"));
        REQUIRE(no_proxy_list("a.com, *").matches("10.1.2.3", "443"));
    }

    SECTION("IP addresses and CIDR ranges, never resolved") {
        no_proxy_list no_proxy("10.1.2.3, 192.168.0.0/16, ::1, [fd00::]/8, 2001:db8::/32");
        REQUIRE(no_proxy.matches("10.1.2.3", "80"));
        REQUIRE_FALSE(no_proxy.matches("10.1.2.4", "80"));
        REQUIRE(no_proxy.matches("192.168.200.1", "80"));
        REQUIRE_FALSE(no_proxy.matches("192.169.0.1", "80"));
        REQUIRE(no_proxy.matches("[::1]", "80"));
        REQUIRE(no_proxy.matches("fd12::1", "80"));
        REQUIRE(no_proxy.matches("2001:db8:1::1", "80"));
        REQUIRE_FALSE(no_proxy.matches("2001:db9::1", "80"));
        REQUIRE(no_proxy.matches("::ffff:10.1.2.3", "80"));
        // A host name is not resolved to match an address
        REQUIRE_FALSE(no_proxy_list("93.184.216.34").matches("example.com", "80"));
    }

    SECTION("An entry with a port only matches that port") {
        no_proxy_list no_proxy("example.com:8080, 10.0.0.1:443, [::1]:8443");
        REQUIRE(no_proxy.matches("example.com", "8080"));
        REQUIRE(no_proxy.matches("api.example.com", "8080"));
        REQUIRE_FALSE(no_proxy.matches("example.com", "80"));
        REQUIRE(no_proxy.matches("10.0.0.1", "443"));
        REQUIRE_FALSE(no_proxy.matches("10.0.0.1", "80"));
        REQUIRE(no_proxy.matches("::1", "8443"));
        REQUIRE_FALSE(no_proxy.matches("::1", "80"));
    }

    SECTION("Invalid entries are ignored") {
        no_proxy_list no_proxy("10.0.0.0/33, example.com:http, [::1, ., *., valid.com");
        REQUIRE_FALSE(no_proxy.matches("10.0.0.1", "80"));
        REQUIRE_FALSE(no_proxy.matches("example.com", "80"));
        REQUIRE(no_proxy.matches("valid.com", "80"));
    }
}

TEST_CASE("Loopback hosts", "[proxy][client][unit]") {
    for (auto host : {"localhost", "LOCALHOST", "localhost.", "app.localhost", "127.0.0.1", "127.1.2.3", "::1",
                      "[::1]", "::ffff:127.0.0.1"}) {
        INFO(host);
        REQUIRE(is_loopback_host(host));
    }
    for (auto host : {"localhost.example.com", "mylocalhost", "10.0.0.1", "0.0.0.0", "::", "example.com"}) {
        INFO(host);
        REQUIRE_FALSE(is_loopback_host(host));
    }
}

TEST_CASE("Proxy settings from the environment", "[proxy][client][unit]") {
    SECTION("Uppercase HTTP_PROXY is ignored (httpoxy), lowercase http_proxy is used") {
        auto config = proxy_config::from_environment(environment({{"HTTP_PROXY", "http://evil:1"}}));
        REQUIRE(config.http.empty());
        config = proxy_config::from_environment(environment({{"HTTP_PROXY", "http://evil:1"}, {"http_proxy", "http://good:2"}}));
        REQUIRE(config.http == "http://good:2");
    }

    SECTION("Lowercase first, then uppercase, for the others") {
        auto config = proxy_config::from_environment(environment({
            {"https_proxy", "http://lower:1"}, {"HTTPS_PROXY", "http://upper:2"},
            {"no_proxy", "lower.com"}, {"NO_PROXY", "upper.com"}}));
        REQUIRE(config.https == "http://lower:1");
        REQUIRE(config.no_proxy == "lower.com");

        config = proxy_config::from_environment(environment({{"HTTPS_PROXY", "http://upper:2"}, {"NO_PROXY", "upper.com"}}));
        REQUIRE(config.https == "http://upper:2");
        REQUIRE(config.no_proxy == "upper.com");
    }

    SECTION("Empty values are not set") {
        auto config = proxy_config::from_environment(environment({{"https_proxy", " "}, {"HTTPS_PROXY", "http://upper:2"}}));
        REQUIRE(config.https == "http://upper:2");
    }

    SECTION("ALL_PROXY when the scheme has none") {
        auto config = proxy_config::from_environment(environment({{"ALL_PROXY", "http://all:3"}, {"https_proxy", "http://tls:4"}}));
        REQUIRE(config.http == "http://all:3");
        REQUIRE(config.https == "http://tls:4");
        config = proxy_config::from_environment(environment({{"all_proxy", "http://lower:3"}, {"ALL_PROXY", "http://upper:3"}}));
        REQUIRE(config.http == "http://lower:3");
        REQUIRE(config.https == "http://lower:3");
    }

    SECTION("Nothing set") {
        auto config = proxy_config::from_environment(environment({}));
        REQUIRE(config.http.empty());
        REQUIRE(config.https.empty());
        REQUIRE(config.no_proxy.empty());
    }
}

TEST_CASE("Proxy selection", "[proxy][client][unit]") {
    proxy_resolver resolver(proxy_config{"http://plain:1", "http://tls:2", "internal.corp, 10.0.0.0/8"});

    SECTION("By scheme") {
        auto choice = resolver.select(false, "example.com", "80");
        REQUIRE(choice.proxy);
        REQUIRE(choice.proxy->host == "plain");
        choice = resolver.select(true, "example.com", "443");
        REQUIRE(choice.proxy);
        REQUIRE(choice.proxy->host == "tls");
    }

    SECTION("NO_PROXY and loopback hosts go directly") {
        REQUIRE_FALSE(resolver.select(false, "api.internal.corp", "80").proxy);
        REQUIRE_FALSE(resolver.select(true, "10.20.30.40", "443").proxy);
        REQUIRE_FALSE(resolver.select(false, "localhost", "80").proxy);
        REQUIRE_FALSE(resolver.select(true, "127.0.0.1", "443").proxy);
    }

    SECTION("A scheme without proxy goes directly") {
        proxy_resolver only_https(proxy_config{"", "http://tls:2", ""});
        REQUIRE_FALSE(only_https.select(false, "example.com", "80").proxy);
        REQUIRE(only_https.select(true, "example.com", "443").proxy);
        REQUIRE_FALSE(proxy_resolver().select(true, "example.com", "443").proxy);
    }

    SECTION("An unusable proxy is an error, unless the host is reached directly") {
        proxy_resolver invalid(proxy_config{"https://secure-proxy:443", "socks5://socks:1080", "direct.com"});
        auto choice = invalid.select(false, "example.com", "80");
        REQUIRE_FALSE(choice.proxy);
        REQUIRE_FALSE(choice.error.empty());
        REQUIRE_FALSE(invalid.select(true, "example.com", "443").error.empty());
        REQUIRE(invalid.select(true, "direct.com", "443").error.empty());
        REQUIRE(invalid.select(true, "localhost", "443").error.empty());
    }
}

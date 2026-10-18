#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/trusted_proxies.hpp>
#include <thinger/http/common/http_request.hpp>

using namespace thinger::http;

using strings = std::vector<std::string>;

TEST_CASE("Trusted proxies match IPs and CIDR ranges", "[trusted_proxies][unit]") {
    trusted_proxies proxies;
    REQUIRE(proxies.empty());

    SECTION("Single IPv4 address") {
        REQUIRE(proxies.add("10.0.0.1"));
        REQUIRE(proxies.contains("10.0.0.1"));
        REQUIRE_FALSE(proxies.contains("10.0.0.2"));
    }

    SECTION("IPv4 ranges") {
        REQUIRE(proxies.add("10.0.0.0/8"));
        REQUIRE(proxies.add("192.168.1.128/25"));
        REQUIRE(proxies.contains("10.255.3.4"));
        REQUIRE_FALSE(proxies.contains("11.0.0.1"));
        REQUIRE(proxies.contains("192.168.1.200"));
        REQUIRE_FALSE(proxies.contains("192.168.1.127"));
    }

    SECTION("/0 trusts every address of the family") {
        REQUIRE(proxies.add("0.0.0.0/0"));
        REQUIRE(proxies.contains("8.8.8.8"));
        REQUIRE_FALSE(proxies.contains("2001:db8::1"));
    }

    SECTION("IPv6 addresses and ranges") {
        REQUIRE(proxies.add("::1"));
        REQUIRE(proxies.add("fd00::/8"));
        REQUIRE(proxies.add("2001:db8:abcd::/50"));
        REQUIRE(proxies.contains("::1"));
        REQUIRE(proxies.contains("fd12:3456::1"));
        REQUIRE_FALSE(proxies.contains("fe80::1"));
        REQUIRE(proxies.contains("2001:db8:abcd:3fff::1"));
        REQUIRE_FALSE(proxies.contains("2001:db8:abcd:4000::1"));
        REQUIRE_FALSE(proxies.contains("127.0.0.1"));
    }

    SECTION("IPv4-mapped IPv6 addresses compare as IPv4") {
        REQUIRE(proxies.add("127.0.0.1"));
        REQUIRE(proxies.contains("::ffff:127.0.0.1"));
        REQUIRE(proxies.add("::ffff:10.0.0.0/104"));
        REQUIRE(proxies.contains("10.1.2.3"));
    }

    SECTION("Invalid entries are rejected") {
        REQUIRE_FALSE(proxies.add("not-an-ip"));
        REQUIRE_FALSE(proxies.add("10.0.0.0/33"));
        REQUIRE_FALSE(proxies.add("fd00::/129"));
        REQUIRE_FALSE(proxies.add("10.0.0.0/"));
        REQUIRE_FALSE(proxies.add("10.0.0.0/8x"));
        REQUIRE_FALSE(proxies.add(""));
        REQUIRE(proxies.empty());
    }

    SECTION("Anything that is not an IP is never trusted") {
        REQUIRE(proxies.add("0.0.0.0/0"));
        REQUIRE_FALSE(proxies.contains(""));
        REQUIRE_FALSE(proxies.contains("unknown"));
        REQUIRE_FALSE(proxies.contains("10.0.0.1:80"));
    }
}

TEST_CASE("Forwarding headers are parsed", "[trusted_proxies][unit]") {
    SECTION("X-Forwarded-For") {
        REQUIRE(trusted_proxies::parse_x_forwarded_for("203.0.113.7") == strings{"203.0.113.7"});
        REQUIRE(trusted_proxies::parse_x_forwarded_for(" 203.0.113.7 ,10.0.0.1,  2001:db8::1 ")
                == strings{"203.0.113.7", "10.0.0.1", "2001:db8::1"});
        REQUIRE(trusted_proxies::parse_x_forwarded_for("203.0.113.7:4711, [2001:db8::1]:80")
                == strings{"203.0.113.7", "2001:db8::1"});
        REQUIRE(trusted_proxies::parse_x_forwarded_for("unknown, 10.0.0.1") == strings{"", "10.0.0.1"});
        REQUIRE(trusted_proxies::parse_x_forwarded_for("").empty());
    }

    SECTION("Forwarded (RFC 7239)") {
        REQUIRE(trusted_proxies::parse_forwarded("for=192.0.2.60;proto=http;by=203.0.113.43")
                == strings{"192.0.2.60"});
        REQUIRE(trusted_proxies::parse_forwarded(R"(for=192.0.2.43, For="[2001:db8:cafe::17]:4711")")
                == strings{"192.0.2.43", "2001:db8:cafe::17"});
        REQUIRE(trusted_proxies::parse_forwarded(R"(proto=https;for="10.0.0.1:80")") == strings{"10.0.0.1"});
        REQUIRE(trusted_proxies::parse_forwarded("for=unknown, for=_hidden, by=10.0.0.1")
                == strings{"", "", ""});
        REQUIRE(trusted_proxies::parse_forwarded(R"(for="a,b";proto=http, for=10.0.0.2)")
                == strings{"", "10.0.0.2"});
    }
}

TEST_CASE("Client IP is resolved from right to left", "[trusted_proxies][unit]") {
    trusted_proxies proxies;
    REQUIRE(proxies.add("10.0.0.0/8"));

    SECTION("Untrusted peer: the header is ignored") {
        REQUIRE(proxies.client_ip("203.0.113.9", {"1.2.3.4"}) == "203.0.113.9");
    }

    SECTION("Trusted peer without header") {
        REQUIRE(proxies.client_ip("10.0.0.1", {}) == "10.0.0.1");
    }

    SECTION("First untrusted address from the right") {
        REQUIRE(proxies.client_ip("10.0.0.1", {"1.2.3.4"}) == "1.2.3.4");
        REQUIRE(proxies.client_ip("10.0.0.1", {"1.2.3.4, 10.0.0.2, 10.0.0.3"}) == "1.2.3.4");
        // addresses prepended by the client are not reached
        REQUIRE(proxies.client_ip("10.0.0.1", {"127.0.0.1, 5.6.7.8"}) == "5.6.7.8");
    }

    SECTION("Leftmost address when all of them are trusted") {
        REQUIRE(proxies.client_ip("10.0.0.1", {"10.0.0.3, 10.0.0.2"}) == "10.0.0.3");
    }

    SECTION("Several header lines are one list") {
        REQUIRE(proxies.client_ip("10.0.0.1", {"9.9.9.9, 1.2.3.4", "10.0.0.2"}) == "1.2.3.4");
    }

    SECTION("An entry that is not an IP stops at the proxy after it") {
        REQUIRE(proxies.client_ip("10.0.0.1", {"1.2.3.4, garbage, 10.0.0.2"}) == "10.0.0.2");
        REQUIRE(proxies.client_ip("10.0.0.1", {"garbage"}) == "10.0.0.1");
    }

    SECTION("Forwarded header") {
        trusted_proxies forwarded(forwarded_header::forwarded);
        REQUIRE(forwarded.add("fd00::/8"));
        REQUIRE(forwarded.header_name() == "Forwarded");
        REQUIRE(forwarded.client_ip("fd00::1", {R"(for="[2001:db8::7]:1234", for=fd00::2)"}) == "2001:db8::7");
        REQUIRE(forwarded.client_ip("fe80::1", {"for=2001:db8::7"}) == "fe80::1");
    }
}

TEST_CASE("Client scheme behind trusted proxies", "[trusted_proxies][unit]") {
    SECTION("Forwarded values") {
        REQUIRE(trusted_proxies::parse_scheme("https") == "https");
        REQUIRE(trusted_proxies::parse_scheme(" HTTP ") == "http");
        REQUIRE(trusted_proxies::parse_scheme(R"("https")") == "https");
        REQUIRE(trusted_proxies::parse_scheme("wss").empty());
        REQUIRE(trusted_proxies::parse_scheme("https, http").empty());
        REQUIRE(trusted_proxies::parse_scheme("").empty());
    }

    using header_list = std::vector<std::pair<std::string, std::string>>;
    // The client of a request from `peer` on a connection of `scheme`, with `headers`
    auto resolve = [](const trusted_proxies& proxies, const std::string& peer, const std::string& scheme,
                      const header_list& headers) {
        auto request = http_request::create_http_request(method::GET, "http://localhost/");
        for (const auto& [key, value] : headers) request->add_header(key, value);
        return proxies.resolve(peer, scheme, *request);
    };

    SECTION("X-Forwarded-Proto") {
        trusted_proxies proxies;
        REQUIRE(proxies.add("10.0.0.0/8"));
        auto scheme = [&](const std::string& peer, const std::string& connection, const header_list& headers) {
            return resolve(proxies, peer, connection, headers).scheme;
        };
        // untrusted peer: the connection's, whatever it claims
        REQUIRE(scheme("203.0.113.9", "http", {{"X-Forwarded-Proto", "https"}}) == "http");
        REQUIRE(scheme("203.0.113.9", "https", {{"X-Forwarded-Proto", "http"}}) == "https");
        // a proxy overwriting it: its value
        REQUIRE(scheme("10.0.0.1", "http", {{"X-Forwarded-For", "1.2.3.4"}, {"X-Forwarded-Proto", "https"}}) == "https");
        REQUIRE(scheme("10.0.0.1", "https", {{"X-Forwarded-Proto", "http"}}) == "http");
        REQUIRE(scheme("10.0.0.1", "http", {{"X-Forwarded-For", "6.6.6.6, 1.2.3.4"}, {"X-Forwarded-Proto", "HTTPS"}}) == "https");
        // a proxy appending to both: the value of the client, not what the client sent
        REQUIRE(scheme("10.0.0.1", "http", {{"X-Forwarded-For", "6.6.6.6, 1.2.3.4"}, {"X-Forwarded-Proto", "https, http"}}) == "http");
        REQUIRE(scheme("10.0.0.1", "http", {{"X-Forwarded-For", "1.2.3.4, 10.0.0.2"}, {"X-Forwarded-Proto", "https"},
                                            {"X-Forwarded-Proto", "http"}}) == "https");
        // counts that do not match: the last value, of the nearest proxy
        REQUIRE(scheme("10.0.0.1", "http", {{"X-Forwarded-For", "1.2.3.4"}, {"X-Forwarded-Proto", "https, http"}}) == "http");
        REQUIRE(scheme("10.0.0.1", "http", {{"X-Forwarded-Proto", "http, https"}}) == "https");
        // missing, or not http nor https: the connection's
        REQUIRE(scheme("10.0.0.1", "https", {}) == "https");
        REQUIRE(scheme("10.0.0.1", "http", {{"X-Forwarded-Proto", "wss"}}) == "http");
        REQUIRE(scheme("10.0.0.1", "http", {{"X-Forwarded-Proto", ""}}) == "http");
        // and the address of the same walk
        auto client = resolve(proxies, "10.0.0.1", "http", {{"X-Forwarded-For", "6.6.6.6, 1.2.3.4"},
                                                            {"X-Forwarded-Proto", "https, http"}});
        REQUIRE(client.ip == "1.2.3.4");
        REQUIRE(client.scheme == "http");
    }

    SECTION("Forwarded: the proto of the element giving the client address") {
        trusted_proxies proxies(forwarded_header::forwarded);
        REQUIRE(proxies.add("10.0.0.0/8"));
        auto scheme = [&](const std::string& peer, const std::string& connection, const std::vector<std::string>& values) {
            header_list headers;
            for (const auto& value : values) headers.emplace_back("Forwarded", value);
            return resolve(proxies, peer, connection, headers).scheme;
        };
        REQUIRE(scheme("203.0.113.9", "http", {"for=1.2.3.4;proto=https"}) == "http");
        REQUIRE(scheme("10.0.0.1", "http", {"for=1.2.3.4;proto=https"}) == "https");
        // the walk stops at the client: what the client prepended is not read
        REQUIRE(scheme("10.0.0.1", "http", {"for=5.6.7.8;proto=https, for=1.2.3.4;proto=http, for=10.0.0.2;proto=https"})
                == "http");
        REQUIRE(scheme("10.0.0.1", "https", {R"(for=1.2.3.4;proto="http")", "for=10.0.0.2"}) == "http");
        // all of them trusted: the leftmost one
        REQUIRE(scheme("10.0.0.1", "http", {"for=10.0.0.3;proto=https, for=10.0.0.2;proto=http"}) == "https");
        // an address that is not an IP: the element of the proxy that wrote it
        REQUIRE(scheme("10.0.0.1", "http", {"for=1.2.3.4;proto=http, for=unknown;proto=https"}) == "https");
        // without proto in that element: the connection's
        REQUIRE(scheme("10.0.0.1", "https", {"for=1.2.3.4, for=10.0.0.2;proto=http"}) == "https");
        REQUIRE(scheme("10.0.0.1", "http", {}) == "http");
        // X-Forwarded-Proto is not read in this mode
        REQUIRE(resolve(proxies, "10.0.0.1", "http", {{"X-Forwarded-Proto", "https"}}).scheme == "http");
    }
}

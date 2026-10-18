#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/util/types.hpp>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <boost/asio/use_future.hpp>
#include <thread>

using namespace thinger;

namespace {

    // Client and peer IP seen by the handler for a request from `peer` with the given headers
    nlohmann::json ips(http::server& server, const std::string& peer,
                       const std::vector<std::pair<std::string, std::string>>& headers = {}) {
        auto request = http::http_request::create_http_request(http::method::GET, "http://localhost/ip");
        for (const auto& [key, value] : headers) request->add_header(key, value);

        boost::asio::io_context ioc;
        auto result = co_spawn(ioc, server.dispatch(request, {.remote_ip = peer}), boost::asio::use_future);
        ioc.run();
        auto response = result.get();
        REQUIRE(response->get_status_code() == 200);
        return nlohmann::json::parse(response->get_content());
    }

    std::string client_ip(http::server& server, const std::string& peer,
                          const std::vector<std::pair<std::string, std::string>>& headers = {}) {
        return ips(server, peer, headers)["client"];
    }

    void add_ip_route(http::server& server) {
        server.get("/ip", [](http::request& req, http::response& res) {
            res.json({{"client", req.get_request_ip()}, {"peer", req.get_peer_ip()}, {"scheme", req.get_request_scheme()}});
        });
    }

    // Scheme seen by the handler for a request to `url` from `peer` with the given headers
    std::string client_scheme(http::server& server, const std::string& peer,
                              const std::vector<std::pair<std::string, std::string>>& headers = {},
                              const std::string& url = "http://localhost/ip") {
        auto request = http::http_request::create_http_request(http::method::GET, url);
        for (const auto& [key, value] : headers) request->add_header(key, value);

        boost::asio::io_context ioc;
        auto result = co_spawn(ioc, server.dispatch(request, {.remote_ip = peer}), boost::asio::use_future);
        ioc.run();
        auto response = result.get();
        REQUIRE(response->get_status_code() == 200);
        return nlohmann::json::parse(response->get_content())["scheme"];
    }

}

TEST_CASE("Forwarding headers are ignored without trusted proxies", "[server][proxies][integration]") {
    http::server server;
    add_ip_route(server);

    auto result = ips(server, "10.0.0.1", {{"X-Forwarded-For", "1.2.3.4"}, {"Forwarded", "for=5.6.7.8"}});
    REQUIRE(result["client"] == "10.0.0.1");
    REQUIRE(result["peer"] == "10.0.0.1");

    // A client claiming to be local is still reported with its own address
    REQUIRE(client_ip(server, "203.0.113.5", {{"X-Forwarded-For", "127.0.0.1"}}) == "203.0.113.5");
}

TEST_CASE("Client IP behind trusted proxies", "[server][proxies][integration]") {
    http::server server;
    add_ip_route(server);
    REQUIRE(server.set_trusted_proxies({"10.0.0.0/8", "fd00::/8"}));

    SECTION("Header of a trusted proxy") {
        auto result = ips(server, "10.0.0.1", {{"X-Forwarded-For", "1.2.3.4"}});
        REQUIRE(result["client"] == "1.2.3.4");
        REQUIRE(result["peer"] == "10.0.0.1");
    }

    SECTION("Header ignored when the peer is not a trusted proxy") {
        REQUIRE(client_ip(server, "192.168.1.10", {{"X-Forwarded-For", "1.2.3.4"}}) == "192.168.1.10");
        REQUIRE(client_ip(server, "", {{"X-Forwarded-For", "127.0.0.1"}}) == "");
    }

    SECTION("Walk from right to left") {
        // spoofed entries prepended by the client are not reached
        REQUIRE(client_ip(server, "10.0.0.1", {{"X-Forwarded-For", "127.0.0.1, 1.2.3.4, 10.0.0.2"}}) == "1.2.3.4");
        // all trusted: the leftmost one
        REQUIRE(client_ip(server, "10.0.0.1", {{"X-Forwarded-For", "10.0.0.3, 10.0.0.2"}}) == "10.0.0.3");
        // several header lines
        REQUIRE(client_ip(server, "10.0.0.1", {{"X-Forwarded-For", "9.9.9.9"}, {"X-Forwarded-For", "10.0.0.2"}}) == "9.9.9.9");
    }

    SECTION("IPv6 proxies and clients") {
        REQUIRE(client_ip(server, "fd00::1", {{"X-Forwarded-For", "2001:db8::5, fd00::2"}}) == "2001:db8::5");
        REQUIRE(client_ip(server, "fe80::1", {{"X-Forwarded-For", "2001:db8::5"}}) == "fe80::1");
    }

    SECTION("Only the configured header is read") {
        REQUIRE(client_ip(server, "10.0.0.1", {{"Forwarded", "for=1.2.3.4"}}) == "10.0.0.1");
    }
}

TEST_CASE("Client IP from the Forwarded header", "[server][proxies][integration]") {
    http::server server;
    add_ip_route(server);
    REQUIRE(server.set_trusted_proxies({"10.0.0.1", "::1"}, http::forwarded_header::forwarded));

    REQUIRE(client_ip(server, "10.0.0.1", {{"Forwarded", R"(for="[2001:db8:cafe::17]:4711";proto=https)"}})
            == "2001:db8:cafe::17");
    REQUIRE(client_ip(server, "::1", {{"Forwarded", "for=192.0.2.43, for=10.0.0.1"}}) == "192.0.2.43");
    REQUIRE(client_ip(server, "10.0.0.1", {{"X-Forwarded-For", "1.2.3.4"}}) == "10.0.0.1");
    REQUIRE(client_ip(server, "10.0.0.2", {{"Forwarded", "for=1.2.3.4"}}) == "10.0.0.2");
}

TEST_CASE("Invalid trusted proxies are rejected", "[server][proxies][integration]") {
    http::server server;
    add_ip_route(server);
    REQUIRE(server.set_trusted_proxies({"10.0.0.1"}));
    REQUIRE_FALSE(server.set_trusted_proxies({"10.0.0.1", "10.0.0.0/40"}));

    // nothing is trusted after a failed configuration
    REQUIRE(client_ip(server, "10.0.0.1", {{"X-Forwarded-For", "1.2.3.4"}}) == "10.0.0.1");
}

TEST_CASE("Client IP over a connection", "[server][proxies][integration]") {
    http::server server;
    add_ip_route(server);

    auto request_ips = [&server](bool trusted) {
        if (trusted) REQUIRE(server.set_trusted_proxies({"127.0.0.1"}));
        REQUIRE(server.listen("127.0.0.1", 0));
        std::thread thread([&server]() { server.wait(); });

        http::client client;
        auto response = client.get("http://127.0.0.1:" + std::to_string(server.local_port()) + "/ip",
                                   {{"X-Forwarded-For", "203.0.113.9"}});
        server.stop();
        thread.join();
        REQUIRE(response.status() == 200);
        return nlohmann::json::parse(response.body());
    };

    SECTION("Peer not trusted") {
        auto result = request_ips(false);
        REQUIRE(result["client"] == "127.0.0.1");
        REQUIRE(result["peer"] == "127.0.0.1");
    }

    SECTION("Local proxy trusted") {
        auto result = request_ips(true);
        REQUIRE(result["client"] == "203.0.113.9");
        REQUIRE(result["peer"] == "127.0.0.1");
    }
}

TEST_CASE("Request scheme behind trusted proxies", "[server][proxies][integration]") {
    http::server server;
    add_ip_route(server);

    SECTION("The scheme of the request without trusted proxies") {
        REQUIRE(client_scheme(server, "10.0.0.1") == "http");
        REQUIRE(client_scheme(server, "10.0.0.1", {}, "https://localhost/ip") == "https");
        // spoofed headers are ignored
        REQUIRE(client_scheme(server, "10.0.0.1", {{"X-Forwarded-Proto", "https"}}) == "http");
        REQUIRE(client_scheme(server, "10.0.0.1", {{"Forwarded", "for=1.2.3.4;proto=http"}}, "https://localhost/ip")
                == "https");
    }

    SECTION("X-Forwarded-Proto of a trusted proxy") {
        REQUIRE(server.set_trusted_proxies({"10.0.0.0/8"}));
        REQUIRE(client_scheme(server, "10.0.0.1", {{"X-Forwarded-For", "1.2.3.4"}, {"X-Forwarded-Proto", "https"}}) == "https");
        REQUIRE(client_scheme(server, "10.0.0.1", {{"X-Forwarded-Proto", "http"}}, "https://localhost/ip") == "http");
        // the last value without X-Forwarded-For to align it with
        REQUIRE(client_scheme(server, "10.0.0.1", {{"X-Forwarded-Proto", "https, http"}}) == "http");
        // a proxy appending to both: the value of the client, not the one it sent
        REQUIRE(client_scheme(server, "10.0.0.1", {{"X-Forwarded-For", "6.6.6.6, 203.0.113.9"},
                                                   {"X-Forwarded-Proto", "https, http"}}) == "http");
        REQUIRE(client_scheme(server, "10.0.0.1", {{"X-Forwarded-Proto", "ftp"}}) == "http");
        // spoofed by a peer that is not trusted
        REQUIRE(client_scheme(server, "203.0.113.9", {{"X-Forwarded-Proto", "https"}}) == "http");
        REQUIRE(client_scheme(server, "", {{"X-Forwarded-Proto", "https"}}) == "http");
        // only the header of the configured mode
        REQUIRE(client_scheme(server, "10.0.0.1", {{"Forwarded", "for=1.2.3.4;proto=https"}}) == "http");
    }

    SECTION("proto of the Forwarded header of a trusted proxy") {
        REQUIRE(server.set_trusted_proxies({"10.0.0.0/8"}, http::forwarded_header::forwarded));
        REQUIRE(client_scheme(server, "10.0.0.1", {{"Forwarded", "for=1.2.3.4;proto=https"}}) == "https");
        // the element of the client, not what it prepended
        REQUIRE(client_scheme(server, "10.0.0.1", {{"Forwarded", "for=9.9.9.9;proto=https, for=1.2.3.4;proto=http"}},
                              "https://localhost/ip") == "http");
        REQUIRE(client_scheme(server, "203.0.113.9", {{"Forwarded", "for=1.2.3.4;proto=https"}}) == "http");
        REQUIRE(client_scheme(server, "10.0.0.1", {{"X-Forwarded-Proto", "https"}}) == "http");
    }
}

TEST_CASE("Request scheme over a connection", "[server][proxies][integration]") {
    http::server server;
    add_ip_route(server);

    auto request_scheme = [&server](bool secure, bool trusted) {
        if (trusted) REQUIRE(server.set_trusted_proxies({"127.0.0.1"}));
        server.enable_ssl(secure);
        REQUIRE(server.listen("127.0.0.1", 0));
        std::thread thread([&server]() { server.wait(); });

        http::client client;
        client.verify_ssl(false);
        auto url = std::string(secure ? "https" : "http") + "://127.0.0.1:" + std::to_string(server.local_port()) + "/ip";
        auto response = client.get(url, {{"X-Forwarded-Proto", secure ? "http" : "https"}});
        server.stop();
        thread.join();
        REQUIRE(response.status() == 200);
        return nlohmann::json::parse(response.body())["scheme"].get<std::string>();
    };

    SECTION("HTTP, the header not trusted") {
        REQUIRE(request_scheme(false, false) == "http");
    }

    SECTION("HTTPS, the header not trusted") {
        REQUIRE(request_scheme(true, false) == "https");
    }

    SECTION("Local proxy trusted") {
        REQUIRE(request_scheme(false, true) == "https");
    }
}

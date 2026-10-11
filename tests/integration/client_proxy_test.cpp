#include <catch2/catch_test_macros.hpp>
#include <thinger/http_client.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/util/base64.hpp>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "../fixtures/test_ca.hpp"

using namespace thinger;
using namespace std::chrono_literals;

// The client through an HTTP proxy written here: plain requests forwarded in absolute-form,
// TLS and WebSockets through CONNECT tunnels, credentials only for the proxy, NO_PROXY and
// the environment variables. Origins have names under .test, which only the proxy resolves
// (to this host): a request reaching one went through it. 0.0.0.0 reaches this host too, and
// is not a loopback address: the proxy rules apply to it, and it can be reached directly.

namespace {

    using boost::asio::ip::tcp;

    // A forward proxy: absolute-form requests (one per connection) and CONNECT tunnels,
    // recording what it receives. *.test hosts are this host; CONNECT to denied.test is
    // refused (403), and never answered to stall.test.
    struct test_proxy {
        struct received {
            std::string method;
            std::string target;
            std::map<std::string, std::string> headers;     // lowercase names

            std::string header(const std::string& name) const {
                auto it = headers.find(name);
                return it == headers.end() ? std::string{} : it->second;
            }
        };

        boost::asio::io_context ioc;
        tcp::acceptor acceptor{ioc, {boost::asio::ip::make_address("127.0.0.1"), 0}};
        const std::string credentials;     // "user:password" required, if not empty
        std::mutex mutex;
        std::vector<received> requests;
        std::thread thread;

        explicit test_proxy(std::string required_credentials = {}) : credentials(std::move(required_credentials)) {
            co_spawn(ioc, accept_loop(), boost::asio::detached);
            thread = std::thread([this] { ioc.run(); });
        }

        ~test_proxy() {
            ioc.stop();
            if (thread.joinable()) thread.join();
        }

        uint16_t port() const { return acceptor.local_endpoint().port(); }

        std::string url(const std::string& user_info = {}) const {
            return "http://" + (user_info.empty() ? "" : user_info + "@") + "127.0.0.1:" + std::to_string(port());
        }

        std::vector<received> received_requests() {
            std::lock_guard lock(mutex);
            return requests;
        }

        static std::string resolve(const std::string& host) {
            if (host.ends_with(".test") || host == "localhost") return "127.0.0.1";
            return host;
        }

        awaitable<void> accept_loop() {
            while (true) {
                auto [ec, socket] = co_await acceptor.async_accept(use_nothrow_awaitable);
                if (ec) co_return;
                co_spawn(ioc, session(std::move(socket)), boost::asio::detached);
            }
        }

        static awaitable<void> write(tcp::socket& socket, std::string data) {
            co_await boost::asio::async_write(socket, boost::asio::buffer(data), use_nothrow_awaitable);
        }

        static awaitable<void> copy(tcp::socket& from, tcp::socket& to) {
            char buffer[8192];
            while (true) {
                auto [ec, bytes] = co_await from.async_read_some(boost::asio::buffer(buffer), use_nothrow_awaitable);
                if (ec) co_return;
                auto [write_ec, written] = co_await boost::asio::async_write(to, boost::asio::buffer(buffer, bytes),
                                                                             use_nothrow_awaitable);
                if (write_ec) co_return;
            }
        }

        awaitable<void> session(tcp::socket client) {
            std::string data;
            char buffer[4096];
            size_t head_end;
            while ((head_end = data.find("\r\n\r\n")) == std::string::npos) {
                auto [ec, bytes] = co_await client.async_read_some(boost::asio::buffer(buffer), use_nothrow_awaitable);
                if (ec) co_return;
                data.append(buffer, bytes);
            }
            std::string rest = data.substr(head_end + 4);
            std::vector<std::string> lines;
            for (size_t start = 0; start < head_end;) {
                auto end = std::min(data.find("\r\n", start), head_end);
                lines.push_back(data.substr(start, end - start));
                start = end + 2;
            }

            received request;
            auto first_space = lines[0].find(' ');
            auto second_space = lines[0].find(' ', first_space + 1);
            request.method = lines[0].substr(0, first_space);
            request.target = lines[0].substr(first_space + 1, second_space - first_space - 1);
            for (size_t i = 1; i < lines.size(); ++i) {
                auto colon = lines[i].find(':');
                auto name = lines[i].substr(0, colon);
                util::ascii::to_lower(name);
                auto value = lines[i].substr(colon + 1);
                while (!value.empty() && value.front() == ' ') value.erase(0, 1);
                request.headers[name] = value;
            }
            {
                std::lock_guard lock(mutex);
                requests.push_back(request);
            }

            if (!credentials.empty() && request.header("proxy-authorization") != "Basic " + util::base64::encode(credentials)) {
                co_await write(client, "HTTP/1.1 407 Proxy Authentication Required\r\n"
                                       "Proxy-Authenticate: Basic realm=\"test\"\r\nContent-Length: 12\r\n\r\nauthenticate");
                co_return;
            }

            std::string host, port, origin_head;
            if (request.method == "CONNECT") {
                auto colon = request.target.rfind(':');
                host = request.target.substr(0, colon);
                port = request.target.substr(colon + 1);
                if (host == "denied.test") {
                    co_await write(client, "HTTP/1.1 403 Forbidden\r\nContent-Length: 6\r\n\r\ndenied");
                    co_return;
                }
                if (host == "stall.test") {
                    co_await client.async_read_some(boost::asio::buffer(buffer), use_nothrow_awaitable);
                    co_return;
                }
            } else {
                if (!request.target.starts_with("http://")) {
                    co_await write(client, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
                    co_return;
                }
                auto path_start = request.target.find('/', 7);
                auto authority = request.target.substr(7, path_start - 7);
                auto colon = authority.rfind(':');
                host = authority.substr(0, colon);
                port = colon == std::string::npos ? "80" : authority.substr(colon + 1);
                // Sent on in origin-form, without the hop-by-hop headers, and closed after
                // the response
                origin_head = request.method + " " + (path_start == std::string::npos ? "/" : request.target.substr(path_start)) +
                              " HTTP/1.1\r\n";
                for (size_t i = 1; i < lines.size(); ++i) {
                    auto name = lines[i].substr(0, lines[i].find(':'));
                    util::ascii::to_lower(name);
                    if (name == "proxy-authorization" || name == "proxy-connection" || name == "connection") continue;
                    origin_head += lines[i] + "\r\n";
                }
                origin_head += "Connection: close\r\n\r\n";
            }

            tcp::socket origin(ioc);
            boost::system::error_code address_ec;
            auto address = boost::asio::ip::make_address(resolve(host), address_ec);
            if (address_ec) {
                co_await write(client, "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n");
                co_return;
            }
            auto [connect_ec] = co_await origin.async_connect({address, static_cast<uint16_t>(std::stoi(port))},
                                                              use_nothrow_awaitable);
            if (connect_ec) {
                co_await write(client, "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n");
                co_return;
            }
            if (request.method == "CONNECT") {
                co_await write(client, "HTTP/1.1 200 Connection established\r\n\r\n");
                if (!rest.empty()) co_await write(origin, rest);
            } else {
                co_await write(origin, origin_head + rest);
            }
            co_await (copy(client, origin) || copy(origin, client));
        }
    };

    // An origin server (on every address, so 0.0.0.0 reaches it), plain or TLS
    struct origin_server {
        http::server server;
        uint16_t port = 0;
        std::thread thread;
        bool tls;

        explicit origin_server(bool use_tls = false) : tls(use_tls) {
            if (tls) server.enable_ssl(true);
            server.get("/headers", [](http::request& req, http::response& res) {
                nlohmann::json json;
                auto request = req.get_http_request();
                json["uri"] = request->get_uri();
                json["headers"] = nlohmann::json::object();
                for (const auto& [name, value] : request->get_headers()) {
                    auto key = name;
                    util::ascii::to_lower(key);
                    json["headers"][key] = value;
                }
                res.json(json);
            });
            server.post("/echo", [](http::request& req, http::response& res) {
                res.send(req.get_http_request()->get_body());
            });
            server.get("/chunked", [](http::request&, http::response& res) {
                res.start_chunked("text/plain");
                res.write_chunk("one;");
                res.write_chunk("two;");
                res.write_chunk("three");
                res.end_chunked();
            });
            server.get("/redirect", [](http::request& req, http::response& res) {
                res.redirect(req.query("to"));
            });
            server.get("/ws", [](http::request&, http::response& res) {
                res.upgrade_websocket([](std::shared_ptr<http::websocket_connection> ws) {
                    ws->on_message([ws](std::string message, bool) {
                        ws->send_text("echo: " + message);
                    });
                });
            });
            REQUIRE(server.listen("0.0.0.0", 0));
            port = server.local_port();
            thread = std::thread([this] { server.wait(); });
        }

        ~origin_server() {
            server.stop();
            if (thread.joinable()) thread.join();
        }

        std::string url(const std::string& host, const std::string& path = "/headers") const {
            return std::string(tls ? "https://" : "http://") + host + ":" + std::to_string(port) + path;
        }
    };

    // Sets environment variables for a test, restoring them afterwards. Variables are set
    // before a client is created (it reads them then), while no request is in progress: getenv
    // and setenv are not thread-safe together. Each test case runs in a process of its own
    // under ctest.
    struct environment {
        std::vector<std::pair<std::string, std::optional<std::string>>> saved;

        environment() {
            // No proxy from the environment the tests run in
            for (auto name : {"http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY",
                              "no_proxy", "NO_PROXY"}) {
                set(name, std::nullopt);
            }
        }

        ~environment() {
            for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
                if (it->second) {
                    ::setenv(it->first.c_str(), it->second->c_str(), 1);
                } else {
                    ::unsetenv(it->first.c_str());
                }
            }
        }

        void set(const std::string& name, const std::optional<std::string>& value) {
            const char* previous = std::getenv(name.c_str());
            saved.emplace_back(name, previous ? std::optional<std::string>(previous) : std::nullopt);
            if (value) {
                ::setenv(name.c_str(), value->c_str(), 1);
            } else {
                ::unsetenv(name.c_str());
            }
        }
    };

    nlohmann::json json_of(const http::client_response& res) {
        return nlohmann::json::parse(res.body());
    }

    std::string basic(const std::string& credentials) {
        return "Basic " + util::base64::encode(credentials);
    }

    http::client_response async_get(http::async_client& client, const std::string& url) {
        http::client_response result;
        client.get(url, [&](http::client_response& res) { result = res; });
        client.wait();
        return result;
    }

}

TEST_CASE("HTTP client sends plain requests to the proxy in absolute-form", "[http][client][proxy][integration]") {
    test_proxy proxy("user@corp:p:ss");
    origin_server origin;
    auto url = origin.url("origin.test", "/headers?x=1");

    auto check = [&](const http::client_response& res) {
        REQUIRE(res.ok());
        auto json = json_of(res);
        REQUIRE(json["uri"] == "/headers?x=1");
        REQUIRE(json["headers"]["host"] == "origin.test:" + std::to_string(origin.port));
        // The credentials are for the proxy only
        REQUIRE_FALSE(json["headers"].contains("proxy-authorization"));

        auto received = proxy.received_requests();
        REQUIRE(received.size() == 1);
        REQUIRE(received[0].method == "GET");
        REQUIRE(received[0].target == url);
        REQUIRE(received[0].header("proxy-authorization") == basic("user@corp:p:ss"));
        REQUIRE(received[0].header("host") == "origin.test:" + std::to_string(origin.port));
    };

    SECTION("Sync client") {
        http::client client;
        client.timeout(5s).proxy(proxy.url("user%40corp:p%3Ass"));
        REQUIRE(client.proxy_for(url) == proxy.url());
        check(client.get(url));
    }

    SECTION("Async client") {
        http::async_client client;
        client.timeout(5s).proxy(proxy.url("user%40corp:p%3Ass"));
        check(async_get(client, url));
    }

    SECTION("POST with a body") {
        http::client client;
        client.timeout(5s).proxy(proxy.url("user%40corp:p%3Ass"));
        auto res = client.post(origin.url("origin.test", "/echo"), R"({"a":1})");
        REQUIRE(res.ok());
        REQUIRE(res.body() == R"({"a":1})");
        auto received = proxy.received_requests();
        REQUIRE(received.size() == 1);
        REQUIRE(received[0].method == "POST");
        REQUIRE(received[0].header("content-length") == "7");
    }
}

TEST_CASE("HTTP client reaches HTTPS origins through a CONNECT tunnel", "[http][client][proxy][tls][integration]") {
    http::test::test_ca ca;
    http::test::server_certificate certificate("origin.test", ca.issue("origin.test"));
    test_proxy proxy("user:secret");
    origin_server origin(true);
    auto url = origin.url("origin.test");
    auto authority = "origin.test:" + std::to_string(origin.port);

    SECTION("TLS end to end, verified for the origin, credentials only in CONNECT") {
        for (bool async : {false, true}) {
            http::client_response res;
            if (async) {
                http::async_client client;
                client.timeout(5s).proxy(proxy.url("user:secret")).add_ca_pem(ca.pem());
                res = async_get(client, url);
            } else {
                http::client client;
                client.timeout(5s).proxy(proxy.url("user:secret")).add_ca_pem(ca.pem());
                res = client.get(url);
            }
            REQUIRE(res.ok());
            auto json = json_of(res);
            REQUIRE(json["headers"]["host"] == authority);
            REQUIRE_FALSE(json["headers"].contains("proxy-authorization"));
        }
        auto received = proxy.received_requests();
        REQUIRE(received.size() == 2);
        for (const auto& request : received) {
            REQUIRE(request.method == "CONNECT");
            REQUIRE(request.target == authority);
            REQUIRE(request.header("host") == authority);
            REQUIRE(request.header("proxy-authorization") == basic("user:secret"));
        }
    }

    SECTION("A certificate of the private CA is not trusted by default") {
        http::client client;
        client.timeout(5s).proxy(proxy.url("user:secret"));
        auto res = client.get(url);
        REQUIRE_FALSE(res);
        REQUIRE(res.error_code().category() == boost::asio::error::get_ssl_category());
        REQUIRE(proxy.received_requests().size() == 1);
    }

    SECTION("The certificate must be issued for the origin, not for the proxy nor another name") {
        http::test::server_certificate other("wrong.test", ca.issue("origin.test"));
        http::client client;
        client.timeout(5s).proxy(proxy.url("user:secret")).add_ca_pem(ca.pem());
        auto res = client.get(origin.url("wrong.test"));
        REQUIRE_FALSE(res);
        REQUIRE(res.has_network_error());
    }

    SECTION("Verification can be disabled") {
        http::client client;
        client.timeout(5s).proxy(proxy.url("user:secret")).verify_ssl(false);
        REQUIRE(client.get(url).ok());
    }
}

TEST_CASE("HTTP client reports a proxy refusing the tunnel", "[http][client][proxy][integration]") {
    http::test::test_ca ca;
    http::test::server_certificate certificate("origin.test", ca.issue("origin.test"));
    test_proxy proxy("user:secret");
    origin_server origin(true);
    http::client client;
    client.timeout(5s).add_ca_pem(ca.pem());

    SECTION("407 without credentials, or with wrong ones") {
        for (auto proxy_url : {proxy.url(), proxy.url("user:wrong")}) {
            client.proxy(proxy_url);
            auto res = client.get(origin.url("origin.test"));
            REQUIRE_FALSE(res);
            REQUIRE(res.error_code() == http::client_error::proxy_authentication_required);
            REQUIRE(res.status() == 407);
            REQUIRE(res.header("Proxy-Authenticate") == "Basic realm=\"test\"");
            REQUIRE(res.body() == "authenticate");
        }
    }

    SECTION("When streaming") {
        client.proxy(proxy.url());
        auto request = client.create_request(http::method::GET, origin.url("origin.test"));
        auto result = client.send_streaming(request, [](const http::stream_info&) { return true; });
        REQUIRE_FALSE(result.ok());
        REQUIRE(result.error == boost::system::error_code(http::client_error::proxy_authentication_required).message());
        REQUIRE(result.status_code == 407);
    }

    SECTION("Another status") {
        client.proxy(proxy.url("user:secret"));
        auto res = client.get("https://denied.test:" + std::to_string(origin.port) + "/headers");
        REQUIRE(res.error_code() == http::client_error::proxy_tunnel_failed);
        REQUIRE(res.status() == 403);
    }

    SECTION("A plain request gets the 407 of the proxy as its response") {
        client.proxy(proxy.url());
        auto res = client.get("http://origin.test:" + std::to_string(origin.port) + "/headers");
        REQUIRE_FALSE(res.has_network_error());
        REQUIRE(res.status() == 407);
    }

    SECTION("The timeout covers the CONNECT") {
        client.proxy(proxy.url("user:secret")).timeout(1s);
        auto start = std::chrono::steady_clock::now();
        auto res = client.get("https://stall.test:443/");
        REQUIRE(res.error_code() == boost::asio::error::timed_out);
        REQUIRE(std::chrono::steady_clock::now() - start < 5s);
    }

    SECTION("An unusable proxy fails the request") {
        for (auto proxy_url : {"https://127.0.0.1:3128", "socks5://127.0.0.1:1080", "http://127.0.0.1:99999"}) {
            client.proxy(proxy_url);
            REQUIRE(client.get(origin.url("origin.test")).error_code() == http::client_error::invalid_proxy);
        }
        // Unless the host is reached directly
        client.proxy("https://127.0.0.1:3128", "0.0.0.0");
        client.verify_ssl(false);
        REQUIRE(client.get(origin.url("0.0.0.0")).ok());
    }
}

TEST_CASE("HTTP client reaches NO_PROXY hosts directly", "[http][client][proxy][integration]") {
    test_proxy proxy;
    origin_server origin;
    auto port = std::to_string(origin.port);
    http::client client;
    client.timeout(5s);

    struct no_proxy_case {
        std::string no_proxy;
        bool direct;
    };
    for (const auto& [no_proxy, direct] : std::vector<no_proxy_case>{
             {"0.0.0.0", true},
             {"example.com, 0.0.0.0/8", true},
             {"0.0.0.0:" + port, true},
             {" 0.0.0.0:1 ", false},
             {"*", true},
             {"example.com", false},
             {"", false}}) {
        INFO("NO_PROXY: " << no_proxy);
        client.proxy(proxy.url(), no_proxy);
        auto before = proxy.received_requests().size();
        REQUIRE(client.get(origin.url("0.0.0.0")).ok());
        REQUIRE(proxy.received_requests().size() == before + (direct ? 0 : 1));
    }

    // Domains, with or without a leading dot, and their subdomains
    client.proxy(proxy.url(), "origin.test");
    REQUIRE(client.proxy_for(origin.url("origin.test")).empty());
    REQUIRE(client.proxy_for(origin.url("api.origin.test")).empty());
    REQUIRE(client.proxy_for(origin.url("other.test")) == proxy.url());
    client.proxy(proxy.url(), ".origin.test");
    REQUIRE(client.proxy_for(origin.url("origin.test")).empty());
    REQUIRE(client.proxy_for(origin.url("api.origin.test")).empty());

    SECTION("localhost and loopback addresses never use the proxy") {
        client.proxy(proxy.url());
        REQUIRE(client.get(origin.url("localhost")).ok());
        REQUIRE(client.get(origin.url("127.0.0.1")).ok());
        auto before = proxy.received_requests().size();
        REQUIRE(client.get(origin.url("0.0.0.0")).ok());
        REQUIRE(proxy.received_requests().size() == before + 1);
        for (const auto& request : proxy.received_requests()) {
            REQUIRE(request.target.find("localhost") == std::string::npos);
            REQUIRE(request.target.find("127.0.0.1") == std::string::npos);
        }
    }
}

TEST_CASE("HTTP client takes the proxy from the environment", "[http][client][proxy][env][integration]") {
    environment env;
    test_proxy proxy;
    origin_server origin;
    auto url = origin.url("0.0.0.0");

    SECTION("Uppercase HTTP_PROXY is ignored (httpoxy), lowercase http_proxy is used") {
        env.set("HTTP_PROXY", proxy.url());
        {
            http::client client;
            REQUIRE(client.proxy_for(url).empty());
            REQUIRE(client.get(url).ok());
            REQUIRE(proxy.received_requests().empty());
        }
        env.set("http_proxy", proxy.url());
        http::client client;
        REQUIRE(client.proxy_for(url) == proxy.url());
        REQUIRE(client.get(url).ok());
        REQUIRE(proxy.received_requests().size() == 1);
    }

    SECTION("HTTPS_PROXY, NO_PROXY and ALL_PROXY") {
        env.set("HTTPS_PROXY", proxy.url());
        env.set("ALL_PROXY", "http://all.example:3128");
        env.set("NO_PROXY", "internal.example");
        http::client client;
        REQUIRE(client.proxy_for("https://example.com/") == proxy.url());
        REQUIRE(client.proxy_for("http://example.com/") == "http://all.example:3128");
        REQUIRE(client.proxy_for("https://www.internal.example/").empty());
    }

    SECTION("The client and request settings come first") {
        env.set("http_proxy", "http://127.0.0.1:1");    // nothing listens there
        http::client client;
        client.timeout(5s);
        client.proxy(proxy.url());
        REQUIRE(client.get(url).ok());
        REQUIRE(proxy.received_requests().size() == 1);

        client.proxy("");
        REQUIRE(client.get(url).ok());
        REQUIRE(proxy.received_requests().size() == 1);

        client.proxy("http://127.0.0.1:1");
        REQUIRE(client.request(url).proxy(proxy.url()).get().ok());
        REQUIRE(proxy.received_requests().size() == 2);
        REQUIRE(client.request(url).proxy("").get().ok());
        REQUIRE(proxy.received_requests().size() == 2);
    }

    SECTION("The environment can be ignored") {
        env.set("http_proxy", proxy.url());
        http::client client;
        client.proxy_from_environment(false);
        REQUIRE(client.get(url).ok());
        REQUIRE(proxy.received_requests().empty());
        client.proxy_from_environment(true);
        REQUIRE(client.get(url).ok());
        REQUIRE(proxy.received_requests().size() == 1);
    }
}

TEST_CASE("HTTP client opens WebSockets through the proxy", "[http][client][proxy][websocket][integration]") {
    http::test::test_ca ca;
    http::test::server_certificate certificate("origin.test", ca.issue("origin.test"));
    test_proxy proxy("user:secret");

    for (bool tls : {false, true}) {
        INFO((tls ? "wss" : "ws"));
        origin_server origin(tls);
        auto authority = "origin.test:" + std::to_string(origin.port);
        auto before = proxy.received_requests().size();

        http::client client;
        client.timeout(5s).proxy(proxy.url("user:secret")).add_ca_pem(ca.pem());
        auto ws = client.websocket(std::string(tls ? "wss://" : "ws://") + authority + "/ws");
        REQUIRE(ws);
        REQUIRE(ws->send_text("hello"));
        auto [message, binary] = ws->receive();
        REQUIRE(message == "echo: hello");
        ws->close();

        // A tunnel for ws:// too, as browsers do
        auto received = proxy.received_requests();
        REQUIRE(received.size() == before + 1);
        REQUIRE(received.back().method == "CONNECT");
        REQUIRE(received.back().target == authority);
        REQUIRE(received.back().header("proxy-authorization") == basic("user:secret"));
    }

    SECTION("Async client, with the request builder") {
        origin_server origin(true);
        http::async_client client;
        client.timeout(5s).proxy(proxy.url("user:secret"));
        std::string echo;
        client.run([&]() -> awaitable<void> {
            auto ws = co_await client.request(origin.url("origin.test", "/ws")).ca_pem(ca.pem()).websocket();
            if (!ws) co_return;
            co_await ws->send_text_async("async");
            auto [message, binary] = co_await ws->receive_async();
            echo = message;
            co_await ws->close_async();
        });
        client.wait();
        REQUIRE(echo == "echo: async");
    }
}

TEST_CASE("HTTP client decides the proxy again for each redirect", "[http][client][proxy][redirect][integration]") {
    test_proxy proxy;
    origin_server origin;
    auto direct_url = origin.url("0.0.0.0");
    auto proxied_url = origin.url("origin.test");
    http::client client;
    client.timeout(5s);

    SECTION("From a proxied host to a NO_PROXY one") {
        client.proxy(proxy.url(), "0.0.0.0");
        auto res = client.get(origin.url("origin.test", "/redirect?to=" + direct_url));
        REQUIRE(res.ok());
        REQUIRE(json_of(res)["headers"]["host"] == "0.0.0.0:" + std::to_string(origin.port));
        auto received = proxy.received_requests();
        REQUIRE(received.size() == 1);
        REQUIRE(received[0].target.find("/redirect") != std::string::npos);
    }

    SECTION("From a NO_PROXY host to a proxied one, with the settings of the request") {
        client.proxy("");
        auto res = client.request(origin.url("0.0.0.0", "/redirect?to=" + proxied_url)).proxy(proxy.url(), "0.0.0.0").get();
        REQUIRE(res.ok());
        REQUIRE(json_of(res)["headers"]["host"] == "origin.test:" + std::to_string(origin.port));
        auto received = proxy.received_requests();
        REQUIRE(received.size() == 1);
        REQUIRE(received[0].target == proxied_url);
    }
}

TEST_CASE("HTTP client never shares a connection between direct and proxied requests", "[http][client][proxy][pool][integration]") {
    test_proxy proxy;
    origin_server origin;
    auto url = origin.url("0.0.0.0");

    http::async_client client;
    client.timeout(5s).proxy(proxy.url());
    std::shared_ptr<http::client_connection> direct, proxied, direct_again;
    bool all_ok = false;
    client.run([&]() -> awaitable<void> {
        auto request = client.create_request(http::method::GET, url);
        request->set_proxy_config(http::proxy_config{});
        auto [first, first_connection] = co_await client.send_with_connection(request);
        direct = first_connection;

        // The direct connection is idle and open: only the route keeps it from this request
        auto [second, second_connection] = co_await client.send_with_connection(client.create_request(http::method::GET, url));
        proxied = second_connection;

        request = client.create_request(http::method::GET, url);
        request->set_proxy_config(http::proxy_config{});
        auto [third, third_connection] = co_await client.send_with_connection(request);
        direct_again = third_connection;
        all_ok = first.ok() && second.ok() && third.ok();
    });
    client.wait();

    REQUIRE(all_ok);
    REQUIRE(direct);
    REQUIRE(proxied);
    REQUIRE(direct != proxied);
    REQUIRE(direct_again == direct);
    REQUIRE(proxy.received_requests().size() == 1);
}

TEST_CASE("HTTP client streams chunked responses through the proxy", "[http][client][proxy][streaming][integration]") {
    http::test::test_ca ca;
    http::test::server_certificate certificate("origin.test", ca.issue("origin.test"));
    test_proxy proxy;

    for (bool tls : {false, true}) {
        INFO((tls ? "tunnel" : "forwarded"));
        origin_server origin(tls);
        http::client client;
        client.timeout(5s).proxy(proxy.url()).add_ca_pem(ca.pem());
        auto url = origin.url("origin.test", "/chunked");

        // Streaming callback
        std::string streamed;
        auto result = client.request(url).get([&](const http::stream_info& info) {
            streamed.append(info.data);
            return true;
        });
        REQUIRE(result.ok());
        REQUIRE(streamed == "one;two;three");

        // Chunked callback of the request
        std::vector<std::pair<int, std::string>> events;
        auto request = client.create_request(http::method::GET, url);
        request->set_header("Accept-Encoding", "identity");
        request->set_chunked_callback([&](int event, const std::string& data) { events.emplace_back(event, data); });
        REQUIRE(client.send(request).ok());
        std::string chunks;
        for (const auto& [event, data] : events) {
            if (event == 1) chunks += data;
        }
        REQUIRE(events.front().first == 0);
        REQUIRE(events.back().first == 2);
        REQUIRE(chunks == "one;two;three");
    }

    bool forwarded = false, tunneled = false;
    for (const auto& request : proxy.received_requests()) {
        forwarded |= request.method == "GET" && request.target.starts_with("http://origin.test:");
        tunneled |= request.method == "CONNECT" && request.target.starts_with("origin.test:");
    }
    REQUIRE(forwarded);
    REQUIRE(tunneled);
}

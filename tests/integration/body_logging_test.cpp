#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/async_client.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/reverse_proxy.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/websocket_connection.hpp>
#include <thinger/util/logger.hpp>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <string>
#include <string_view>
#include <vector>

#include "../fixtures/running_workers.hpp"
#include "server_test_helpers.hpp"

// The logs of the HTTP messages never show what may be a credential: bodies, WebSocket
// messages and header values are left out by default, and the credential headers are
// redacted even when the bodies are logged (an opt-in)

using namespace thinger;
using server_test::running_server;
using thinger::http::util::url::redacted_parameters;

namespace {

    const std::string token = "token-c1d2e3f4";
    const std::string request_cookie = "request-cookie-a9b8c7";
    const std::string response_cookie = "response-cookie-5f6e7d";
    const std::string request_body = "request-body-password=hunter2-and-more-request-body-text";
    const std::string response_body = "response-body-BEGIN-PRIVATE-KEY-and-more-response-body-text";
    const std::string ws_payload = "websocket-payload-secret-4a5b6c";

    // Every message of the library logger, at trace level, while alive
    struct log_capture {
        std::shared_ptr<spdlog::logger> previous = logging::get_logger();
        std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink =
            std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(4096);

        log_capture() {
            auto logger = std::make_shared<spdlog::logger>("http", sink);
            logger->set_pattern("%v");
            logger->set_level(spdlog::level::trace);
            logging::set_logger(logger);
        }
        ~log_capture() { logging::set_logger(previous); }

        std::vector<std::string> lines() const { return sink->last_formatted(); }

        bool contains(const std::string& text) const {
            auto all = lines();
            return std::any_of(all.begin(), all.end(),
                               [&](const std::string& line) { return line.find(text) != std::string::npos; });
        }

        std::string dump() const {
            std::string out;
            for (const auto& line : lines()) out += line;
            return out;
        }
    };

    void routes(http::server& server) {
        server.post("/login", [](http::request& req, http::response& res) {
            res.header("Set-Cookie", "session=" + response_cookie + "; HttpOnly");
            res.header("X-Visible", "visible-value");
            res.send(req.body() == request_body ? response_body : std::string("unexpected body"));
        });
        server.get("/data", [](http::request&, http::response& res) { res.send("ok"); });
        server.get("/echo", [](http::request&, http::response& res) {
            res.upgrade_websocket({}, [](std::shared_ptr<http::websocket_connection> ws) {
                ws->on_message([ws](std::string message, bool binary) {
                    if (binary) ws->send_binary(std::move(message));
                    else ws->send_text(std::move(message));
                });
            });
        });
    }

    http::client_response login(http::client& client, uint16_t port) {
        return client.request("http://127.0.0.1:" + std::to_string(port) + "/login")
            .header("Authorization", "Bearer " + token)
            .header("Cookie", "id=" + request_cookie)
            .body(request_body, "text/plain")
            .post();
    }

    void require_no_secret(const log_capture& logs) {
        INFO("captured logs:\n" << logs.dump());
        for (const auto& line : logs.lines()) {
            REQUIRE(line.find(token) == std::string::npos);
            REQUIRE(line.find(request_cookie) == std::string::npos);
            REQUIRE(line.find(response_cookie) == std::string::npos);
        }
    }

    redacted_parameters defaults_and(std::string_view name) {
        auto names = redacted_parameters::defaults();
        names.add(name);
        return names;
    }

    void answer_ok(http::server& server) {
        for (const char* path : {"/x", "/join"}) {
            server.get(path, [](http::request&, http::response& res) { res.send("ok"); });
        }
    }

    void redact_on(http::server& server, const redacted_parameters& names) {
        http::connection_options options;
        options.logging.redact = names;
        server.set_connection_options(options);
    }

    // The request line of a server redacting `names`, for a request for `target`
    void require_server_line(const redacted_parameters& names, const std::string& target, const std::string& line) {
        http::server server;
        answer_ok(server);
        redact_on(server, names);
        // The logger set before the server thread logs, and restored after it ends
        log_capture logs;
        running_server running(server);

        http::client client;
        REQUIRE(client.get(running.url(target)).status() == 200);

        INFO("captured logs:\n" << logs.dump());
        REQUIRE(logs.contains("[SERVER REQUEST] " + line));
    }

    // The request line of a client redacting `names`, for a request for `target`
    void require_client_line(const redacted_parameters& names, const std::string& target, const std::string& line) {
        http::server server;
        answer_ok(server);
        log_capture logs;
        running_server running(server);

        http::client client;
        client.redact_parameters(names);
        REQUIRE(client.get(running.url(target)).status() == 200);

        INFO("captured logs:\n" << logs.dump());
        REQUIRE(logs.contains("[CLIENT->] " + line));
    }

}

TEST_CASE("HTTP logs show no body, header value or credential by default", "[logging][integration]") {
    http::server server;
    routes(server);
    // The logger set before the server thread logs, and restored after it ends
    log_capture logs;
    running_server running(server);

    http::client client;
    auto res = login(client, running.port);
    REQUIRE(res.status() == 200);
    REQUIRE(res.body() == response_body);

    INFO("captured logs:\n" << logs.dump());
    require_no_secret(logs);
    for (const auto& line : logs.lines()) {
        REQUIRE(line.find("request-body") == std::string::npos);
        REQUIRE(line.find("response-body") == std::string::npos);
        REQUIRE(line.find("visible-value") == std::string::npos);
    }

    // The request lines, the status, the header names and the body sizes are logged
    REQUIRE(logs.contains("[CLIENT->] POST /login"));
    REQUIRE(logs.contains("[SERVER REQUEST] POST /login"));
    REQUIRE(logs.contains("[SERVER RESPONSE] HTTP/1.1 200 OK"));
    REQUIRE(logs.contains("[HTTP CLIENT RESPONSE] HTTP/1.1 200 OK"));
    REQUIRE(logs.contains("  Authorization"));
    REQUIRE(logs.contains("  Set-Cookie"));
    REQUIRE(logs.contains("Body: " + std::to_string(request_body.size()) + " bytes"));
    REQUIRE(logs.contains("Body: " + std::to_string(response_body.size()) + " bytes"));
}

TEST_CASE("WebSocket messages are not logged, only their size", "[logging][websocket][integration]") {
    http::server server;
    routes(server);
    // The logger set before the server thread logs, and restored after it ends
    log_capture logs;
    running_server running(server);

    http::client client;
    auto ws = client.websocket("ws://127.0.0.1:" + std::to_string(running.port) + "/echo");
    REQUIRE(ws);
    REQUIRE(ws->send_text(ws_payload));
    auto [received, binary] = ws->receive();
    REQUIRE_FALSE(binary);
    REQUIRE(received == ws_payload);
    ws->close();

    INFO("captured logs:\n" << logs.dump());
    for (const auto& line : logs.lines()) {
        REQUIRE(line.find(ws_payload) == std::string::npos);
        REQUIRE(line.find("websocket-payload") == std::string::npos);
    }
    REQUIRE(logs.contains("websocket text message received: " + std::to_string(ws_payload.size()) + " bytes"));
}

TEST_CASE("Logged bodies are truncated and credential headers stay redacted", "[logging][integration]") {
    constexpr size_t max_length = 16;
    http::server server;
    routes(server);
    http::connection_options options;
    options.logging = {true, max_length};
    server.set_connection_options(options);
    // The logger set before the server thread logs, and restored after it ends
    log_capture logs;
    running_server running(server);

    http::client client;
    client.log_bodies(true, max_length);
    auto res = login(client, running.port);
    REQUIRE(res.status() == 200);
    REQUIRE(res.body() == response_body);

    INFO("captured logs:\n" << logs.dump());
    require_no_secret(logs);

    // The start of each body, and how much is left out
    REQUIRE(logs.contains("  " + request_body.substr(0, max_length) + "... (" +
                          std::to_string(request_body.size() - max_length) + " more bytes)"));
    REQUIRE(logs.contains("  " + response_body.substr(0, max_length) + "... (" +
                          std::to_string(response_body.size() - max_length) + " more bytes)"));
    for (const auto& line : logs.lines()) {
        REQUIRE(line.find(request_body.substr(0, max_length + 1)) == std::string::npos);
        REQUIRE(line.find(response_body.substr(0, max_length + 1)) == std::string::npos);
    }

    // Header values, but those of the credential headers
    REQUIRE(logs.contains("  X-Visible: visible-value"));
    REQUIRE(logs.contains("  Authorization: [redacted]"));
    REQUIRE(logs.contains("  Cookie: [redacted]"));
    REQUIRE(logs.contains("  Set-Cookie: [redacted]"));
}

TEST_CASE("Two servers of one process log each as configured", "[logging][connection_options][integration]") {
    // One logs the header values and the bodies, the other keeps the default (off)
    http::server logging_server;
    http::connection_options options;
    options.logging = {true, 1024};
    logging_server.set_connection_options(options);
    http::server quiet_server;
    for (auto* server : {&logging_server, &quiet_server}) {
        server->post("/reply", [](http::request& req, http::response& res) { res.send("reply-to-" + req.body()); });
    }
    log_capture logs;
    running_server logging_running(logging_server);
    running_server quiet_running(quiet_server);

    http::client client;
    auto post = [&](uint16_t port, const std::string& name) {
        return client.request("http://127.0.0.1:" + std::to_string(port) + "/reply")
            .header("X-Server", name + "-server")
            .body(name + "-body", "text/plain")
            .post();
    };
    REQUIRE(post(logging_running.port, "logging").body() == "reply-to-logging-body");
    REQUIRE(post(quiet_running.port, "quiet").body() == "reply-to-quiet-body");

    INFO("captured logs:\n" << logs.dump());
    REQUIRE(logs.contains("  X-Server: logging-server"));
    REQUIRE(logs.contains("  reply-to-logging-body"));
    REQUIRE_FALSE(logs.contains("quiet-server"));
    REQUIRE_FALSE(logs.contains("reply-to-quiet-body"));
    // The quiet one logs the names and sizes alone
    REQUIRE(logs.contains("  X-Server"));
    REQUIRE(logs.contains("Body: " + std::to_string(std::string("reply-to-quiet-body").size()) + " bytes"));
}

TEST_CASE("Logged bodies cannot forge log lines", "[logging][unit]") {
    log_capture logs;
    http::http_request request;
    request.set_method(http::method::POST);
    request.set_uri("/forge");
    request.add_header("Proxy-Authorization", "Basic " + token);
    request.set_content("line\r\nforged line\x01");

    request.log("TEST", http::body_logging{true, 1024});
    INFO("captured logs:\n" << logs.dump());
    require_no_secret(logs);
    REQUIRE(logs.contains("  Proxy-Authorization: [redacted]"));
    REQUIRE(logs.contains("  line\\r\\nforged line\\x01"));

    // Without the opt-in: the size alone
    log_capture quiet;
    request.log("TEST");
    INFO("captured logs:\n" << quiet.dump());
    REQUIRE(quiet.contains("Body: 18 bytes"));
    REQUIRE_FALSE(quiet.contains("forged"));
}

TEST_CASE("Credentials in the query string are redacted from the request lines", "[logging][integration]") {
    http::server server;
    routes(server);
    // The logger set before the server thread logs, and restored after it ends
    log_capture logs;
    running_server running(server);

    http::client client;
    auto res = client.get("http://127.0.0.1:" + std::to_string(running.port) + "/data?authorization=" + token + "&count=10");
    REQUIRE(res.status() == 200);

    require_no_secret(logs);
    REQUIRE(logs.contains("[CLIENT->] GET /data?authorization=[redacted]&count=10"));
    REQUIRE(logs.contains("[SERVER REQUEST] GET /data?authorization=[redacted]&count=10"));
}

TEST_CASE("One-time codes in the query string are redacted by default", "[logging][integration]") {
    http::server server;
    server.get("/cb", [](http::request&, http::response& res) { res.send("ok"); });
    // The logger set before the server thread logs, and restored after it ends
    log_capture logs;
    running_server running(server);

    http::client client;
    auto res = client.get("http://127.0.0.1:" + std::to_string(running.port) + "/cb?code=abc&state=s");
    REQUIRE(res.status() == 200);

    INFO("captured logs:\n" << logs.dump());
    REQUIRE(logs.contains("[SERVER REQUEST] GET /cb?code=[redacted]&state=s"));
    REQUIRE(logs.contains("[CLIENT->] GET /cb?code=[redacted]&state=s"));
    REQUIRE_FALSE(logs.contains("abc"));
}

TEST_CASE("No request line is logged with the info level off", "[logging][integration]") {
    http::server server;
    server.get("/cb", [](http::request&, http::response& res) { res.send("ok"); });
    // The logger set before the server thread logs, and restored after it ends
    log_capture logs;
    logging::get_logger()->set_level(spdlog::level::warn);
    running_server running(server);

    http::client client;
    auto res = client.get("http://127.0.0.1:" + std::to_string(running.port) + "/cb?code=abc&state=s");
    REQUIRE(res.status() == 200);

    INFO("captured logs:\n" << logs.dump());
    REQUIRE_FALSE(logs.contains("[SERVER REQUEST]"));
    REQUIRE_FALSE(logs.contains("[CLIENT->]"));
}

TEST_CASE("Connection options and clients start from the default set of names", "[logging][integration]") {
    http::connection_options options;
    REQUIRE(options.logging.redact.contains("code"));
    REQUIRE(options.logging.redact.size() == redacted_parameters::defaults().size());

    // The two fields of the opt-in are set as before, which also goes back to the defaults
    options.logging.redact = {"invite"};
    options.logging = {true, 512};
    REQUIRE(options.logging.enabled);
    REQUIRE(options.logging.max_length == 512);
    REQUIRE(options.logging.redact.contains("code"));
    REQUIRE_FALSE(options.logging.redact.contains("invite"));

    http::client client;
    REQUIRE(client.get_body_logging().redact.contains("code"));
}

TEST_CASE("A server redacts the names of its connection options", "[logging][connection_options][integration]") {
    SECTION("the defaults and one more") {
        require_server_line(defaults_and("invite"), "/join?invite=xyz&code=abc&page=2",
                            "GET /join?invite=[redacted]&code=[redacted]&page=2");
    }
    SECTION("the defaults but one") {
        auto names = redacted_parameters::defaults();
        names.remove("key");
        require_server_line(names, "/x?key=visible&token=t", "GET /x?key=visible&token=[redacted]");
    }
    SECTION("a set of its own, which replaces the defaults") {
        require_server_line({"invite"}, "/x?invite=a&code=b", "GET /x?invite=[redacted]&code=b");
    }
    SECTION("no name at all") {
        require_server_line({}, "/x?code=b&token=t", "GET /x?code=b&token=t");
    }
    SECTION("without case, on both sides") {
        require_server_line({"Invite"}, "/x?INVITE=a", "GET /x?INVITE=[redacted]");
    }
    SECTION("whether it logs the header values and the bodies or not") {
        http::server server;
        answer_ok(server);
        http::connection_options options;
        options.logging.enabled = true;
        options.logging.redact = {"invite"};
        server.set_connection_options(options);
        log_capture logs;
        running_server running(server);

        http::client client;
        REQUIRE(client.get(running.url("/x?invite=a&code=b")).status() == 200);

        INFO("captured logs:\n" << logs.dump());
        REQUIRE(logs.contains("[SERVER REQUEST] GET /x?invite=[redacted]&code=b"));
    }
}

TEST_CASE("A client redacts the names set on it", "[logging][integration]") {
    SECTION("the defaults and one more") {
        require_client_line(defaults_and("invite"), "/join?invite=xyz", "GET /join?invite=[redacted]");
    }
    SECTION("a set of its own, which replaces the defaults") {
        require_client_line({"invite"}, "/x?invite=a&code=b", "GET /x?invite=[redacted]&code=b");
    }
    SECTION("no name at all") {
        require_client_line({}, "/x?code=b", "GET /x?code=b");
    }
    SECTION("without case, on both sides") {
        require_client_line({"Invite"}, "/x?INVITE=a", "GET /x?INVITE=[redacted]");
    }
}

TEST_CASE("An async_client redacts the names set on it", "[logging][integration]") {
    http::server server;
    answer_ok(server);
    log_capture logs;
    running_server running(server);
    http::test::running_workers workers;

    http::async_client client(workers);
    client.redact_parameters(defaults_and("invite"));
    std::atomic<int> status{0};
    client.get(running.url("/join?invite=xyz"), [&](http::client_response& res) { status = res.status(); });
    client.wait();
    REQUIRE(status == 200);

    INFO("captured logs:\n" << logs.dump());
    REQUIRE(logs.contains("[CLIENT->] GET /join?invite=[redacted]"));
    // Its own set: the server, with the defaults, shows it
    REQUIRE(logs.contains("[SERVER REQUEST] GET /join?invite=xyz"));
    REQUIRE_FALSE(logs.contains("[CLIENT->] GET /join?invite=xyz"));
}

TEST_CASE("log_bodies() keeps the names of a client", "[logging][integration]") {
    http::server server;
    answer_ok(server);
    log_capture logs;
    running_server running(server);

    http::client client;
    client.redact_parameters({"invite"}).log_bodies(true, 64);
    REQUIRE(client.get_body_logging().enabled);
    REQUIRE(client.get_body_logging().max_length == 64);
    REQUIRE(client.get(running.url("/x?invite=a&code=b")).status() == 200);
    client.log_bodies(false);
    REQUIRE_FALSE(client.get_body_logging().enabled);
    REQUIRE(client.get(running.url("/x?invite=c&code=d")).status() == 200);

    INFO("captured logs:\n" << logs.dump());
    REQUIRE(logs.contains("[CLIENT->] GET /x?invite=[redacted]&code=b"));
    REQUIRE(logs.contains("[CLIENT->] GET /x?invite=[redacted]&code=d"));
}

TEST_CASE("A new set of a client applies from its next request, on a pooled connection too", "[logging][integration]") {
    http::server server;
    answer_ok(server);
    log_capture logs;
    running_server running(server);

    http::client client;
    client.redact_parameters({});
    REQUIRE(client.get(running.url("/x?invite=first")).status() == 200);
    client.redact_parameters({"invite"});
    REQUIRE(client.get(running.url("/x?invite=second")).status() == 200);

    INFO("captured logs:\n" << logs.dump());
    REQUIRE(logs.contains("Reusing connection from pool"));
    REQUIRE(logs.contains("[CLIENT->] GET /x?invite=first"));
    REQUIRE(logs.contains("[CLIENT->] GET /x?invite=[redacted]"));
    REQUIRE_FALSE(logs.contains("[CLIENT->] GET /x?invite=second"));
}

TEST_CASE("Two servers of one process redact each its own names", "[logging][connection_options][integration]") {
    http::server plain;
    http::server custom;
    answer_ok(plain);
    answer_ok(custom);
    redact_on(custom, defaults_and("invite"));
    log_capture logs;
    running_server plain_running(plain);
    running_server custom_running(custom);

    // The `via` tells the servers apart in the log
    http::client client;
    REQUIRE(client.get(plain_running.url("/x?invite=a&via=plain")).status() == 200);
    REQUIRE(client.get(custom_running.url("/x?invite=a&via=custom")).status() == 200);

    INFO("captured logs:\n" << logs.dump());
    REQUIRE(logs.contains("[SERVER REQUEST] GET /x?invite=a&via=plain"));
    REQUIRE(logs.contains("[SERVER REQUEST] GET /x?invite=[redacted]&via=custom"));
    REQUIRE_FALSE(logs.contains("[SERVER REQUEST] GET /x?invite=[redacted]&via=plain"));
    REQUIRE_FALSE(logs.contains("[SERVER REQUEST] GET /x?invite=a&via=custom"));
}

TEST_CASE("A set changed after listen() applies to new connections alone", "[logging][connection_options][integration]") {
    http::server server;
    answer_ok(server);
    log_capture logs;
    running_server running(server);

    http::client open;
    REQUIRE(open.get(running.url("/x?invite=before")).status() == 200);

    redact_on(server, {"invite"});

    // The connection already open keeps the set it was accepted with
    REQUIRE(open.get(running.url("/x?invite=open")).status() == 200);
    // A new one takes the new set
    http::client fresh;
    REQUIRE(fresh.get(running.url("/x?invite=fresh")).status() == 200);

    INFO("captured logs:\n" << logs.dump());
    REQUIRE(logs.contains("Reusing connection from pool"));
    REQUIRE(logs.contains("[SERVER REQUEST] GET /x?invite=before"));
    REQUIRE(logs.contains("[SERVER REQUEST] GET /x?invite=open"));
    REQUIRE(logs.contains("[SERVER REQUEST] GET /x?invite=[redacted]"));
    REQUIRE_FALSE(logs.contains("[SERVER REQUEST] GET /x?invite=fresh"));
}

namespace {

    // A WebSocket endpoint, for the logs of opening it
    void answer_websocket(http::server& server) {
        server.get("/ws", [](http::request&, http::response& res) {
            res.upgrade_websocket({}, [](std::shared_ptr<http::websocket_connection>) {});
        });
    }

    // Answers with `location` as its Location and Content-Location
    void answer_location(http::server& server, const std::string& location) {
        server.get("/moved", [location](http::request&, http::response& res) {
            res.status(http::http_response::status::moved_temporarily);
            res.header("Location", location);
            res.header("Content-Location", location);
            res.send("moved");
        });
    }

    http::client_response get_moved(http::client& client, const running_server& running, const std::string& referer) {
        client.follow_redirects(false);
        return client.request(running.url("/moved")).header("Referer", referer).get();
    }

    void require_no_line_with(const log_capture& logs, const std::string& text) {
        INFO("captured logs:\n" << logs.dump());
        REQUIRE_FALSE(logs.contains(text));
    }

}

TEST_CASE("Opening a WebSocket logs its URL redacted", "[logging][websocket][integration]") {
    http::server server;
    answer_websocket(server);
    log_capture logs;
    running_server running(server);
    const std::string host = "ws://127.0.0.1:" + std::to_string(running.port);

    SECTION("with the defaults") {
        http::client client;
        REQUIRE(client.websocket(host + "/ws?token=t0k3n&room=1"));
        INFO("captured logs:\n" << logs.dump());
        REQUIRE(logs.contains("WebSocket connected to " + host + "/ws?token=[redacted]&room=1"));
        require_no_line_with(logs, "t0k3n");
    }
    SECTION("with the set of the client") {
        http::client client;
        client.redact_parameters(defaults_and("invite"));
        REQUIRE(client.websocket(host + "/ws?invite=x-v4lue"));
        INFO("captured logs:\n" << logs.dump());
        REQUIRE(logs.contains("WebSocket connected to " + host + "/ws?invite=[redacted]"));
        REQUIRE(logs.contains("[CLIENT->] GET /ws?invite=[redacted]"));
        REQUIRE_FALSE(logs.contains("WebSocket connected to " + host + "/ws?invite=x-v4lue"));
        REQUIRE_FALSE(logs.contains("[CLIENT->] GET /ws?invite=x-v4lue"));
    }
    SECTION("with a URL given as http://") {
        http::client client;
        REQUIRE(client.websocket("http://127.0.0.1:" + std::to_string(running.port) + "/ws?code=abc"));
        INFO("captured logs:\n" << logs.dump());
        REQUIRE(logs.contains("WebSocket connected to " + host + "/ws?code=[redacted]"));
        require_no_line_with(logs, "abc");
    }
}

TEST_CASE("Opening a WebSocket over a Unix socket logs with the set of the client", "[logging][websocket][unix][integration]") {
    http::server server;
    answer_websocket(server);
    log_capture logs;
    running_server running(server, server_test::transport::unix);

    http::client client;
    client.redact_parameters({"invite"}).unix_socket(running.listener.endpoint().unix_path);
    REQUIRE(client.websocket("ws://localhost/ws?invite=x-v4lue&code=b"));

    INFO("captured logs:\n" << logs.dump());
    REQUIRE(logs.contains("[CLIENT->] GET /ws?invite=[redacted]&code=b"));
    REQUIRE(logs.contains("WebSocket connected to ws://localhost:80/ws?invite=[redacted]&code=b"));
    REQUIRE_FALSE(logs.contains("[CLIENT->] GET /ws?invite=x-v4lue"));
}

TEST_CASE("An invalid WebSocket URL is logged with the set of the client", "[logging][websocket][integration]") {
    log_capture logs;
    http::client client;
    client.redact_parameters({"invite"});

    auto request = std::make_shared<http::http_request>();
    request->set_host("127.0.0.1");
    request->set_port("not-a-port");
    request->set_uri("/ws?invite=x-v4lue&code=b");
    REQUIRE_FALSE(client.websocket(request));

    INFO("captured logs:\n" << logs.dump());
    REQUIRE(logs.contains("Invalid WebSocket URL: http://127.0.0.1:not-a-port/ws?invite=[redacted]&code=b"));
    require_no_line_with(logs, "x-v4lue");
}

TEST_CASE("An unusable proxy logs the URL with the set of the client", "[logging][proxy][integration]") {
    log_capture logs;
    http::client client;
    client.redact_parameters({"invite"}).proxy("socks5://127.0.0.1:1080");

    // A loopback host would not use the proxy
    REQUIRE(client.get("http://origin.test:1/x?invite=x-v4lue&code=b").error_code() == http::client_error::invalid_proxy);

    INFO("captured logs:\n" << logs.dump());
    REQUIRE(logs.contains("cannot use the proxy for http://origin.test:1/x?invite=[redacted]&code=b"));
    require_no_line_with(logs, "x-v4lue");
}

TEST_CASE("Following a redirect logs its Location redacted", "[logging][redirect][integration]") {
    http::server server;
    answer_ok(server);
    server.get("/moved", [](http::request&, http::response& res) {
        res.status(http::http_response::status::moved_temporarily);
        res.header("Location", "/x?code=abc&invite=x-v4lue&state=s");
        res.send("moved");
    });
    log_capture logs;
    running_server running(server);

    http::client client;
    client.follow_redirects(true);

    SECTION("with the defaults") {
        REQUIRE(client.get(running.url("/moved")).status() == 200);
        INFO("captured logs:\n" << logs.dump());
        REQUIRE(logs.contains("Following redirect #1 to: /x?code=[redacted]&invite=x-v4lue&state=s"));
        require_no_line_with(logs, "abc");
    }
    SECTION("with the set of the client") {
        client.redact_parameters({"invite"});
        REQUIRE(client.get(running.url("/moved")).status() == 200);
        INFO("captured logs:\n" << logs.dump());
        REQUIRE(logs.contains("Following redirect #1 to: /x?code=abc&invite=[redacted]&state=s"));
    }
    SECTION("with the debug level off, not logged") {
        logging::get_logger()->set_level(spdlog::level::info);
        REQUIRE(client.get(running.url("/moved")).status() == 200);
        require_no_line_with(logs, "Following redirect");
    }
}

TEST_CASE("The values logged of the headers holding a URI are redacted", "[logging][integration]") {
    const std::string location = "https://app.example/cb?code=abc&token=t0k3n&state=s";
    const std::string shown = "https://app.example/cb?code=[redacted]&token=[redacted]&state=s";
    http::server server;
    answer_location(server, location);

    auto require_values = [&](const log_capture& logs) {
        INFO("captured logs:\n" << logs.dump());
        REQUIRE(logs.contains("  Location: " + shown));
        REQUIRE(logs.contains("  Content-Location: " + shown));
        REQUIRE(logs.contains("  Referer: " + shown));
        REQUIRE_FALSE(logs.contains("abc"));
        REQUIRE_FALSE(logs.contains("t0k3n"));
    };

    SECTION("in the server") {
        http::connection_options options;
        options.logging.enabled = true;
        server.set_connection_options(options);
        log_capture logs;
        running_server running(server);

        http::client client;
        REQUIRE(get_moved(client, running, location).status() == 302);
        require_values(logs);
    }
    SECTION("in the client") {
        log_capture logs;
        running_server running(server);

        http::client client;
        client.log_bodies(true);
        REQUIRE(get_moved(client, running, location).status() == 302);
        require_values(logs);
    }
    SECTION("only with the log of the values on") {
        log_capture logs;
        running_server running(server);

        http::client client;
        REQUIRE(get_moved(client, running, location).status() == 302);
        INFO("captured logs:\n" << logs.dump());
        REQUIRE(logs.contains("  Location"));
        REQUIRE_FALSE(logs.contains("app.example"));
    }
}

TEST_CASE("The headers holding a URI are redacted with the set of the server or the client", "[logging][connection_options][integration]") {
    const std::string location = "/next?invite=x-v4lue&code=b";
    const std::string shown = "/next?invite=[redacted]&code=b";
    http::server server;
    answer_location(server, location);

    auto require_values = [&](const log_capture& logs) {
        INFO("captured logs:\n" << logs.dump());
        REQUIRE(logs.contains("  Location: " + shown));
        REQUIRE(logs.contains("  Content-Location: " + shown));
        REQUIRE(logs.contains("  Referer: " + shown));
        REQUIRE_FALSE(logs.contains("  Location: " + location));
    };

    SECTION("the server") {
        http::connection_options options;
        options.logging.enabled = true;
        options.logging.redact = {"invite"};
        server.set_connection_options(options);
        log_capture logs;
        running_server running(server);

        http::client client;
        REQUIRE(get_moved(client, running, location).status() == 302);
        require_values(logs);
    }
    SECTION("the client") {
        log_capture logs;
        running_server running(server);

        http::client client;
        client.log_bodies(true).redact_parameters({"invite"});
        REQUIRE(get_moved(client, running, location).status() == 302);
        require_values(logs);
    }
}

TEST_CASE("The headers holding a URI are redacted before they are truncated", "[logging][integration]") {
    // Cut inside the value of the parameter: only its marker may be shown, in part
    constexpr size_t max_length = 12;
    http::server server;
    answer_location(server, "/cb?code=abcdefghijklmnop");
    http::connection_options options;
    options.logging = {true, max_length};
    server.set_connection_options(options);
    log_capture logs;
    running_server running(server);

    http::client client;
    REQUIRE(get_moved(client, running, "/from").status() == 302);

    INFO("captured logs:\n" << logs.dump());
    REQUIRE(logs.contains("  Location: /cb?code=[re... ("));
    REQUIRE_FALSE(logs.contains("abc"));
}

TEST_CASE("The password of a URI in a header is always redacted", "[logging][integration]") {
    http::server server;
    answer_location(server, "http://user:pa55word@app.example/x?invite=a");
    http::connection_options options;
    options.logging.enabled = true;
    options.logging.redact = {};
    server.set_connection_options(options);
    log_capture logs;
    running_server running(server);

    http::client client;
    REQUIRE(get_moved(client, running, "/from").status() == 302);

    INFO("captured logs:\n" << logs.dump());
    REQUIRE(logs.contains("  Location: http://user:[redacted]@app.example/x?invite=a"));
    REQUIRE_FALSE(logs.contains("pa55word"));
}

TEST_CASE("A reverse proxy logs its failures without the query of the request", "[logging][proxy][integration]") {
    http::reverse_proxy reverse_proxy;
    std::string target;
    {
        // A port nothing listens on
        boost::asio::io_context io;
        boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::make_address("127.0.0.1"), 0});
        target = "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port());
    }
    http::server server;
    server.set_not_found_handler([&](http::request& req, http::response& res) -> awaitable<void> {
        co_await reverse_proxy.forward(req, res, *http::proxy_target::from_url(target));
    });
    log_capture logs;
    running_server running(server);

    http::client client;
    REQUIRE(client.get(running.url("/app/page?invite=x-v4lue&code=abc")).status() == 502);

    INFO("captured logs:\n" << logs.dump());
    REQUIRE(logs.contains("reverse proxy GET /app/page -> " + target));
    // The request line is that of the server, with its set
    REQUIRE(logs.contains("[SERVER REQUEST] GET /app/page?invite=x-v4lue&code=[redacted]"));
    REQUIRE_FALSE(logs.contains("abc"));
}

TEST_CASE("A reverse proxy logs a path invalid for its target without the query", "[logging][proxy][integration]") {
    http::reverse_proxy reverse_proxy;
    http::server server;
    server.set_not_found_handler([&](http::request& req, http::response& res) -> awaitable<void> {
        http::proxy_options options;
        options.on_request = [](http::request&, http::http_request& outgoing) {
            outgoing.set_uri("no-leading-slash?code=abc");
        };
        co_await reverse_proxy.forward(req, res, *http::proxy_target::from_url("http://127.0.0.1:1"), options);
    });
    log_capture logs;
    running_server running(server);

    http::client client;
    REQUIRE(client.get(running.url("/app")).status() == 500);

    INFO("captured logs:\n" << logs.dump());
    REQUIRE(logs.contains("invalid path for the target: no-leading-slash"));
    REQUIRE_FALSE(logs.contains("abc"));
}

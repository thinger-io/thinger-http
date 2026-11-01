#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/websocket_connection.hpp>
#include <thinger/asio/ssl/certificate_manager.hpp>

#include <filesystem>

#include "server_test_helpers.hpp"
#include "../fixtures/stream_test_helpers.hpp"
#include "../fixtures/test_server_fixture.hpp"

// The support of the transport matrix (tests/fixtures/transport_matrix.hpp): servers listening
// on TCP, TLS and a Unix socket, the client helper, raw connections and the raw WebSocket client
// reaching them, and what the listeners leave behind (nothing)

using namespace thinger;
using namespace std::chrono_literals;
using http::test::transport;
using server_test::running_server;

namespace {

    void routes(http::server& server) {
        server.get("/hello", [](http::request& req, http::response& res) {
            res.send("hello " + req.get_http_request()->get_header("Host"));
        });
        server.get("/echo", [](http::request&, http::response& res) {
            res.upgrade_websocket({}, [](std::shared_ptr<http::websocket_connection> ws) {
                ws->on_message([ws](std::string message, bool) { ws->send_text(std::move(message)); });
            });
        });
    }

}

TEST_CASE("The client helper reaches a server on each transport", "[transport][integration]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    routes(server);
    running_server running(server, kind);
    REQUIRE(running.kind() == kind);

    http::client client;
    running.configure(client);
    auto response = client.get(running.url("/hello"));
    REQUIRE(response.status() == 200);
    REQUIRE(response.body() == "hello " + running.endpoint().host_header());

    SECTION("The URL names the transport") {
        auto url = running.url("/hello");
        switch (kind) {
            case transport::tcp: REQUIRE(url == "http://127.0.0.1:" + std::to_string(running.port) + "/hello"); break;
            case transport::tls: REQUIRE(url == "https://localhost:" + std::to_string(running.port) + "/hello"); break;
            case transport::unix:
                REQUIRE(url == "http://localhost/hello");
                REQUIRE(std::filesystem::exists(running.endpoint().unix_path));
                REQUIRE(running.endpoint().unix_path.size() < 100);
                break;
        }
    }
}

TEST_CASE("The TLS listener is verified against the matrix CA", "[transport][tls][integration]") {
    http::server server;
    routes(server);
    running_server running(server, transport::tls);

    // a client that does not trust the matrix CA refuses the server: verification is on
    http::client untrusting;
    REQUIRE_FALSE(untrusting.get(running.url("/hello")).ok());

    http::client client;
    running.configure(client);
    REQUIRE(client.get(running.url("/hello")).ok());
}

TEST_CASE("TestServerFixture listens on each transport", "[transport][integration]") {
    MATRIX_TRANSPORT(kind);
    http::test::TestServerFixture fixture(kind);
    http::client client;
    fixture.configure(client);
    auto response = client.get(fixture.base_url + "/get");
    REQUIRE(response.status() == 200);
    REQUIRE(response.body().find("\"ok\"") != std::string::npos);
}

TEST_CASE("Raw exchanges run on each transport", "[transport][integration]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    routes(server);
    running_server running(server, kind);
    auto host = running.endpoint().host_header();

    SECTION("raw_exchange reads up to the close") {
        auto result = server_test::raw_exchange(running,
            "GET /hello HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n");
        REQUIRE(result.closed);
        REQUIRE(result.data.starts_with("HTTP/1.1 200"));
        REQUIRE(result.data.ends_with("hello " + host));
    }

    SECTION("A read that times out leaves the connection usable") {
        http::test::raw_connection connection(running.endpoint());
        std::string data;
        REQUIRE(connection.read_some(data, 100ms) == 0);
        REQUIRE_FALSE(connection.eof());
        REQUIRE(connection.write("GET /hello HTTP/1.1\r\nHost: " + host + "\r\n\r\n"));
        while (data.find("hello " + host) == std::string::npos && !connection.eof()) {
            connection.read_some(data, 5s);
        }
        REQUIRE(data.starts_with("HTTP/1.1 200"));
        REQUIRE_FALSE(connection.eof());

        // the server sees the end of the stream after the half close, and closes too
        connection.shutdown_send();
        auto rest = connection.read_until_closed(5s);
        REQUIRE(rest.closed);
    }

    SECTION("The raw WebSocket client") {
        stream_test::raw_ws_client ws(running.endpoint());
        REQUIRE(ws.handshake("/echo").starts_with("HTTP/1.1 101"));
        ws.send_frame(stream_test::OP_TEXT, "ping over the matrix");
        auto frame = ws.read_frame();
        REQUIRE(frame);
        REQUIRE(frame->opcode == stream_test::OP_TEXT);
        REQUIRE(frame->payload == "ping over the matrix");
        ws.send_frame(stream_test::OP_CLOSE, stream_test::close_payload(1000));
        auto close = ws.read_frame();
        REQUIRE(close);
        REQUIRE(close->opcode == stream_test::OP_CLOSE);
        REQUIRE(ws.closed_within(5s));
    }
}

TEST_CASE("Listeners leave nothing behind", "[transport][integration]") {
    std::string path;
    {
        http::server server;
        routes(server);
        running_server running(server, transport::unix);
        path = running.endpoint().unix_path;
        REQUIRE(std::filesystem::exists(path));
    }
    REQUIRE_FALSE(std::filesystem::exists(std::filesystem::path(path).parent_path()));

    std::weak_ptr<asio::certificate_manager> outer_certificates;
    {
        http::server server;
        routes(server);
        running_server running(server, transport::tls);
        outer_certificates = running.listener.certificates();
        REQUIRE(running.listener.certificates()->get_default_certificate());
        {
            // a second listener (a proxy and its target) has certificates of its own
            http::server inner;
            routes(inner);
            running_server nested(inner, transport::tls);
            REQUIRE(nested.listener.certificates() != running.listener.certificates());
            http::client client;
            nested.configure(client);
            REQUIRE(client.get(nested.url("/hello")).ok());
        }
        http::client client;
        running.configure(client);
        REQUIRE(client.get(running.url("/hello")).ok());
    }
    REQUIRE(outer_certificates.expired());
}

#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/websocket_client.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/client/async_client.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thread>
#include <chrono>
#include <future>

using namespace thinger;
using namespace std::chrono_literals;

namespace {

// WebSocket test server fixture
struct WebSocketServerFixture {
    http::server server;
    uint16_t port = 0;
    std::string ws_url;
    std::thread server_thread;

    WebSocketServerFixture() {
        setup_websocket_endpoints();
        start_server();
    }

    ~WebSocketServerFixture() {
        server.stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

private:
    void setup_websocket_endpoints() {
        // Echo WebSocket endpoint - echoes back any message received
        // Note: ws->start() is called automatically by the server after the handler
        server.get("/ws/echo", [](http::request& req, http::response& res) {
            res.upgrade_websocket([](std::shared_ptr<http::websocket_connection> ws) {
                ws->on_message([ws](std::string message, bool binary) {
                    // Echo back the message with same type
                    if (binary) {
                        ws->send_binary(std::move(message));
                    } else {
                        ws->send_text(std::move(message));
                    }
                });
            });
        });

        // WebSocket endpoint that sends a welcome message on connect
        server.get("/ws/welcome", [](http::request& req, http::response& res) {
            res.upgrade_websocket([](std::shared_ptr<http::websocket_connection> ws) {
                ws->on_message([ws](std::string message, bool binary) {
                    ws->send_text("received: " + message);
                });
                // Send welcome message - this will be queued and sent after start()
                ws->send_text("welcome");
            });
        });
    }

    void start_server() {
        REQUIRE(server.listen("127.0.0.1", 0));
        port = server.local_port();
        ws_url = "ws://127.0.0.1:" + std::to_string(port);

        std::promise<void> ready;
        server_thread = std::thread([this, &ready]() {
            ready.set_value();
            server.wait();
        });
        ready.get_future().wait();
    }
};

} // anonymous namespace

TEST_CASE("HTTP Client WebSocket Integration", "[websocket][client][integration]") {
    WebSocketServerFixture fixture;

    SECTION("Connect via client.websocket() - sync") {
        http::client client;

        auto ws = client.websocket(fixture.ws_url + "/ws/echo");

        REQUIRE(ws.has_value());
        REQUIRE(ws->is_open());

        ws->close();
    }

    SECTION("Send and receive via client.websocket()") {
        http::client client;

        auto ws = client.websocket(fixture.ws_url + "/ws/echo");
        REQUIRE(ws.has_value());

        // Send a text message
        REQUIRE(ws->send_text("Hello from client.websocket()!"));

        // Receive echo
        auto [message, is_binary] = ws->receive();

        REQUIRE(message == "Hello from client.websocket()!");
        REQUIRE_FALSE(is_binary);

        ws->close();
    }

    SECTION("Send binary via client.websocket()") {
        http::client client;

        auto ws = client.websocket(fixture.ws_url + "/ws/echo");
        REQUIRE(ws.has_value());

        // Send binary data
        std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
        REQUIRE(ws->send_binary(data.data(), data.size()));

        // Receive echo
        auto [message, is_binary] = ws->receive();

        REQUIRE(message.size() == data.size());
        REQUIRE(is_binary);

        ws->close();
    }

    SECTION("Invalid URL returns nullopt") {
        http::client client;

        auto ws = client.websocket("ws://invalid.host.test:9999/ws");

        REQUIRE_FALSE(ws.has_value());
    }

    SECTION("Multiple messages via client.websocket()") {
        http::client client;

        auto ws = client.websocket(fixture.ws_url + "/ws/echo");
        REQUIRE(ws.has_value());

        for (int i = 0; i < 3; ++i) {
            std::string msg = "Msg" + std::to_string(i);
            REQUIRE(ws->send_text(msg));
            auto [response, _] = ws->receive();
            REQUIRE(response == msg);
        }

        ws->close();
    }

    SECTION("Server closes connection on buffer overflow") {
        http::client client;

        auto ws = client.websocket(fixture.ws_url + "/ws/echo");
        REQUIRE(ws.has_value());
        REQUIRE(ws->is_open());

        // Create message larger than websocket_connection::MAX_BUFFER_SIZE (16MB)
        std::string large_message(17 * 1024 * 1024, 'X');

        // Send oversized message - may fail mid-write when server detects overflow
        ws->send_text(large_message);

        // Server should have closed the connection after detecting buffer overflow
        // receive() returns empty when the server-side connection is gone
        auto [message, is_binary] = ws->receive();
        REQUIRE(message.empty());

        ws->close();
    }

    SECTION("Same client can do HTTP and WebSocket") {
        http::client client;

        // First do a regular HTTP request (would need an HTTP endpoint)
        // For now, just test WebSocket works
        auto ws = client.websocket(fixture.ws_url + "/ws/welcome");
        REQUIRE(ws.has_value());

        // Receive welcome message
        auto [message, _] = ws->receive();
        REQUIRE(message == "welcome");

        ws->close();
    }
}

TEST_CASE("Async Client WebSocket Integration", "[websocket][client][integration]") {
    WebSocketServerFixture fixture;

    SECTION("Connect via async_client.websocket() - async") {
        http::async_client client;

        std::atomic<bool> connected{false};
        std::atomic<bool> callback_called{false};

        client.websocket(fixture.ws_url + "/ws/echo", [&](std::shared_ptr<http::websocket_client> ws) {
            callback_called = true;
            if (ws && ws->is_open()) {
                connected = true;
                ws->close();
            }
        });

        client.wait();

        REQUIRE(callback_called);
        REQUIRE(connected);
    }

    SECTION("Send/receive via async_client.websocket()") {
        http::async_client client;

        std::string received_message;
        std::atomic<bool> done{false};

        client.websocket(fixture.ws_url + "/ws/echo", [&](std::shared_ptr<http::websocket_client> ws) {
            if (ws && ws->is_open()) {
                ws->send_text("Hello async!");
                auto [msg, _] = ws->receive();
                received_message = msg;
                ws->close();
            }
            done = true;
        });

        client.wait();

        REQUIRE(done);
        REQUIRE(received_message == "Hello async!");
    }

    SECTION("Invalid URL calls callback with nullptr") {
        http::async_client client;

        std::atomic<bool> callback_called{false};
        std::shared_ptr<http::websocket_client> received_ws;

        client.websocket("ws://invalid.host.test:9999/ws", [&](std::shared_ptr<http::websocket_client> ws) {
            callback_called = true;
            received_ws = ws;
        });

        client.wait();

        REQUIRE(callback_called);
        REQUIRE_FALSE(received_ws);  // Should be nullptr
    }
}

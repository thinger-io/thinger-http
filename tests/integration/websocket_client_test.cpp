#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/websocket_client.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/client/async_client.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thread>
#include <chrono>

using namespace thinger;
using namespace std::chrono_literals;

namespace {

// WebSocket test server fixture
struct WebSocketServerFixture {
    http::server server;
    uint16_t port = 9100;
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
        bool started = false;
        int attempts = 0;
        const int max_attempts = 10;

        while (!started && attempts < max_attempts) {
            if (server.listen("127.0.0.1", port)) {
                started = true;
                ws_url = "ws://127.0.0.1:" + std::to_string(port);
            } else {
                port++;
                attempts++;
            }
        }

        if (!started) {
            throw std::runtime_error("Could not start WebSocket test server");
        }

        server_thread = std::thread([this]() {
            server.wait();
        });

        // Give server time to start
        std::this_thread::sleep_for(100ms);
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

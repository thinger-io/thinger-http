#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/client/async_client.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/util/logger.hpp>
#include "../fixtures/test_server_fixture.hpp"
#include <chrono>
#include <atomic>
#include <future>
#include <thread>

using namespace thinger;
using namespace std::chrono_literals;

namespace {

// WebSocket test server fixture for request builder tests
struct WebSocketBuilderFixture {
    http::server server;
    uint16_t port = 0;
    std::string ws_url;
    std::thread server_thread;
    std::string last_protocol;  // Store the negotiated protocol

    WebSocketBuilderFixture() {
        setup_websocket_endpoints();
        start_server();
    }

    ~WebSocketBuilderFixture() {
        server.stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

private:
    void setup_websocket_endpoints() {
        // Basic echo WebSocket endpoint (no protocol required)
        server.get("/ws/echo", [](http::request& req, http::response& res) {
            res.upgrade_websocket([](std::shared_ptr<http::websocket_connection> ws) {
                ws->on_message([ws](std::string message, bool binary) {
                    ws->send_text("echo: " + message);
                });
            });
        });

        // Echo WebSocket endpoint that requires a protocol
        server.get("/ws/echo-with-protocol", [](http::request& req, http::response& res) {
            std::set<std::string> protocols = {"echo", "chat", "json"};
            res.upgrade_websocket([](std::shared_ptr<http::websocket_connection> ws) {
                ws->on_message([ws](std::string message, bool binary) {
                    ws->send_text("echo: " + message);
                });
            }, protocols);
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

// ============================================
// Sync Client Request Builder Tests
// ============================================

TEST_CASE("Request Builder with sync client", "[http][client][request_builder][sync]") {

    thinger::http::test::TestServerFixture fixture;
    auto& base_url = fixture.base_url;

    SECTION("GET with headers using builder") {
        http::client client;

        auto response = client.request(base_url + "/headers")
            .header("X-Custom-Header", "test-value")
            .header("Authorization", "Bearer token123")
            .get();

        REQUIRE(response.ok());
        REQUIRE(response.status() == 200);
    }

    SECTION("POST with body using builder") {
        http::client client;

        auto response = client.request(base_url + "/post")
            .header("X-Request-ID", "12345")
            .body(R"({"name": "test", "value": 42})", "application/json")
            .post();

        REQUIRE(response.ok());
        REQUIRE(response.status() == 200);
    }

    SECTION("PUT with body using builder") {
        http::client client;

        auto response = client.request(base_url + "/put")
            .body(R"({"updated": true})")
            .put();

        REQUIRE(response.ok());
        REQUIRE(response.status() == 200);
    }

    SECTION("PATCH with body using builder") {
        http::client client;

        auto response = client.request(base_url + "/patch")
            .body(R"({"partial": "update"})")
            .patch();

        REQUIRE(response.ok());
        REQUIRE(response.status() == 200);
    }

    SECTION("DELETE using builder") {
        http::client client;

        auto response = client.request(base_url + "/delete")
            .header("Authorization", "Bearer admin-token")
            .del();

        REQUIRE(response.ok());
        REQUIRE(response.status() == 200);
    }

    SECTION("HEAD using builder") {
        http::client client;

        auto response = client.request(base_url + "/get")
            .head();

        // HEAD may return 200 or 405 depending on server implementation
        // The important thing is that we get a valid response
        REQUIRE(response.status() > 0);
        // If server supports HEAD, body should be empty
        if (response.ok()) {
            REQUIRE(response.body().empty());
        }
    }

    SECTION("OPTIONS using builder") {
        http::client client;

        auto response = client.request(base_url + "/get")
            .options();

        // Server should respond to OPTIONS
        REQUIRE(response.status() >= 200);
    }

    SECTION("Builder with multiple headers") {
        http::client client;

        std::map<std::string, std::string> hdrs = {
            {"X-Header-1", "value1"},
            {"X-Header-2", "value2"},
            {"X-Header-3", "value3"}
        };

        auto response = client.request(base_url + "/headers")
            .headers(hdrs)
            .header("X-Header-4", "value4")
            .get();

        REQUIRE(response.ok());
    }

    SECTION("Builder with form body") {
        http::client client;

        http::form f;
        f.field("username", "testuser");
        f.field("password", "secret123");

        auto response = client.request(base_url + "/post")
            .body(f)
            .post();

        REQUIRE(response.ok());
        REQUIRE(response.status() == 200);
    }

    SECTION("Streaming GET using builder") {
        http::client client;

        size_t bytes_received = 0;
        auto result = client.request(base_url + "/get")
            .get([&bytes_received](const http::stream_info& info) {
                bytes_received += info.data.size();
                return true; // continue streaming
            });

        REQUIRE(result.ok());
        REQUIRE(bytes_received > 0);
    }
}

// ============================================
// Async Client Request Builder Tests (Awaitable)
// ============================================

TEST_CASE("Request Builder with async client (awaitable)", "[http][client][request_builder][async][awaitable]") {

    thinger::http::test::TestServerFixture fixture;
    auto& base_url = fixture.base_url;

    SECTION("GET with headers using builder and co_await") {
        http::async_client client;
        std::atomic<bool> success{false};

        client.run([&]() -> awaitable<void> {
            auto response = co_await client.request(base_url + "/headers")
                .header("X-Custom-Header", "async-test")
                .get();

            success = response.ok() && response.status() == 200;
        });

        client.wait();
        REQUIRE(success);
    }

    SECTION("POST with body using builder and co_await") {
        http::async_client client;
        std::atomic<bool> success{false};

        client.run([&]() -> awaitable<void> {
            auto response = co_await client.request(base_url + "/post")
                .header("Content-Type", "application/json")
                .body(R"({"async": true})")
                .post();

            success = response.ok() && response.status() == 200;
        });

        client.wait();
        REQUIRE(success);
    }

    SECTION("Multiple requests with builder using co_await") {
        http::async_client client;
        std::atomic<int> completed{0};

        for (int i = 0; i < 5; ++i) {
            client.run([&, i]() -> awaitable<void> {
                auto response = co_await client.request(base_url + "/get")
                    .header("X-Request-Index", std::to_string(i))
                    .get();

                if (response.ok()) {
                    completed++;
                }
            });
        }

        client.wait();
        REQUIRE(completed == 5);
    }

    SECTION("PUT using builder and co_await") {
        http::async_client client;
        std::atomic<bool> success{false};

        client.run([&]() -> awaitable<void> {
            auto response = co_await client.request(base_url + "/put")
                .body(R"({"data": "updated"})")
                .put();

            success = response.ok();
        });

        client.wait();
        REQUIRE(success);
    }

    SECTION("DELETE using builder and co_await") {
        http::async_client client;
        std::atomic<bool> success{false};

        client.run([&]() -> awaitable<void> {
            auto response = co_await client.request(base_url + "/delete")
                .del();

            success = response.ok();
        });

        client.wait();
        REQUIRE(success);
    }
}

// ============================================
// Async Client Request Builder Tests (Callback)
// ============================================

TEST_CASE("Request Builder with async client (callback)", "[http][client][request_builder][async][callback]") {

    thinger::http::test::TestServerFixture fixture;
    auto& base_url = fixture.base_url;

    SECTION("GET with callback using builder") {
        http::async_client client;
        std::promise<bool> promise;
        auto future = promise.get_future();

        client.request(base_url + "/headers")
            .header("X-Callback-Test", "true")
            .get([&promise](http::client_response& response) {
                promise.set_value(response.ok() && response.status() == 200);
            });

        client.wait();
        REQUIRE(future.get() == true);
    }

    SECTION("POST with callback using builder") {
        http::async_client client;
        std::promise<bool> promise;
        auto future = promise.get_future();

        client.request(base_url + "/post")
            .body(R"({"callback": true})", "application/json")
            .post([&promise](http::client_response& response) {
                promise.set_value(response.ok() && response.status() == 200);
            });

        client.wait();
        REQUIRE(future.get() == true);
    }

    SECTION("PUT with callback using builder") {
        http::async_client client;
        std::promise<bool> promise;
        auto future = promise.get_future();

        client.request(base_url + "/put")
            .body(R"({"updated": true})")
            .put([&promise](http::client_response& response) {
                promise.set_value(response.ok());
            });

        client.wait();
        REQUIRE(future.get() == true);
    }

    SECTION("PATCH with callback using builder") {
        http::async_client client;
        std::promise<bool> promise;
        auto future = promise.get_future();

        client.request(base_url + "/patch")
            .body(R"({"partial": true})")
            .patch([&promise](http::client_response& response) {
                promise.set_value(response.ok());
            });

        client.wait();
        REQUIRE(future.get() == true);
    }

    SECTION("DELETE with callback using builder") {
        http::async_client client;
        std::promise<bool> promise;
        auto future = promise.get_future();

        client.request(base_url + "/delete")
            .header("Authorization", "Bearer token")
            .del([&promise](http::client_response& response) {
                promise.set_value(response.ok());
            });

        client.wait();
        REQUIRE(future.get() == true);
    }

    SECTION("HEAD with callback using builder") {
        http::async_client client;
        std::promise<bool> promise;
        auto future = promise.get_future();

        client.request(base_url + "/get")
            .head([&promise](http::client_response& response) {
                // HEAD may return 200 or 405 depending on server
                // The important thing is we get a response
                promise.set_value(response.status() > 0);
            });

        client.wait();
        REQUIRE(future.get() == true);
    }

    SECTION("OPTIONS with callback using builder") {
        http::async_client client;
        std::promise<bool> promise;
        auto future = promise.get_future();

        client.request(base_url + "/get")
            .options([&promise](http::client_response& response) {
                promise.set_value(response.status() >= 200);
            });

        client.wait();
        REQUIRE(future.get() == true);
    }

    SECTION("Multiple callback requests in parallel") {
        http::async_client client;
        std::atomic<int> completed{0};
        const int num_requests = 10;

        for (int i = 0; i < num_requests; ++i) {
            client.request(base_url + "/get")
                .header("X-Request-Index", std::to_string(i))
                .get([&completed](http::client_response& response) {
                    if (response.ok()) {
                        completed++;
                    }
                });
        }

        client.wait();
        REQUIRE(completed == num_requests);
    }

    SECTION("Mixed callback and awaitable requests") {
        http::async_client client;
        std::atomic<int> callback_done{0};
        std::atomic<int> awaitable_done{0};

        // Launch some callback requests
        for (int i = 0; i < 3; ++i) {
            client.request(base_url + "/get")
                .get([&callback_done](http::client_response& response) {
                    if (response.ok()) callback_done++;
                });
        }

        // Launch some awaitable requests
        for (int i = 0; i < 3; ++i) {
            client.run([&]() -> awaitable<void> {
                auto response = co_await client.request(base_url + "/get").get();
                if (response.ok()) awaitable_done++;
            });
        }

        client.wait();

        REQUIRE(callback_done == 3);
        REQUIRE(awaitable_done == 3);
    }
}

// ============================================
// Sync vs Async Builder API Consistency
// ============================================

TEST_CASE("Request Builder API consistency between sync and async", "[http][client][request_builder][consistency]") {

    thinger::http::test::TestServerFixture fixture;
    auto& base_url = fixture.base_url;

    SECTION("Same builder pattern works for both clients") {
        // Sync client
        http::client sync_client;
        auto sync_response = sync_client.request(base_url + "/get")
            .header("X-Client-Type", "sync")
            .get();

        REQUIRE(sync_response.ok());

        // Async client (with awaitable)
        http::async_client async_client;
        std::atomic<bool> async_success{false};

        async_client.run([&]() -> awaitable<void> {
            auto response = co_await async_client.request(base_url + "/get")
                .header("X-Client-Type", "async")
                .get();

            async_success = response.ok();
        });

        async_client.wait();
        REQUIRE(async_success);

        // Async client (with callback)
        http::async_client callback_client;
        std::promise<bool> promise;
        auto future = promise.get_future();

        callback_client.request(base_url + "/get")
            .header("X-Client-Type", "callback")
            .get([&promise](http::client_response& response) {
                promise.set_value(response.ok());
            });

        callback_client.wait();
        REQUIRE(future.get() == true);
    }
}

// ============================================
// WebSocket Request Builder Tests
// ============================================

TEST_CASE("Request Builder WebSocket with sync client", "[http][client][request_builder][websocket][sync]") {
    WebSocketBuilderFixture fixture;

    SECTION("WebSocket connect using builder") {
        http::client client;

        auto ws = client.request(fixture.ws_url + "/ws/echo")
            .websocket();

        REQUIRE(ws.has_value());

        ws->send_text("hello");
        auto [msg, binary] = ws->receive();
        REQUIRE(msg == "echo: hello");
        REQUIRE_FALSE(binary);

        ws->close();
    }

    SECTION("WebSocket connect with custom headers using builder") {
        http::client client;

        auto ws = client.request(fixture.ws_url + "/ws/echo")
            .header("X-Custom-Header", "test-value")
            .header("Authorization", "Bearer token123")
            .websocket();

        REQUIRE(ws.has_value());

        ws->send_text("test");
        auto [msg, binary] = ws->receive();
        REQUIRE(msg == "echo: test");

        ws->close();
    }

    SECTION("WebSocket connect with protocol using builder") {
        http::client client;

        auto ws = client.request(fixture.ws_url + "/ws/echo-with-protocol")
            .protocol("echo")
            .websocket();

        REQUIRE(ws.has_value());

        ws->send_text("protocol test");
        auto [msg, binary] = ws->receive();
        REQUIRE(msg == "echo: protocol test");

        ws->close();
    }

    SECTION("WebSocket connect with headers and protocol using builder") {
        http::client client;

        auto ws = client.request(fixture.ws_url + "/ws/echo-with-protocol")
            .header("Authorization", "Bearer token")
            .protocol("json")
            .websocket();

        REQUIRE(ws.has_value());

        ws->send_text("combined test");
        auto [msg, binary] = ws->receive();
        REQUIRE(msg == "echo: combined test");

        ws->close();
    }
}

TEST_CASE("Request Builder WebSocket with async client", "[http][client][request_builder][websocket][async]") {
    WebSocketBuilderFixture fixture;

    SECTION("WebSocket connect using builder with awaitable") {
        http::async_client client;
        std::atomic<bool> success{false};

        client.run([&]() -> awaitable<void> {
            auto ws = co_await client.request(fixture.ws_url + "/ws/echo")
                .websocket();

            if (ws) {
                co_await ws->send_text_async("async hello");
                auto [msg, binary] = co_await ws->receive_async();
                success = (msg == "echo: async hello");
                co_await ws->close_async();
            }
        });

        client.wait();
        REQUIRE(success);
    }

    SECTION("WebSocket connect with protocol using builder (awaitable)") {
        http::async_client client;
        std::atomic<bool> success{false};

        client.run([&]() -> awaitable<void> {
            auto ws = co_await client.request(fixture.ws_url + "/ws/echo-with-protocol")
                .header("Authorization", "Bearer token")
                .protocol("chat")
                .websocket();

            if (ws) {
                co_await ws->send_text_async("protocol async test");
                auto [msg, binary] = co_await ws->receive_async();
                success = (msg == "echo: protocol async test");
                co_await ws->close_async();
            }
        });

        client.wait();
        REQUIRE(success);
    }

    SECTION("WebSocket connect using builder with callback") {
        http::async_client client;
        std::promise<bool> promise;
        auto future = promise.get_future();

        client.request(fixture.ws_url + "/ws/echo")
            .header("X-Test", "callback")
            .websocket([&promise](std::shared_ptr<http::websocket_client> ws) {
                if (ws) {
                    ws->send_text("callback hello");
                    auto [msg, binary] = ws->receive();
                    promise.set_value(msg == "echo: callback hello");
                    ws->close();
                } else {
                    promise.set_value(false);
                }
            });

        client.wait();
        REQUIRE(future.get() == true);
    }

    SECTION("WebSocket connect with protocol using builder (callback)") {
        http::async_client client;
        std::promise<bool> promise;
        auto future = promise.get_future();

        client.request(fixture.ws_url + "/ws/echo-with-protocol")
            .protocol("json")
            .websocket([&promise](std::shared_ptr<http::websocket_client> ws) {
                if (ws) {
                    ws->send_text("protocol callback test");
                    auto [msg, binary] = ws->receive();
                    promise.set_value(msg == "echo: protocol callback test");
                    ws->close();
                } else {
                    promise.set_value(false);
                }
            });

        client.wait();
        REQUIRE(future.get() == true);
    }
}

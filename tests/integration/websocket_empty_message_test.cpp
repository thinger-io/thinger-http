#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/websocket_client.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/client/async_client.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/websocket_connection.hpp>
#include "server_test_helpers.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <vector>

// Empty WebSocket messages (a data frame without payload, valid per RFC 6455) are sent and
// received as messages both ways, and do not end the stream of the client

using namespace thinger;
using namespace std::chrono_literals;

namespace {

    struct received_message {
        std::string data;
        bool binary;
    };

    struct empty_message_server {
        http::server server;
        std::mutex mutex;
        std::vector<received_message> received;
        std::optional<server_test::running_server> running;

        empty_message_server() {
            // Echoes every message, and records it
            server.get("/echo", [this](http::request&, http::response& res) {
                res.upgrade_websocket([this](std::shared_ptr<http::websocket_connection> ws) {
                    ws->on_message([this, ws](std::string message, bool binary) {
                        {
                            std::lock_guard lock(mutex);
                            received.push_back({message, binary});
                        }
                        if (binary) ws->send_binary(std::move(message));
                        else ws->send_text(std::move(message));
                    });
                });
            });
            // Sends empty messages of both types, then a last one, and closes when told
            server.get("/empties", [](http::request&, http::response& res) {
                res.upgrade_websocket([](std::shared_ptr<http::websocket_connection> ws) {
                    ws->on_message([ws](std::string message, bool) {
                        if (message == "close") ws->stop();
                    });
                    ws->send_text("");
                    ws->send_binary("");
                    ws->send_text("last");
                });
            });
            running.emplace(server);
        }

        std::string url(const std::string& path) const {
            return "ws://127.0.0.1:" + std::to_string(running->port) + path;
        }
    };

}

TEST_CASE("WebSocket empty messages from the client reach the server", "[websocket][client][integration]") {
    empty_message_server server;
    http::client client;
    auto ws = client.websocket(server.url("/echo"));
    REQUIRE(ws.has_value());

    REQUIRE(ws->send_text(""));
    REQUIRE(ws->send_binary(std::string_view{}));
    REQUIRE(ws->send_text("after"));

    // echoed back in order: the empty ones as empty messages, the connection still open
    auto [first, first_binary] = ws->receive();
    CHECK(first.empty());
    CHECK_FALSE(first_binary);
    CHECK(ws->is_open());
    auto [second, second_binary] = ws->receive();
    CHECK(second.empty());
    CHECK(second_binary);
    CHECK(ws->is_open());
    auto [third, third_binary] = ws->receive();
    CHECK(third == "after");
    CHECK_FALSE(third_binary);

    {
        std::lock_guard lock(server.mutex);
        REQUIRE(server.received.size() == 3);
        CHECK(server.received[0].data.empty());
        CHECK_FALSE(server.received[0].binary);
        CHECK(server.received[1].data.empty());
        CHECK(server.received[1].binary);
        CHECK(server.received[2].data == "after");
    }
    ws->close();
}

TEST_CASE("WebSocket empty messages from the server reach the client", "[websocket][client][integration]") {
    empty_message_server server;

    SECTION("synchronous receive") {
        http::client client;
        auto ws = client.websocket(server.url("/empties"));
        REQUIRE(ws.has_value());

        auto [first, first_binary] = ws->receive();
        CHECK(first.empty());
        CHECK_FALSE(first_binary);
        CHECK(ws->is_open());
        auto [second, second_binary] = ws->receive();
        CHECK(second.empty());
        CHECK(second_binary);
        CHECK(ws->is_open());
        auto [third, third_binary] = ws->receive();
        CHECK(third == "last");
        ws->close();
    }

    SECTION("event-driven: on_message gets them, on_close is not fired by them") {
        http::async_client client;
        std::mutex mutex;
        std::vector<received_message> messages;
        std::atomic<bool> closed{false};
        std::atomic<bool> closed_before_last{false};
        std::promise<void> last_received;
        std::promise<void> close_called;
        std::shared_ptr<http::websocket_client> keep;

        client.run([&]() -> awaitable<void> {
            auto result = co_await client.websocket(server.url("/empties"));
            if (!result) {
                last_received.set_value();
                co_return;
            }
            auto executor = co_await boost::asio::this_coro::executor;
            auto ws = std::make_shared<http::websocket_client>(std::move(*result));
            keep = ws;
            ws->on_message([&, executor, raw = ws.get()](const std::string& message, bool binary) {
                std::lock_guard lock(mutex);
                messages.push_back({message, binary});
                if (message == "last") {
                    closed_before_last = closed.load();
                    last_received.set_value();
                    // ask the server to close: on_close fires then, not before
                    co_spawn(executor, [raw]() -> awaitable<void> { co_await raw->send_text_async("close"); },
                             detached);
                }
            });
            ws->on_close([&] {
                closed = true;
                close_called.set_value();
            });
            ws->run();
        });

        REQUIRE(last_received.get_future().wait_for(5s) == std::future_status::ready);
        CHECK_FALSE(closed_before_last);
        {
            std::lock_guard lock(mutex);
            REQUIRE(messages.size() == 3);
            CHECK(messages[0].data.empty());
            CHECK_FALSE(messages[0].binary);
            CHECK(messages[1].data.empty());
            CHECK(messages[1].binary);
            CHECK(messages[2].data == "last");
        }
        CHECK(close_called.get_future().wait_for(5s) == std::future_status::ready);
        {
            std::lock_guard lock(mutex);
            CHECK(messages.size() == 3);
        }
        client.stop();
        client.wait();
    }
}

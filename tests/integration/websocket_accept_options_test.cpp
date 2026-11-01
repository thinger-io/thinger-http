#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <thinger/asio/sockets/websocket.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/websocket_connection.hpp>

#include "server_test_helpers.hpp"
#include "../fixtures/stream_test_helpers.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// websocket_accept_options: the defaults (16 MiB messages, and a keepalive of 60 s and 120 s
// with the message interface, none as a byte stream), each option with small values, and the
// subprotocols negotiated by upgrade_websocket()

using namespace thinger;
using namespace std::chrono_literals;
using namespace stream_test;
using server_test::running_server;

namespace {

    using clock = std::chrono::steady_clock;

    // A WebSocket of the message interface on a socket of its own (no server, no peer): the
    // keepalive start() sets on the WebSocket
    struct started_connection {
        boost::asio::io_context io_context;
        std::shared_ptr<memory_socket> socket = std::make_shared<memory_socket>(io_context, [](std::string) {});
        std::shared_ptr<asio::websocket> ws = std::make_shared<asio::websocket>(socket, true, true);

        explicit started_connection(const http::websocket_accept_options& options = {}) {
            auto connection = std::make_shared<http::websocket_connection>(ws, options);
            connection->on_message([](std::string, bool) {});
            connection->start();
        }

        ~started_connection() {
            socket->push_eof();
            io_context.run_for(100ms);
        }
    };

    // What the server received, and the subprotocol and keepalive of the byte streams
    struct received_state {
        std::mutex mutex;
        std::vector<size_t> sizes;
        std::optional<std::chrono::milliseconds> stream_ping;
        std::optional<std::chrono::milliseconds> stream_idle;

        size_t count() {
            std::scoped_lock lock(mutex);
            return sizes.size();
        }
    };

    // A route of the message interface with `options`, answering each message with its size
    void sizes_route(http::server& server, const std::string& path, http::websocket_accept_options options,
                     received_state& state) {
        server.get(path, [&state, options](http::response& res) {
            res.upgrade_websocket(options, [&state](std::shared_ptr<http::websocket_connection> ws) {
                ws->on_message([&state, weak = std::weak_ptr(ws)](std::string message, bool) {
                    {
                        std::scoped_lock lock(state.mutex);
                        state.sizes.push_back(message.size());
                    }
                    if (auto ws = weak.lock()) ws->send_text(std::to_string(message.size()));
                });
            });
        });
    }

    // A route upgrading to a byte stream with `options`, keeping it while read
    void stream_route(http::server& server, const std::string& path, http::websocket_accept_options options,
                      received_state& state) {
        server.get(path, [&state, options](http::response& res) {
            res.upgrade_websocket_stream(options, [&state](http::upgraded_websocket upgraded) {
                {
                    std::scoped_lock lock(state.mutex);
                    state.stream_ping = upgraded.stream->get_ping_interval();
                    state.stream_idle = upgraded.stream->get_idle_timeout();
                }
                co_spawn(upgraded.stream->get_executor(), [stream = upgraded.stream]() -> thinger::awaitable<void> {
                    uint8_t buffer[64];
                    while (true) {
                        auto [ec, bytes] = co_await stream->read_some(buffer, sizeof(buffer));
                        if (ec) break;
                    }
                }, boost::asio::detached);
            });
        });
    }

    // The next frame that is not a ping (answered); nullopt if none arrives
    std::optional<ws_frame> next_data_frame(raw_ws_client& client, std::chrono::milliseconds timeout = 5s) {
        while (auto frame = client.read_frame(timeout)) {
            if (frame->opcode != OP_PING) return frame;
            client.send_frame(OP_PONG, frame->payload);
        }
        return std::nullopt;
    }

    // Whether the server pings within the timeout
    bool pinged_within(raw_ws_client& client, std::chrono::milliseconds timeout) {
        auto frame = client.read_frame(timeout);
        return frame && frame->opcode == OP_PING;
    }

    // Whether the server closes within the timeout: its close frame (1001 Going Away, as when
    // idle) or the connection closed
    bool closed_idle_within(raw_ws_client& client, std::chrono::milliseconds timeout) {
        auto frame = client.read_frame(timeout);
        if (frame) return frame->opcode == OP_CLOSE && frame->close_code() == 1001;
        return client.closed_within(10ms);
    }

}

TEST_CASE("WebSocket accept options default to the behaviour of before", "[websocket][options][integration]") {
    const http::websocket_accept_options defaults;
    CHECK(defaults.max_message_size == size_t{16} * 1024 * 1024);
    CHECK(defaults.max_message_size == http::websocket_connection::MAX_BUFFER_SIZE);
    CHECK(defaults.protocol_required);
    CHECK(defaults.binary);
    CHECK_FALSE(defaults.ping_interval.has_value());
    CHECK_FALSE(defaults.idle_timeout.has_value());
    CHECK(defaults.protocols.empty());

    SECTION("The message interface pings after 60 s without receiving anything, and closes after 120 s") {
        started_connection started;
        CHECK(started.ws->get_ping_interval() == 60s);
        CHECK(started.ws->get_idle_timeout() == 120s);
    }

    SECTION("Unless the options set them, 0 turning each off") {
        started_connection set({.ping_interval = 5s, .idle_timeout = 7s});
        CHECK(set.ws->get_ping_interval() == 5s);
        CHECK(set.ws->get_idle_timeout() == 7s);
        started_connection off({.ping_interval = 0ms, .idle_timeout = 0ms});
        CHECK(off.ws->get_ping_interval() == 0ms);
        CHECK(off.ws->get_idle_timeout() == 0ms);
    }
}

TEST_CASE("A WebSocket message of up to 16 MiB is delivered by default, one byte more closes with 1009",
          "[websocket][options][integration]") {
    constexpr size_t limit = size_t{16} * 1024 * 1024;
    received_state state;
    http::server server;
    sizes_route(server, "/default", {}, state);
    running_server running(server);
    raw_ws_client client(running.port);
    REQUIRE(client.handshake("/default").starts_with("HTTP/1.1 101"));

    client.send_frame(OP_BINARY, std::string(limit, 'x'));
    auto answer = next_data_frame(client, 20s);
    REQUIRE(answer);
    REQUIRE(answer->opcode == OP_TEXT);
    CHECK(answer->payload == std::to_string(limit));

    client.send_frame(OP_BINARY, std::string(limit + 1, 'x'));
    auto close = next_data_frame(client, 20s);
    REQUIRE(close);
    REQUIRE(close->opcode == OP_CLOSE);
    CHECK(close->close_code() == 1009);
    CHECK(state.count() == 1);
}

TEST_CASE("WebSocket accept options set the largest message and the keepalive of the message interface",
          "[websocket][options][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    received_state state;
    http::server server;
    sizes_route(server, "/small", {.max_message_size = 10}, state);
    sizes_route(server, "/ping", {.ping_interval = 100ms}, state);
    sizes_route(server, "/idle", {.ping_interval = 0ms, .idle_timeout = 300ms}, state);
    sizes_route(server, "/off", {.ping_interval = 0ms, .idle_timeout = 0ms}, state);
    running_server running(server, kind);
    raw_ws_client client(running.endpoint());

    SECTION("max_message_size") {
        REQUIRE(client.handshake("/small").starts_with("HTTP/1.1 101"));
        client.send_frame(OP_TEXT, "0123456789");
        auto answer = next_data_frame(client);
        REQUIRE(answer);
        CHECK(answer->payload == "10");
        // fragmented over the limit too
        client.send_frame(OP_TEXT, "01234", false);
        client.send_frame(OP_CONTINUATION, "567890");
        auto close = next_data_frame(client);
        REQUIRE(close);
        REQUIRE(close->opcode == OP_CLOSE);
        CHECK(close->close_code() == 1009);
        CHECK(state.count() == 1);
    }

    SECTION("ping_interval") {
        REQUIRE(client.handshake("/ping").starts_with("HTTP/1.1 101"));
        auto begin = clock::now();
        REQUIRE(pinged_within(client, 2s));
        CHECK(clock::now() - begin >= 80ms);
        CHECK(clock::now() - begin < 1s);
    }

    SECTION("idle_timeout") {
        REQUIRE(client.handshake("/idle").starts_with("HTTP/1.1 101"));
        auto begin = clock::now();
        REQUIRE(closed_idle_within(client, 3s));
        CHECK(clock::now() - begin >= 250ms);
        CHECK(clock::now() - begin < 2s);
    }

    SECTION("Both off") {
        REQUIRE(client.handshake("/off").starts_with("HTTP/1.1 101"));
        CHECK_FALSE(client.read_frame(500ms).has_value());
        client.send_frame(OP_TEXT, "still");
        auto answer = next_data_frame(client);
        REQUIRE(answer);
        CHECK(answer->payload == "5");
    }
}

TEST_CASE("A WebSocket byte stream has no keepalive unless the options set one", "[websocket][options][stream][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    received_state state;
    http::server server;
    stream_route(server, "/unset", {}, state);
    stream_route(server, "/zero", {.ping_interval = 0ms, .idle_timeout = 0ms}, state);
    stream_route(server, "/ping", {.ping_interval = 100ms}, state);
    stream_route(server, "/idle", {.ping_interval = 0ms, .idle_timeout = 300ms}, state);
    running_server running(server, kind);
    raw_ws_client client(running.endpoint());
    auto stream_keepalive = [&] {
        std::scoped_lock lock(state.mutex);
        return std::make_pair(state.stream_ping, state.stream_idle);
    };

    SECTION("Unset: off") {
        REQUIRE(client.handshake("/unset").starts_with("HTTP/1.1 101"));
        CHECK_FALSE(client.read_frame(500ms).has_value());
        auto [ping, idle] = stream_keepalive();
        CHECK(ping == 0ms);
        CHECK(idle == 0ms);
    }

    SECTION("0: off") {
        REQUIRE(client.handshake("/zero").starts_with("HTTP/1.1 101"));
        CHECK_FALSE(client.read_frame(500ms).has_value());
        auto [ping, idle] = stream_keepalive();
        CHECK(ping == 0ms);
        CHECK(idle == 0ms);
    }

    SECTION("ping_interval") {
        REQUIRE(client.handshake("/ping").starts_with("HTTP/1.1 101"));
        REQUIRE(pinged_within(client, 2s));
        auto [ping, idle] = stream_keepalive();
        CHECK(ping == 100ms);
        CHECK(idle == 0ms);
    }

    SECTION("idle_timeout") {
        REQUIRE(client.handshake("/idle").starts_with("HTTP/1.1 101"));
        auto begin = clock::now();
        REQUIRE(closed_idle_within(client, 3s));
        CHECK(clock::now() - begin >= 250ms);
        auto [ping, idle] = stream_keepalive();
        CHECK(ping == 0ms);
        CHECK(idle == 300ms);
    }
}

TEST_CASE("upgrade_websocket negotiates the subprotocols of its options", "[websocket][options][protocol][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    received_state state;
    http::server server;
    sizes_route(server, "/required", {.protocols = {"a", "b"}}, state);
    sizes_route(server, "/optional", {.protocols = {"a", "b"}, .protocol_required = false}, state);
    running_server running(server, kind);

    struct offer {
        std::string header;     // Sec-WebSocket-Protocol, empty for none
        std::string accepted;   // the subprotocol accepted, empty for none
    };
    auto required = GENERATE(true, false);
    auto path = required ? std::string("/required") : std::string("/optional");

    auto handshake = [&](const std::string& offered) {
        raw_ws_client client(running.endpoint());
        auto headers = offered.empty() ? std::string() : "Sec-WebSocket-Protocol: " + offered + "\r\n";
        return client.handshake(path, headers);
    };
    auto accepted_with = [](const std::string& response, const std::string& protocol) {
        if (!response.starts_with("HTTP/1.1 101")) return false;
        bool has = response.find("Sec-WebSocket-Protocol:") != std::string::npos;
        if (protocol.empty()) return !has;
        return response.find("Sec-WebSocket-Protocol: " + protocol + "\r\n") != std::string::npos;
    };
    INFO("protocol_required " << required);

    SECTION("None offered: 400 if required, else accepted without a subprotocol") {
        auto response = handshake("");
        INFO(response);
        if (required) CHECK(response.starts_with("HTTP/1.1 400"));
        else CHECK(accepted_with(response, ""));
    }

    SECTION("One of them offered: accepted with it") {
        auto offered = GENERATE(std::string("a"), std::string("b"));
        auto response = handshake(offered);
        INFO(response);
        CHECK(accepted_with(response, offered));
    }

    SECTION("Several offered: accepted with the first one the client offers that is in the list") {
        auto [offered, accepted] = GENERATE(offer{"b, a", "b"}, offer{"a, b", "a"}, offer{"c, b, a", "b"},
                                            offer{"c,a", "a"});
        auto response = handshake(offered);
        INFO(offered << "\n" << response);
        CHECK(accepted_with(response, accepted));
    }

    SECTION("Only others offered: 400") {
        auto offered = GENERATE(std::string("c"), std::string("c, d"));
        auto response = handshake(offered);
        INFO(response);
        CHECK(response.starts_with("HTTP/1.1 400"));
    }
}

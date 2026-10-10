#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>

#include "server_test_helpers.hpp"
#include "../fixtures/stream_test_helpers.hpp"

// Large messages through the message interface: the server websocket_connection and the
// client websocket_client deliver whole messages, whatever their size and fragmentation

using namespace thinger;
using namespace std::chrono_literals;
using namespace stream_test;
using server_test::running_server;

namespace {

    void echo_route(http::server& server) {
        server.get("/echo", [](http::request& req, http::response& res) {
            res.upgrade_websocket([](std::shared_ptr<http::websocket_connection> ws) {
                ws->on_message([ws](std::string message, bool binary) {
                    if (binary) ws->send_binary(std::move(message));
                    else ws->send_text(std::move(message));
                });
            });
        });
    }

    // Text that is valid UTF-8
    std::string text(size_t size) {
        std::string data(size, 'a');
        for (size_t i = 0; i < size; ++i) data[i] = char('a' + i % 26);
        return data;
    }

}

TEST_CASE("WebSocket messages larger than a read are echoed whole", "[websocket][message][integration]") {
    http::server server;
    echo_route(server);
    running_server running(server);
    auto url = "ws://127.0.0.1:" + std::to_string(running.port) + "/echo";

    for (size_t size : {size_t(64 * 1024), size_t(100 * 1024), size_t(1024 * 1024)}) {
        DYNAMIC_SECTION("Library client, " << size << " bytes") {
            http::client client;
            auto ws = client.websocket(url);
            REQUIRE(ws);

            auto binary = pattern(size, unsigned(size));
            REQUIRE(ws->send_binary(binary));
            auto [received, is_binary] = ws->receive();
            REQUIRE(is_binary);
            REQUIRE(received.size() == size);
            REQUIRE(received == binary);

            auto message = text(size);
            REQUIRE(ws->send_text(message));
            auto [received_text, text_binary] = ws->receive();
            REQUIRE_FALSE(text_binary);
            REQUIRE(received_text == message);
            ws->close();
        }

        DYNAMIC_SECTION("Raw client, one frame and fragmented, " << size << " bytes") {
            raw_ws_client client(running.port);
            REQUIRE(client.handshake("/echo").starts_with("HTTP/1.1 101"));

            auto binary = pattern(size, unsigned(size) + 1);
            client.send_frame(OP_BINARY, binary);
            auto echo = client.read_frame(10s);
            REQUIRE(echo);
            REQUIRE(echo->opcode == OP_BINARY);
            REQUIRE(echo->payload == binary);

            // fragmented: frames of uneven sizes
            auto message = text(size);
            size_t offset = 0;
            size_t piece = 1000;
            while (offset < message.size()) {
                auto chunk = std::min(piece, message.size() - offset);
                bool last = offset + chunk == message.size();
                client.send_frame(offset == 0 ? OP_TEXT : OP_CONTINUATION,
                                  std::string_view(message).substr(offset, chunk), last);
                offset += chunk;
                piece = piece * 3 + 7;
            }
            auto text_echo = client.read_frame(10s);
            REQUIRE(text_echo);
            REQUIRE(text_echo->opcode == OP_TEXT);
            REQUIRE(text_echo->payload == message);
        }
    }
}

#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/websocket_connection.hpp>

#include "server_test_helpers.hpp"
#include "../fixtures/stream_test_helpers.hpp"
#include <algorithm>
#include <atomic>
#include <future>
#include <string_view>
#include <thread>

// Large messages through the message interface: the server websocket_connection and the
// client websocket_client deliver whole messages, whatever their size and fragmentation

using namespace thinger;
using namespace std::chrono_literals;
using namespace stream_test;
using server_test::running_server;

namespace {

    void echo_route(http::server& server) {
        server.get("/echo", [](http::request& req, http::response& res) {
            res.upgrade_websocket({}, [](std::shared_ptr<http::websocket_connection> ws) {
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

TEST_CASE("WebSocket messages larger than a read are echoed whole", "[websocket][message][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    echo_route(server);
    running_server running(server, kind);
    auto url = running.url("/echo");

    for (size_t size : {size_t(64 * 1024), size_t(100 * 1024), size_t(1024 * 1024)}) {
        DYNAMIC_SECTION("Library client, " << size << " bytes") {
            http::client client;
            running.configure(client);
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
            raw_ws_client client(running.endpoint());
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

namespace {

    // An echo route at `path` whose largest message is `max_message_size`
    void echo_route(http::server& server, const std::string& path, size_t max_message_size) {
        server.get(path, [max_message_size](http::request& req, http::response& res) {
            http::websocket_accept_options options;
            options.max_message_size = max_message_size;
            res.upgrade_websocket(options, [](std::shared_ptr<http::websocket_connection> ws) {
                ws->on_message([ws](std::string message, bool binary) {
                    if (binary) ws->send_binary(std::move(message));
                    else ws->send_text(std::move(message));
                });
            });
        });
    }

    // Send `data` in pieces growing from one byte, with a pause after each of the first ones
    void send_in_pieces(raw_ws_client& client, std::string_view data) {
        size_t piece = 1;
        for (size_t offset = 0, sent = 0; offset < data.size(); ++sent) {
            auto chunk = std::min(piece, data.size() - offset);
            client.send_raw(std::string(data.substr(offset, chunk)));
            offset += chunk;
            piece = std::min<size_t>(piece * 2 + 1, 64 * 1024);
            if (sent < 12) std::this_thread::sleep_for(2ms);
        }
    }

    // The header of a masked frame announcing `size` bytes, with a zero key: the payload bytes
    // after it go as they are
    std::string frame_header_announcing(uint8_t opcode, uint64_t size, bool fin = true) {
        std::string header{char((fin ? 0x80 : 0x00) | opcode), char(0x80 | 127)};
        for (int i = 7; i >= 0; --i) header.push_back(char((size >> (i * 8)) & 0xFF));
        return header.append(4, '\0');
    }

    // The server closes with `code`, its close frame before anything else (the connection then
    // ends at its close deadline: the rest of the frame refused never comes)
    void check_closed_with(raw_ws_client& client, uint16_t code) {
        auto frame = client.read_frame(5s);
        REQUIRE(frame);
        CHECK(frame->opcode == OP_CLOSE);
        CHECK(frame->close_code() == code);
    }

}

// A large message trickled in pieces from one byte, in two frames with a ping between them: the
// buffer grows as the bytes arrive, the ping is answered meanwhile, and the message is echoed
// whole and byte-exact
TEST_CASE("A WebSocket message trickled in pieces is delivered whole", "[websocket][message][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    echo_route(server);
    running_server running(server, kind);

    raw_ws_client client(running.endpoint());
    REQUIRE(client.handshake("/echo").starts_with("HTTP/1.1 101"));

    auto message = pattern(1024 * 1024, 7);
    auto first = std::string_view(message).substr(0, 300 * 1024);
    auto rest = std::string_view(message).substr(first.size());
    send_in_pieces(client, make_frame(OP_BINARY, first, false));
    client.send_frame(OP_PING, "between");
    auto pong = client.read_frame(5s);
    REQUIRE(pong);
    CHECK(pong->opcode == OP_PONG);
    CHECK(pong->payload == "between");
    send_in_pieces(client, make_frame(OP_CONTINUATION, rest, true));

    auto echo = client.read_frame(10s);
    REQUIRE(echo);
    CHECK(echo->opcode == OP_BINARY);
    REQUIRE(echo->payload.size() == message.size());
    CHECK(echo->payload == message);
}

// The largest message allowed is delivered, one byte more closes with 1009 as soon as the frame
// header that crosses the bound is read (with a byte of its payload), without waiting for the
// rest; also with a bound smaller than a read
TEST_CASE("A WebSocket message larger than max_message_size closes with 1009",
          "[websocket][message][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    echo_route(server);
    echo_route(server, "/small", 16);
    echo_route(server, "/limited", 4096);
    running_server running(server, kind);
    raw_ws_client client(running.endpoint());

    SECTION("A header announcing past the default bound") {
        REQUIRE(client.handshake("/echo").starts_with("HTTP/1.1 101"));
        client.send_raw(frame_header_announcing(OP_BINARY, http::websocket_connection::MAX_BUFFER_SIZE + 1) + "x");
        check_closed_with(client, 1009);
    }
    SECTION("Exactly the bound, then one byte more") {
        REQUIRE(client.handshake("/limited").starts_with("HTTP/1.1 101"));
        auto largest = pattern(4096, 3);
        send_in_pieces(client, make_frame(OP_BINARY, largest));
        auto echo = client.read_frame(5s);
        REQUIRE(echo);
        CHECK(echo->payload == largest);
        client.send_raw(frame_header_announcing(OP_BINARY, 4097) + "x");
        check_closed_with(client, 1009);
    }
    SECTION("Fragments adding up past the bound") {
        REQUIRE(client.handshake("/limited").starts_with("HTTP/1.1 101"));
        client.send_frame(OP_BINARY, pattern(3000, 4), false);
        client.send_raw(frame_header_announcing(OP_CONTINUATION, 1097) + "x");
        check_closed_with(client, 1009);
    }
    SECTION("A bound smaller than a read") {
        REQUIRE(client.handshake("/small").starts_with("HTTP/1.1 101"));
        client.send_frame(OP_TEXT, "sixteen bytes ok");
        auto echo = client.read_frame(5s);
        REQUIRE(echo);
        CHECK(echo->payload == "sixteen bytes ok");
        client.send_frame(OP_TEXT, "seventeen bytes!!");
        check_closed_with(client, 1009);
    }
}

namespace {

    // An echo route at `path` whose connections are told to `closed` when they close
    void echo_route_telling_close(http::server& server, const std::string& path,
                                  std::promise<asio::websocket_close>& closed) {
        server.get(path, [&closed](http::request& req, http::response& res) {
            res.upgrade_websocket({}, [&closed](std::shared_ptr<http::websocket_connection> ws) {
                ws->on_message([ws](std::string message, bool binary) {
                    if (binary) ws->send_binary(std::move(message));
                    else ws->send_text(std::move(message));
                });
                ws->on_close([&closed](const asio::websocket_close& status) { closed.set_value(status); });
            });
        });
    }

}

// Frames a client sends right after its upgrade request, in the same write, are read with the
// request; the message interface delivers them once it starts reading, as the frames that come
// after the 101 (as Node's ws and Go's gorilla/websocket do)
TEST_CASE("WebSocket frames sent right after the upgrade request are delivered",
          "[websocket][message][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    echo_route(server);
    std::promise<asio::websocket_close> closed;
    echo_route_telling_close(server, "/closing", closed);
    running_server running(server, kind);
    raw_ws_client client(running.endpoint());

    SECTION("One message, and the connection stays open") {
        REQUIRE(client.handshake("/echo", "", make_frame(OP_TEXT, "hello")).starts_with("HTTP/1.1 101"));
        auto echo = client.read_frame(5s);
        REQUIRE(echo);
        CHECK(echo->opcode == OP_TEXT);
        CHECK(echo->payload == "hello");

        client.send_frame(OP_TEXT, "again");
        auto again = client.read_frame(5s);
        REQUIRE(again);
        CHECK(again->payload == "again");
    }
    SECTION("Two messages and part of a third, whose rest comes later") {
        auto third = pattern(3000, 9);
        auto third_frame = make_frame(OP_BINARY, third);
        auto early = make_frame(OP_TEXT, "first") + make_frame(OP_BINARY, pattern(200, 8)) + third_frame.substr(0, 1000);
        REQUIRE(client.handshake("/echo", "", early).starts_with("HTTP/1.1 101"));
        auto first = client.read_frame(5s);
        REQUIRE(first);
        CHECK(first->opcode == OP_TEXT);
        CHECK(first->payload == "first");
        auto second = client.read_frame(5s);
        REQUIRE(second);
        CHECK(second->opcode == OP_BINARY);
        CHECK(second->payload == pattern(200, 8));

        // the third is not whole yet
        CHECK_FALSE(client.read_frame(200ms));
        client.send_raw(third_frame.substr(1000));
        auto last = client.read_frame(5s);
        REQUIRE(last);
        CHECK(last->opcode == OP_BINARY);
        CHECK(last->payload == third);
    }
    SECTION("A close frame") {
        REQUIRE(client.handshake("/closing", "", make_frame(OP_CLOSE, close_payload(1000))).starts_with("HTTP/1.1 101"));
        auto reply = client.read_frame(5s);
        REQUIRE(reply);
        CHECK(reply->opcode == OP_CLOSE);
        CHECK(reply->close_code() == 1000);
        auto told = closed.get_future();
        REQUIRE(told.wait_for(5s) == std::future_status::ready);
        auto status = told.get();
        CHECK(status.code == 1000);
        CHECK(status.remote);
        CHECK(client.closed_within(5s));
    }
}

// The frames read with the request go through the same checks as the ones after the 101
TEST_CASE("WebSocket frames sent right after the upgrade request are checked as any other",
          "[websocket][message][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    echo_route(server);
    echo_route(server, "/small", 16);
    running_server running(server, kind);
    raw_ws_client client(running.endpoint());

    SECTION("An unmasked frame closes with 1002") {
        REQUIRE(client.handshake("/echo", "", make_frame(OP_TEXT, "hello", true, false)).starts_with("HTTP/1.1 101"));
        check_closed_with(client, 1002);
    }
    SECTION("Text that is not valid UTF-8 closes with 1007") {
        REQUIRE(client.handshake("/echo", "", make_frame(OP_TEXT, "\xff\xfe")).starts_with("HTTP/1.1 101"));
        check_closed_with(client, 1007);
    }
    SECTION("A message larger than max_message_size closes with 1009") {
        auto early = make_frame(OP_TEXT, "sixteen bytes ok") + make_frame(OP_TEXT, "seventeen bytes!!");
        REQUIRE(client.handshake("/small", "", early).starts_with("HTTP/1.1 101"));
        auto echo = client.read_frame(5s);
        REQUIRE(echo);
        CHECK(echo->payload == "sixteen bytes ok");
        check_closed_with(client, 1009);
    }
}

// A handler that pauses the reads before the WebSocket starts: the message read with the request
// waits for resume_reading()
TEST_CASE("WebSocket frames sent right after the upgrade request wait while reading is paused",
          "[websocket][message][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    std::atomic<size_t> received{0};
    std::promise<std::shared_ptr<http::websocket_connection>> created;
    server.get("/paused", [&](http::request& req, http::response& res) {
        res.upgrade_websocket({}, [&](std::shared_ptr<http::websocket_connection> ws) {
            ws->pause_reading();
            ws->on_message([&received, ws](std::string message, bool binary) {
                ++received;
                ws->send_text(std::move(message));
            });
            created.set_value(ws);
        });
    });
    running_server running(server, kind);
    raw_ws_client client(running.endpoint());

    REQUIRE(client.handshake("/paused", "", make_frame(OP_TEXT, "hello")).starts_with("HTTP/1.1 101"));
    auto future = created.get_future();
    REQUIRE(future.wait_for(5s) == std::future_status::ready);
    auto ws = future.get();
    CHECK_FALSE(client.read_frame(300ms));
    CHECK(received == 0);

    ws->resume_reading();
    auto echo = client.read_frame(5s);
    REQUIRE(echo);
    CHECK(echo->payload == "hello");
    CHECK(received == 1);
}

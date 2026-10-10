#include <catch2/catch_test_macros.hpp>
#include <thinger/asio/socket_pipe.hpp>
#include <thinger/asio/sockets/tcp_socket.hpp>
#include <thinger/asio/sockets/websocket.hpp>
#include <thinger/http/client/async_client.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/util/base64.hpp>
#include <thinger/util/sha1.hpp>

#include "server_test_helpers.hpp"
#include "../fixtures/stream_test_helpers.hpp"

#include <future>
#include <mutex>
#include <vector>

// The server-side WebSocket used as a byte stream socket: protocols running on it, subprotocol
// negotiation, control frames, closing, and tunnels with socket_pipe

using namespace thinger;
using namespace std::chrono_literals;
using namespace stream_test;
using server_test::running_server;

namespace {

    // Value set once from the server thread, read from the test
    template<typename T>
    class result_slot {
    public:
        void set(T value) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!value_) value_ = std::move(value);
        }
        std::optional<T> wait(std::chrono::milliseconds timeout = 5s) {
            auto deadline = std::chrono::steady_clock::now() + timeout;
            for (;;) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (value_) return value_;
                }
                if (std::chrono::steady_clock::now() > deadline) return std::nullopt;
                std::this_thread::sleep_for(5ms);
            }
        }
    private:
        std::mutex mutex_;
        std::optional<T> value_;
    };

    std::string be32(uint32_t value) {
        return {char(value >> 24), char((value >> 16) & 0xFF), char((value >> 8) & 0xFF), char(value & 0xFF)};
    }

    // Length-prefixed echo protocol, as a protocol written for TCP: a 4-byte big-endian length
    // and the payload, answered with the same message
    awaitable<boost::system::error_code> length_prefixed_echo(std::shared_ptr<asio::socket> socket) {
        for (;;) {
            uint8_t header[4];
            auto [ec, bytes] = co_await socket->read(header, sizeof(header));
            if (ec) co_return ec;
            uint32_t length = (uint32_t(header[0]) << 24) | (uint32_t(header[1]) << 16) |
                              (uint32_t(header[2]) << 8) | header[3];
            std::vector<uint8_t> payload(length);
            if (length > 0) {
                auto [payload_ec, payload_bytes] = co_await socket->read(payload.data(), length);
                if (payload_ec) co_return payload_ec;
            }
            auto [write_ec, written] = co_await socket->write(
                std::vector<boost::asio::const_buffer>{boost::asio::buffer(header), boost::asio::buffer(payload)});
            if (write_ec) co_return write_ec;
        }
    }

    // TCP echo server on its own thread; counts the sessions ended by their peer
    struct tcp_echo_server {
        boost::asio::io_context ioc;
        tcp::acceptor acceptor{ioc, {tcp::v4(), 0}};
        std::atomic<int> sessions_ended{0};
        std::thread thread;

        explicit tcp_echo_server(std::string quit_word = "") : quit_(std::move(quit_word)) {
            co_spawn(ioc, accept_loop(), detached);
            thread = std::thread([this] { ioc.run(); });
        }

        ~tcp_echo_server() {
            boost::asio::post(ioc, [this] { acceptor.close(); });
            ioc.stop();
            thread.join();
        }

        uint16_t port() const { return acceptor.local_endpoint().port(); }

    private:
        awaitable<void> accept_loop() {
            for (;;) {
                auto [ec, sock] = co_await acceptor.async_accept(use_nothrow_awaitable);
                if (ec) co_return;
                co_spawn(ioc, session(std::move(sock)), detached);
            }
        }

        awaitable<void> session(tcp::socket sock) {
            std::array<char, 16384> buffer;
            for (;;) {
                auto [ec, n] = co_await sock.async_read_some(boost::asio::buffer(buffer), use_nothrow_awaitable);
                if (ec) break;
                // the quit word makes this end close the connection
                if (!quit_.empty() && std::string_view(buffer.data(), n) == quit_) {
                    boost::system::error_code ignored;
                    sock.shutdown(tcp::socket::shutdown_both, ignored);
                    sock.close(ignored);
                    co_return;
                }
                auto [wec, wn] = co_await boost::asio::async_write(sock, boost::asio::buffer(buffer.data(), n),
                                                                   use_nothrow_awaitable);
                if (wec) break;
            }
            sessions_ended++;
        }

        std::string quit_;
    };

    // Tunnel from an upgraded WebSocket to a TCP server, on the io_context of the WebSocket
    void tunnel_to_tcp(std::shared_ptr<asio::websocket> ws, uint16_t port, result_slot<bool>* ended = nullptr) {
        co_spawn(ws->get_io_context(), [ws, port, ended]() -> awaitable<void> {
            auto backend = std::make_shared<asio::tcp_socket>("backend", ws->get_io_context());
            if (co_await backend->connect("127.0.0.1", std::to_string(port), 5s)) {
                ws->close();
                co_return;
            }
            auto pipe = std::make_shared<asio::socket_pipe>(ws, backend);
            co_await pipe->run();
            if (ended) ended->set(true);
        }, detached);
    }

    std::string header_value(const std::string& response, const std::string& name) {
        auto position = response.find(name + ": ");
        if (position == std::string::npos) return {};
        position += name.size() + 2;
        return response.substr(position, response.find("\r\n", position) - position);
    }

}

TEST_CASE("WebSocket stream: a binary protocol reads fragmented messages as one stream", "[websocket][stream][integration]") {
    http::server server;
    result_slot<boost::system::error_code> ended;
    server.get("/echo", [&](http::request& req, http::response& res) {
        res.upgrade_websocket_stream([&](std::shared_ptr<asio::websocket> ws, std::string protocol) {
            co_spawn(ws->get_io_context(), [ws, &ended]() -> awaitable<void> {
                ended.set(co_await length_prefixed_echo(ws));
            }, detached);
        });
    });
    running_server running(server);

    raw_ws_client client(running.port);
    auto response = client.handshake("/echo");
    REQUIRE(response.starts_with("HTTP/1.1 101"));

    // Messages of the protocol, as a stream
    std::string stream;
    for (auto payload : {std::string("hello"), std::string(), pattern(70000, 1), std::string("x"), pattern(300, 2)}) {
        stream += be32(uint32_t(payload.size())) + payload;
    }

    // Cut anywhere, sent as single messages and as fragments of messages, with control
    // frames and empty frames in between
    std::vector<size_t> cuts = {1, 2, 3, 5, 9, 200, 65000, 4000, 7, 1500};
    size_t offset = 0;
    size_t piece = 0;
    while (offset < stream.size()) {
        size_t size = std::min(cuts[piece % cuts.size()], stream.size() - offset);
        auto data = std::string_view(stream).substr(offset, size);
        switch (piece % 4) {
            case 0:
                client.send_frame(OP_BINARY, data);
                break;
            case 1: {
                // fragmented: first frame, a ping, an empty continuation, the rest
                auto half = data.size() / 2;
                client.send_frame(OP_BINARY, data.substr(0, half), false);
                client.send_frame(OP_PING, "between");
                client.send_frame(OP_CONTINUATION, "", false);
                client.send_frame(OP_CONTINUATION, data.substr(half), true);
                break;
            }
            case 2:
                // a text message is part of the stream as well
                client.send_frame(OP_BINARY, "", true);
                client.send_frame(OP_TEXT, data);
                break;
            default: {
                // one byte per frame
                for (size_t i = 0; i < data.size(); ++i) {
                    client.send_frame(i == 0 ? OP_BINARY : OP_CONTINUATION, data.substr(i, 1), i + 1 == data.size());
                }
                break;
            }
        }
        offset += size;
        piece++;
    }

    // Answered with the same stream (pongs are dropped)
    std::string received;
    auto deadline = std::chrono::steady_clock::now() + 10s;
    while (received.size() < stream.size() && std::chrono::steady_clock::now() < deadline) {
        auto frame = client.read_frame(5s);
        REQUIRE(frame);
        if (frame->opcode == OP_PONG) {
            REQUIRE(frame->payload == "between");
            continue;
        }
        REQUIRE(frame->opcode == OP_BINARY);
        REQUIRE(frame->fin);
        received += frame->payload;
    }
    REQUIRE(received.size() == stream.size());
    REQUIRE(received == stream);

    // A close frame ends the stream as the end of a TCP stream does
    client.send_frame(OP_CLOSE, close_payload(1000));
    auto close = client.read_frame(5s);
    REQUIRE(close);
    REQUIRE(close->opcode == OP_CLOSE);
    REQUIRE(close->close_code() == 1000);
    REQUIRE(client.closed_within(2s));
    auto end = ended.wait();
    REQUIRE(end);
    REQUIRE(*end == boost::asio::error::eof);
}

TEST_CASE("WebSocket stream: read_until and exact reads over frames", "[websocket][stream][integration]") {
    http::server server;
    server.get("/lines", [](http::request& req, http::response& res) {
        res.upgrade_websocket_stream([](std::shared_ptr<asio::websocket> ws, std::string) {
            co_spawn(ws->get_io_context(), [ws]() -> awaitable<void> {
                boost::asio::streambuf buffer;
                for (;;) {
                    auto [ec, bytes] = co_await ws->read_until(buffer, "\r\n");
                    if (ec) co_return;
                    std::string line(static_cast<const char*>(buffer.data().data()), bytes - 2);
                    buffer.consume(bytes);
                    // a line "read N" is followed by N bytes
                    if (line.starts_with("read ")) {
                        size_t size = std::stoul(line.substr(5));
                        size_t held = std::min(size, buffer.size());
                        auto [read_ec, read_bytes] = co_await ws->read(buffer, size - held);
                        if (read_ec) co_return;
                        line = std::string(static_cast<const char*>(buffer.data().data()), size);
                        buffer.consume(size);
                    }
                    for (auto& c : line) c = char(std::toupper(c));
                    co_await ws->write(line + "\n");
                }
            }, detached);
        });
    });
    running_server running(server);

    raw_ws_client client(running.port);
    REQUIRE(client.handshake("/lines").starts_with("HTTP/1.1 101"));
    client.send_frame(OP_BINARY, "hel");
    client.send_frame(OP_BINARY, "lo\r\nwor");
    client.send_frame(OP_BINARY, "ld\r");
    client.send_frame(OP_BINARY, "\nread 10\r\nabc");
    client.send_frame(OP_BINARY, "defghij");
    REQUIRE(client.read_data(23) == "HELLO\nWORLD\nABCDEFGHIJ\n");
}

TEST_CASE("WebSocket stream: subprotocol negotiation", "[websocket][stream][integration]") {
    http::server server;
    result_slot<std::string> negotiated;
    auto handler = [&](std::shared_ptr<asio::websocket> ws, std::string protocol) {
        negotiated.set(protocol + "|" + ws->get_protocol());
        ws->close();
    };
    server.get("/mqtt", [&](http::request& req, http::response& res) {
        res.upgrade_websocket_stream({.protocols = {"mqtt", "mqttv3.1", "mqttv3.1.1"}}, handler);
    });
    server.get("/optional", [&](http::request& req, http::response& res) {
        res.upgrade_websocket_stream({.protocols = {"iotmp"}, .protocol_required = false}, handler);
    });
    server.get("/any", [&](http::request& req, http::response& res) {
        res.upgrade_websocket_stream(handler);
    });
    server.get("/legacy", [&](http::request& req, http::response& res) {
        res.upgrade_websocket([](std::shared_ptr<http::websocket_connection>) {}, {"chat", "json"});
    });
    running_server running(server);

    auto handshake = [&](const std::string& path, const std::string& protocols) {
        raw_ws_client client(running.port);
        return client.handshake(path, protocols.empty() ? "" : "Sec-WebSocket-Protocol: " + protocols + "\r\n");
    };

    SECTION("One of the alternatives") {
        auto response = handshake("/mqtt", "mqttv3.1");
        REQUIRE(response.starts_with("HTTP/1.1 101"));
        REQUIRE(header_value(response, "Sec-WebSocket-Protocol") == "mqttv3.1");
        REQUIRE(negotiated.wait() == "mqttv3.1|mqttv3.1");
    }

    SECTION("The first one the client offers that is supported") {
        auto response = handshake("/mqtt", "foo, mqttv3.1.1,mqtt");
        REQUIRE(response.starts_with("HTTP/1.1 101"));
        REQUIRE(header_value(response, "Sec-WebSocket-Protocol") == "mqttv3.1.1");
        REQUIRE(negotiated.wait() == "mqttv3.1.1|mqttv3.1.1");
    }

    SECTION("Several headers") {
        raw_ws_client client(running.port);
        auto response = client.handshake("/mqtt", "Sec-WebSocket-Protocol: foo\r\nSec-WebSocket-Protocol: mqtt\r\n");
        REQUIRE(response.starts_with("HTTP/1.1 101"));
        REQUIRE(header_value(response, "Sec-WebSocket-Protocol") == "mqtt");
    }

    SECTION("None supported: rejected") {
        REQUIRE(handshake("/mqtt", "foo, bar").starts_with("HTTP/1.1 400"));
        REQUIRE_FALSE(negotiated.wait(200ms));
    }

    SECTION("None offered when required: rejected") {
        REQUIRE(handshake("/mqtt", "").starts_with("HTTP/1.1 400"));
        REQUIRE_FALSE(negotiated.wait(200ms));
    }

    SECTION("Optional: accepted without a subprotocol") {
        auto response = handshake("/optional", "foo");
        REQUIRE(response.starts_with("HTTP/1.1 101"));
        REQUIRE(header_value(response, "Sec-WebSocket-Protocol").empty());
        REQUIRE(negotiated.wait() == "|");
    }

    SECTION("Optional: accepted with it") {
        REQUIRE(header_value(handshake("/optional", "iotmp"), "Sec-WebSocket-Protocol") == "iotmp");
        REQUIRE(negotiated.wait() == "iotmp|iotmp");
    }

    SECTION("No subprotocols: any client accepted without one") {
        auto response = handshake("/any", "something");
        REQUIRE(response.starts_with("HTTP/1.1 101"));
        REQUIRE(header_value(response, "Sec-WebSocket-Protocol").empty());
        REQUIRE(negotiated.wait() == "|");
    }

    SECTION("Not a WebSocket request") {
        auto result = server_test::raw_exchange(running.port, "GET /mqtt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(result.data.starts_with("HTTP/1.1 426"));
    }

    SECTION("Message interface: a list offered") {
        auto response = handshake("/legacy", "foo, json");
        REQUIRE(response.starts_with("HTTP/1.1 101"));
        REQUIRE(header_value(response, "Sec-WebSocket-Protocol") == "json");
        REQUIRE(handshake("/legacy", "foo").starts_with("HTTP/1.1 400"));
        REQUIRE(handshake("/legacy", "").starts_with("HTTP/1.1 400"));
    }
}

TEST_CASE("WebSocket stream: frames sent along with the handshake are kept", "[websocket][stream][integration]") {
    http::server server;
    server.get("/echo", [](http::request& req, http::response& res) {
        res.upgrade_websocket_stream([](std::shared_ptr<asio::websocket> ws, std::string) {
            co_spawn(ws->get_io_context(), length_prefixed_echo(ws), detached);
        });
    });
    running_server running(server);

    raw_ws_client client(running.port);
    auto early = make_frame(OP_BINARY, be32(5) + "early") + make_frame(OP_BINARY, be32(3) + "fo", false);
    REQUIRE(client.handshake("/echo", "", early).starts_with("HTTP/1.1 101"));
    client.send_frame(OP_CONTINUATION, "o");
    REQUIRE(client.read_data(16) == be32(5) + "early" + be32(3) + "foo");
}

TEST_CASE("WebSocket stream: concurrent writes are sent whole and in order", "[websocket][stream][integration]") {
    constexpr int writers = 4;
    constexpr int messages = 20;
    constexpr size_t size = 50000;

    http::server server;
    server.get("/flood", [](http::request& req, http::response& res) {
        res.upgrade_websocket_stream([](std::shared_ptr<asio::websocket> ws, std::string) {
            for (int writer = 0; writer < writers; ++writer) {
                co_spawn(ws->get_io_context(), [ws, writer]() -> awaitable<void> {
                    for (int i = 0; i < messages; ++i) {
                        std::string message(size, char(writer * messages + i));
                        auto [ec, bytes] = co_await ws->write(message);
                        if (ec || bytes != size) co_return;
                    }
                }, detached);
            }
        });
    });
    running_server running(server);

    raw_ws_client client(running.port);
    REQUIRE(client.handshake("/flood").starts_with("HTTP/1.1 101"));
    std::vector<int> next(writers, 0);
    for (int received = 0; received < writers * messages; ++received) {
        auto frame = client.read_frame(10s);
        REQUIRE(frame);
        REQUIRE(frame->opcode == OP_BINARY);
        REQUIRE(frame->payload.size() == size);
        REQUIRE(frame->payload.find_first_not_of(frame->payload[0]) == std::string::npos);
        int value = uint8_t(frame->payload[0]);
        int writer = value / messages;
        REQUIRE(writer < writers);
        REQUIRE(value % messages == next[writer]);
        next[writer]++;
    }
}

TEST_CASE("WebSocket stream: text messages", "[websocket][stream][integration]") {
    http::server server;
    server.get("/text", [](http::request& req, http::response& res) {
        res.upgrade_websocket_stream({.binary = false}, [](std::shared_ptr<asio::websocket> ws, std::string) {
            co_spawn(ws->get_io_context(), [ws]() -> awaitable<void> {
                co_await ws->write(std::string_view("{\"hello\":1}"));
                ws->set_binary(true);
                co_await ws->write(std::string_view("raw"));
                co_await ws->write(std::string_view(""));     // nothing sent
                co_await ws->write(std::string_view("end"));
            }, detached);
        });
    });
    running_server running(server);

    raw_ws_client client(running.port);
    REQUIRE(client.handshake("/text").starts_with("HTTP/1.1 101"));
    auto text = client.read_frame();
    REQUIRE(text);
    REQUIRE(text->opcode == OP_TEXT);
    REQUIRE(text->payload == "{\"hello\":1}");
    auto binary = client.read_frame();
    REQUIRE(binary);
    REQUIRE(binary->opcode == OP_BINARY);
    REQUIRE(binary->payload == "raw");
    auto end = client.read_frame();
    REQUIRE(end);
    REQUIRE(end->payload == "end");
}

TEST_CASE("WebSocket stream: ping, keepalive and idle timeout", "[websocket][stream][integration]") {
    http::server server;
    result_slot<boost::system::error_code> ended;
    auto echo = [&](std::shared_ptr<asio::websocket> ws, std::string) {
        co_spawn(ws->get_io_context(), [ws, &ended]() -> awaitable<void> {
            ended.set(co_await length_prefixed_echo(ws));
        }, detached);
    };
    server.get("/echo", [&](http::request& req, http::response& res) {
        res.upgrade_websocket_stream(echo);
    });
    server.get("/keepalive", [&](http::request& req, http::response& res) {
        res.upgrade_websocket_stream({.ping_interval = 100ms, .idle_timeout = 600ms}, echo);
    });
    running_server running(server);

    SECTION("Pings are answered") {
        raw_ws_client client(running.port);
        REQUIRE(client.handshake("/echo").starts_with("HTTP/1.1 101"));
        client.send_frame(OP_PING, "are you there?");
        auto pong = client.read_frame();
        REQUIRE(pong);
        REQUIRE(pong->opcode == OP_PONG);
        REQUIRE(pong->payload == "are you there?");
        // a pong without a ping is ignored
        client.send_frame(OP_PONG, "unsolicited");
        client.send_frame(OP_BINARY, be32(2) + "ok");
        REQUIRE(client.read_data(6) == be32(2) + "ok");
    }

    SECTION("A client answering the pings stays connected") {
        raw_ws_client client(running.port);
        REQUIRE(client.handshake("/keepalive").starts_with("HTTP/1.1 101"));
        int pings = 0;
        auto until = std::chrono::steady_clock::now() + 1500ms;
        while (std::chrono::steady_clock::now() < until) {
            auto frame = client.read_frame(300ms);
            if (!frame) continue;
            REQUIRE(frame->opcode == OP_PING);
            client.send_frame(OP_PONG, frame->payload);
            pings++;
        }
        REQUIRE(pings >= 3);
        client.send_frame(OP_BINARY, be32(5) + "alive");
        REQUIRE(client.read_data(9) == be32(5) + "alive");
        REQUIRE_FALSE(ended.wait(10ms));
    }

    SECTION("An idle client is closed") {
        raw_ws_client client(running.port);
        REQUIRE(client.handshake("/keepalive").starts_with("HTTP/1.1 101"));
        auto start = std::chrono::steady_clock::now();
        std::optional<ws_frame> close;
        while (!close && std::chrono::steady_clock::now() - start < 3s) {
            auto frame = client.read_frame(3s);
            REQUIRE(frame);
            if (frame->opcode == OP_CLOSE) close = frame;
            else REQUIRE(frame->opcode == OP_PING);   // not answered
        }
        REQUIRE(close);
        REQUIRE(close->close_code() == 1001);
        REQUIRE(std::chrono::steady_clock::now() - start >= 500ms);
        // the server closes once its close frame is answered
        client.send_frame(OP_CLOSE, close_payload(1001));
        REQUIRE(client.closed_within(2s));
        REQUIRE(ended.wait());
    }
}

TEST_CASE("WebSocket stream: closing", "[websocket][stream][integration]") {
    http::server server;
    result_slot<bool> handler_done;
    server.get("/close", [&](http::request& req, http::response& res) {
        res.upgrade_websocket_stream([&](std::shared_ptr<asio::websocket> ws, std::string) {
            co_spawn(ws->get_io_context(), [ws, &handler_done]() -> awaitable<void> {
                co_await ws->write(std::string_view("bye"));
                ws->close();
                bool closed = !ws->is_open();
                auto [ec, bytes] = co_await ws->write(std::string_view("late"));
                handler_done.set(closed && ec == boost::asio::error::bad_descriptor);
            }, detached);
        });
    });
    server.get("/graceful", [&](http::request& req, http::response& res) {
        res.upgrade_websocket_stream([&](std::shared_ptr<asio::websocket> ws, std::string) {
            co_spawn(ws->get_io_context(), [ws, &handler_done]() -> awaitable<void> {
                co_await ws->write(std::string_view("bye"));
                co_await ws->close_graceful();
                handler_done.set(!ws->is_open());
            }, detached);
        });
    });
    server.get("/echo", [&](http::request& req, http::response& res) {
        res.upgrade_websocket_stream([](std::shared_ptr<asio::websocket> ws, std::string) {
            co_spawn(ws->get_io_context(), length_prefixed_echo(ws), detached);
        });
    });
    server.get("/drop", [&](http::request& req, http::response& res) {
        // released without closing
        res.upgrade_websocket_stream([](std::shared_ptr<asio::websocket>, std::string) {});
    });
    running_server running(server);

    for (std::string path : {"/close", "/graceful", "/drop"}) {
        DYNAMIC_SECTION("Closed by the server: " << path) {
            raw_ws_client client(running.port);
            REQUIRE(client.handshake(path).starts_with("HTTP/1.1 101"));
            if (path != "/drop") REQUIRE(client.read_data(3) == "bye");
            auto close = client.read_frame();
            REQUIRE(close);
            REQUIRE(close->opcode == OP_CLOSE);
            REQUIRE(close->close_code() == (path == "/drop" ? 1001 : 1000));
            // closed as soon as the close frame is answered, not after the timeout
            auto start = std::chrono::steady_clock::now();
            client.send_frame(OP_CLOSE, close_payload(1000));
            REQUIRE(client.closed_within(2s));
            REQUIRE(std::chrono::steady_clock::now() - start < 2s);
            if (path != "/drop") REQUIRE(handler_done.wait() == true);
        }
    }

    SECTION("A peer that does not answer the close frame is closed after the timeout") {
        raw_ws_client client(running.port);
        REQUIRE(client.handshake("/close").starts_with("HTTP/1.1 101"));
        REQUIRE(client.read_data(3) == "bye");
        REQUIRE(client.read_frame()->opcode == OP_CLOSE);
        REQUIRE(client.closed_within(asio::websocket::CLOSE_TIMEOUT + 3s));
    }

    SECTION("Protocol errors close the connection with 1002") {
        raw_ws_client client(running.port);
        REQUIRE(client.handshake("/echo").starts_with("HTTP/1.1 101"));
        // unmasked frame from a client
        client.send_raw(make_frame(OP_BINARY, "unmasked", true, false));
        auto close = client.read_frame();
        REQUIRE(close);
        REQUIRE(close->opcode == OP_CLOSE);
        REQUIRE(close->close_code() == 1002);
        client.send_frame(OP_CLOSE, close_payload(1002));
        REQUIRE(client.closed_within(2s));
    }
}

TEST_CASE("WebSocket stream: tunnel to a TCP server", "[websocket][stream][socket-pipe][integration]") {
    tcp_echo_server backend("quit");
    http::server server;
    result_slot<bool> pipe_ended;
    server.get("/tunnel", [&](http::request& req, http::response& res) {
        res.upgrade_websocket_stream({.protocols = {"binary"}}, [&](std::shared_ptr<asio::websocket> ws, std::string) {
            tunnel_to_tcp(ws, backend.port(), &pipe_ended);
        });
    });
    running_server running(server);

    raw_ws_client client(running.port);
    REQUIRE(client.handshake("/tunnel", "Sec-WebSocket-Protocol: binary\r\n").starts_with("HTTP/1.1 101"));

    SECTION("Large data both ways, closed by the WebSocket end") {
        auto data = pattern(2 * 1024 * 1024, 7);
        constexpr size_t chunk = 64 * 1024;
        for (size_t offset = 0; offset < data.size(); offset += chunk) {
            auto piece = std::string_view(data).substr(offset, chunk);
            // half as one message, half fragmented
            client.send_frame(OP_BINARY, piece.substr(0, chunk / 2));
            client.send_frame(OP_BINARY, piece.substr(chunk / 2, 1000), false);
            client.send_frame(OP_CONTINUATION, piece.substr(chunk / 2 + 1000), true);
            REQUIRE(client.read_data(piece.size()) == piece);
        }

        client.send_frame(OP_CLOSE, close_payload(1000));
        auto close = client.read_frame();
        REQUIRE(close);
        REQUIRE(close->opcode == OP_CLOSE);
        REQUIRE(client.closed_within(2s));
        REQUIRE(pipe_ended.wait());
        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (backend.sessions_ended == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
        REQUIRE(backend.sessions_ended == 1);
    }

    SECTION("Closed by the TCP end") {
        client.send_frame(OP_BINARY, "ping");
        REQUIRE(client.read_data(4) == "ping");
        client.send_frame(OP_BINARY, "quit");
        auto close = client.read_frame();
        REQUIRE(close);
        REQUIRE(close->opcode == OP_CLOSE);
        REQUIRE(close->close_code() == 1000);
        client.send_frame(OP_CLOSE, close_payload(1000));
        REQUIRE(client.closed_within(2s));
        REQUIRE(pipe_ended.wait());
    }

    SECTION("Closed by the WebSocket end without a close frame") {
        client.send_frame(OP_BINARY, "ping");
        REQUIRE(client.read_data(4) == "ping");
        client.close();
        REQUIRE(pipe_ended.wait());
        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (backend.sessions_ended == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
        REQUIRE(backend.sessions_ended == 1);
    }
}

TEST_CASE("WebSocket stream: the client WebSocket used as a socket", "[websocket][stream][socket-pipe][client][integration]") {
    tcp_echo_server backend;
    http::server server;
    server.get("/tunnel", [&](http::request& req, http::response& res) {
        res.upgrade_websocket_stream({.protocols = {"binary"}}, [&](std::shared_ptr<asio::websocket> ws, std::string) {
            tunnel_to_tcp(ws, backend.port());
        });
    });
    running_server running(server);
    auto url = "ws://127.0.0.1:" + std::to_string(running.port) + "/tunnel";

    SECTION("Large data written and read at the same time") {
        http::async_client client;
        result_slot<std::string> result;
        auto data = pattern(4 * 1024 * 1024, 3);
        client.run([&]() -> awaitable<void> {
            auto ws = co_await client.websocket(url, "binary");
            if (!ws || ws->get_protocol() != "binary") {
                result.set("no websocket");
                co_return;
            }
            auto websocket = ws->release_socket();
            websocket->set_binary(true);
            std::shared_ptr<asio::socket> socket = websocket;

            // A protocol written for TCP: a writer and a reader on the same socket
            auto writer = [&]() -> awaitable<void> {
                for (size_t offset = 0; offset < data.size(); offset += 100000) {
                    auto piece = std::string_view(data).substr(offset, 100000);
                    auto [ec, bytes] = co_await socket->write(piece);
                    if (ec) co_return;
                }
            };
            auto reader = [&]() -> awaitable<std::string> {
                std::string received(data.size(), '\0');
                auto [ec, bytes] = co_await socket->read(reinterpret_cast<uint8_t*>(received.data()), received.size());
                received.resize(bytes);
                co_return received;
            };
            auto received = co_await (writer() && reader());
            socket->close();
            result.set(std::move(received));
        });
        auto received = result.wait(30s);
        REQUIRE(received);
        REQUIRE(received->size() == data.size());
        REQUIRE(*received == data);
        client.wait();
    }

    SECTION("In a pipe on the client side") {
        // client WebSocket <-> local TCP connection, as a proxy to a WebSocket target does
        http::async_client client;
        result_slot<uint16_t> local_port;
        client.run([&]() -> awaitable<void> {
            auto ws = co_await client.websocket(url, "binary");
            if (!ws) co_return;
            auto websocket = ws->release_socket();
            websocket->set_binary(true);
            // the other socket of the pipe on the io_context of the WebSocket
            auto& io_context = websocket->get_io_context();
            tcp::acceptor local(io_context, {tcp::v4(), 0});
            local_port.set(local.local_endpoint().port());
            auto [ec, accepted] = co_await local.async_accept(use_nothrow_awaitable);
            if (ec) co_return;
            auto local_socket = std::make_shared<asio::tcp_socket>("local", std::move(accepted));
            auto pipe = std::make_shared<asio::socket_pipe>(local_socket, websocket);
            co_await pipe->run();
        });

        auto port = local_port.wait();
        REQUIRE(port);
        boost::asio::io_context ioc;
        tcp::socket app(ioc);
        app.connect({boost::asio::ip::make_address("127.0.0.1"), *port});
        auto data = pattern(512 * 1024, 9);
        for (size_t offset = 0; offset < data.size(); offset += 32768) {
            auto piece = data.substr(offset, 32768);
            boost::asio::write(app, boost::asio::buffer(piece));
            std::string echoed(piece.size(), '\0');
            boost::asio::read(app, boost::asio::buffer(echoed));
            REQUIRE(echoed == piece);
        }
        // closing the local end closes the tunnel
        app.shutdown(tcp::socket::shutdown_both);
        app.close();
        REQUIRE(client.wait_for(5s));
    }
}

// ============================================================================
// Regressions: closing without an io_context, interrupted writes, control frames
// ============================================================================

namespace {

    constexpr size_t STUCK_SIZE = 32 * 1024 * 1024;   // more than the socket buffers hold

    // Wait on the io_context of the socket
    awaitable<void> sleep_for(std::shared_ptr<asio::socket> socket, std::chrono::milliseconds duration) {
        boost::asio::steady_timer timer(socket->get_io_context(), duration);
        co_await timer.async_wait(use_nothrow_awaitable);
    }

    std::vector<ws_frame> read_frames(raw_ws_client& client, size_t count) {
        std::vector<ws_frame> frames;
        while (frames.size() < count) {
            auto frame = client.read_frame(10s);
            if (!frame) break;
            frames.push_back(std::move(*frame));
        }
        return frames;
    }

}

TEST_CASE("WebSocket stream: a synchronous client closes without running its io_context", "[websocket][stream][client][integration]") {
    http::server server;
    result_slot<boost::system::error_code> ended;
    server.get("/echo", [&](http::request& req, http::response& res) {
        res.upgrade_websocket_stream([&](std::shared_ptr<asio::websocket> ws, std::string) {
            co_spawn(ws->get_io_context(), [ws, &ended]() -> awaitable<void> {
                ended.set(co_await length_prefixed_echo(ws));
            }, detached);
        });
    });
    server.get("/ok", [](http::response& res) { res.send("ok"); });
    running_server running(server);
    auto base = "127.0.0.1:" + std::to_string(running.port);

    http::client client;
    auto ws = client.websocket("ws://" + base + "/echo");
    REQUIRE(ws);
    // released without close(): closed now, leaving nothing for the next call of the client
    ws.reset();
    REQUIRE(ended.wait(2s));

    auto start = std::chrono::steady_clock::now();
    auto response = client.get("http://" + base + "/ok");
    REQUIRE(response.ok());
    REQUIRE(std::chrono::steady_clock::now() - start < 1s);
}

TEST_CASE("WebSocket stream: a websocket released on another thread starts no I/O", "[websocket][stream][integration]") {
    boost::asio::io_context ioc;
    auto work = boost::asio::make_work_guard(ioc);
    tcp::acceptor acceptor(ioc, {tcp::v4(), 0});
    tcp::socket peer(ioc);
    peer.connect({boost::asio::ip::make_address("127.0.0.1"), acceptor.local_endpoint().port()});
    auto accepted = acceptor.accept();
    std::thread io_thread([&ioc] { ioc.run(); });

    // created and used on the io_context (keepalive running), released here
    std::promise<std::shared_ptr<asio::websocket>> created;
    boost::asio::post(ioc, [&] {
        auto tcp_end = std::make_shared<asio::tcp_socket>("ws", std::move(accepted));
        auto ws = std::make_shared<asio::websocket>(tcp_end, true, true);
        ws->set_keepalive(std::chrono::hours(1), std::chrono::hours(1));   // waiting on its timer
        created.set_value(ws);
    });
    auto ws = created.get_future().get();
    std::this_thread::sleep_for(50ms);
    std::weak_ptr<asio::websocket> weak = ws;
    ws.reset();
    // a ping being sent may hold it for a moment
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!weak.expired() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    REQUIRE(weak.expired());
    std::this_thread::sleep_for(50ms);

    std::promise<void> cleaned;
    boost::asio::post(ioc, [&] {
        boost::system::error_code ignored;
        peer.close(ignored);
        acceptor.close(ignored);
        cleaned.set_value();
    });
    cleaned.get_future().wait();
    work.reset();
    ioc.stop();   // its keepalive coroutine ends at its next tick, an hour away
    io_thread.join();
}

TEST_CASE("WebSocket stream: interrupted writes keep the framing", "[websocket][stream][integration]") {
    http::server server;
    result_slot<boost::system::error_code> first_write;
    result_slot<std::string> read_result;
    std::string big = pattern(STUCK_SIZE, 11);

    server.get("/cancel", [&](http::request& req, http::response& res) {
        res.upgrade_websocket_stream([&](std::shared_ptr<asio::websocket> ws, std::string) {
            co_spawn(ws->get_io_context(), [ws, &first_write, &big]() -> awaitable<void> {
                co_spawn(ws->get_io_context(), [ws]() -> awaitable<void> {
                    co_await sleep_for(ws, 100ms);
                    ws->cancel();   // in the middle of the write: the client is not reading
                }, detached);
                auto [ec, bytes] = co_await ws->write(std::string_view(big));
                first_write.set(ec);
                co_await ws->write(std::string_view("after"));
            }, detached);
        });
    });
    server.get("/close", [&](http::request& req, http::response& res) {
        res.upgrade_websocket_stream([&](std::shared_ptr<asio::websocket> ws, std::string) {
            co_spawn(ws->get_io_context(), [ws, &first_write, &big]() -> awaitable<void> {
                co_spawn(ws->get_io_context(), [ws]() -> awaitable<void> {
                    co_await sleep_for(ws, 100ms);
                    ws->close();
                }, detached);
                auto [ec, bytes] = co_await ws->write(std::string_view(big));
                first_write.set(ec);
            }, detached);
        });
    });
    server.get("/pong", [&](http::request& req, http::response& res) {
        res.upgrade_websocket_stream([&](std::shared_ptr<asio::websocket> ws, std::string) {
            co_spawn(ws->get_io_context(), [ws, &big]() -> awaitable<void> {
                co_await ws->write(std::string_view(big));
            }, detached);
            co_spawn(ws->get_io_context(), [ws, &read_result]() -> awaitable<void> {
                uint8_t data[4];
                auto [ec, bytes] = co_await ws->read(data, sizeof(data));
                read_result.set(ec ? ec.message() : std::string(reinterpret_cast<char*>(data), bytes));
            }, detached);
        });
    });
    server.get("/pings", [&](http::request& req, http::response& res) {
        res.upgrade_websocket_stream({.ping_interval = 300ms}, [&](std::shared_ptr<asio::websocket> ws, std::string) {
            co_spawn(ws->get_io_context(), [ws, &big]() -> awaitable<void> {
                co_await ws->write(std::string_view(big));
            }, detached);
            co_spawn(ws->get_io_context(), length_prefixed_echo(ws), detached);
        });
    });
    running_server running(server);

    SECTION("cancel() in the middle of a frame: the next write completes it first") {
        raw_ws_client client(running.port);
        REQUIRE(client.handshake("/cancel").starts_with("HTTP/1.1 101"));
        auto ec = first_write.wait();
        REQUIRE(ec);
        REQUIRE(*ec == boost::asio::error::operation_aborted);
        auto frames = read_frames(client, 2);
        REQUIRE(frames.size() == 2);
        REQUIRE(frames[0].opcode == OP_BINARY);
        REQUIRE(frames[0].payload == big);
        REQUIRE(frames[1].opcode == OP_BINARY);
        REQUIRE(frames[1].payload == "after");
    }

    SECTION("close() in the middle of a frame: completed, then the close frame") {
        raw_ws_client client(running.port);
        REQUIRE(client.handshake("/close").starts_with("HTTP/1.1 101"));
        REQUIRE(first_write.wait());
        auto frames = read_frames(client, 2);
        REQUIRE(frames.size() == 2);
        REQUIRE(frames[0].payload == big);
        REQUIRE(frames[1].opcode == OP_CLOSE);
        REQUIRE(frames[1].close_code() == 1000);
        client.send_frame(OP_CLOSE, close_payload(1000));
        REQUIRE(client.closed_within(2s));
    }

    SECTION("A ping does not hold the reads while a write is in progress") {
        raw_ws_client client(running.port);
        REQUIRE(client.handshake("/pong").starts_with("HTTP/1.1 101"));
        std::this_thread::sleep_for(100ms);
        client.send_frame(OP_PING, "p");
        client.send_frame(OP_BINARY, "data");
        // read while the write is still stuck: the client has not read anything yet
        REQUIRE(read_result.wait(3s) == "data");
        auto frames = read_frames(client, 2);
        REQUIRE(frames.size() == 2);
        REQUIRE(frames[0].payload == big);
        REQUIRE(frames[1].opcode == OP_PONG);
        REQUIRE(frames[1].payload == "p");
    }

    SECTION("Keepalive pings do not pile up behind a write in progress") {
        raw_ws_client client(running.port);
        REQUIRE(client.handshake("/pings").starts_with("HTTP/1.1 101"));
        std::this_thread::sleep_for(4500ms);   // 15 ping intervals
        auto start = std::chrono::steady_clock::now();
        auto frames = read_frames(client, 1);
        REQUIRE(frames.size() == 1);
        REQUIRE(frames[0].payload == big);
        int pings = 0;
        while (auto frame = client.read_frame(100ms)) {
            REQUIRE(frame->opcode == OP_PING);
            pings++;
        }
        REQUIRE(pings >= 1);
        // the one queued, and those of the time the client took to read the message
        auto intervals = (std::chrono::steady_clock::now() - start) / 300ms;
        REQUIRE(pings <= 2 + intervals);
    }
}

TEST_CASE("WebSocket stream: handshake checks", "[websocket][stream][integration]") {
    http::server server;
    auto handler = [](http::request& req, http::response& res) {
        res.upgrade_websocket_stream([](std::shared_ptr<asio::websocket> ws, std::string) {});
    };
    server.get("/ws", handler);
    server.post("/ws", handler);
    running_server running(server);

    auto exchange = [&](const std::string& method, const std::string& headers) {
        return server_test::raw_exchange(running.port, method + " /ws HTTP/1.1\r\nHost: localhost\r\n" + headers +
                                                       "Content-Length: 0\r\n\r\n", 300ms).data;
    };
    const std::string upgrade = "Upgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n";
    const std::string key = "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
    const std::string version = "Sec-WebSocket-Version: 13\r\n";

    REQUIRE(exchange("GET", upgrade + key + version).starts_with("HTTP/1.1 101"));
    REQUIRE(exchange("POST", upgrade + key + version).starts_with("HTTP/1.1 400"));
    REQUIRE(exchange("GET", "Upgrade: websocket\r\nConnection: keep-alive\r\n" + key + version).starts_with("HTTP/1.1 400"));
    auto old_version = exchange("GET", upgrade + key + "Sec-WebSocket-Version: 8\r\n");
    REQUIRE(old_version.starts_with("HTTP/1.1 426"));
    REQUIRE(header_value(old_version, "Sec-WebSocket-Version") == "13");
    REQUIRE(exchange("GET", upgrade + key).starts_with("HTTP/1.1 426"));
    REQUIRE(exchange("GET", upgrade + "Sec-WebSocket-Key: c2hvcnQ=\r\n" + version).starts_with("HTTP/1.1 400"));
    REQUIRE(exchange("GET", upgrade + "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ!!\r\n" + version).starts_with("HTTP/1.1 400"));
    REQUIRE(exchange("GET", upgrade + version).starts_with("HTTP/1.1 400"));
}

TEST_CASE("WebSocket client rejects a subprotocol it did not offer", "[websocket][client][integration]") {
    boost::asio::io_context server_context;
    tcp::acceptor acceptor(server_context, {boost::asio::ip::make_address("127.0.0.1"), 0});
    auto port = acceptor.local_endpoint().port();

    std::thread server_thread([&acceptor, &server_context]() {
        tcp::socket socket(server_context);
        acceptor.accept(socket);
        boost::asio::streambuf request;
        boost::asio::read_until(socket, request, "\r\n\r\n");
        std::string text(boost::asio::buffers_begin(request.data()), boost::asio::buffers_end(request.data()));
        const std::string key_header = "Sec-WebSocket-Key: ";
        auto key_start = text.find(key_header) + key_header.size();
        auto key = text.substr(key_start, text.find("\r\n", key_start) - key_start);
        auto accept = util::base64::encode(util::sha1::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
        std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                               "Sec-WebSocket-Protocol: other\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
        boost::asio::write(socket, boost::asio::buffer(response));
        uint8_t data[64];
        boost::system::error_code ec;
        socket.read_some(boost::asio::buffer(data), ec);
    });

    {
        http::client client;
        auto ws = client.websocket("ws://127.0.0.1:" + std::to_string(port) + "/", "mqtt, mqttv3.1");
        REQUIRE_FALSE(ws);
    }
    server_thread.join();
}

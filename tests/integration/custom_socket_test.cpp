#include <catch2/catch_test_macros.hpp>
#include <thinger/asio/socket_pipe.hpp>
#include <thinger/asio/sockets/websocket.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>

#include "server_test_helpers.hpp"
#include "../fixtures/stream_test_helpers.hpp"

#include <future>

// Sockets defined by the application (asio::socket implementing only the required methods):
// the defaults of the base class, and tunnels between them and a WebSocket

using namespace thinger;
using namespace std::chrono_literals;
using namespace stream_test;
using server_test::running_server;

namespace {

    // The other end of a virtual stream, on its own thread (as the connection a virtual stream
    // runs inside): echoes what it receives, and ends the stream on "bye"
    struct remote_end {
        boost::asio::io_context ioc;
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{ioc.get_executor()};
        std::thread thread{[this] { ioc.run(); }};
        std::atomic<bool> closed{false};
        std::atomic<size_t> received{0};

        ~remote_end() {
            work.reset();
            ioc.stop();
            thread.join();
        }

        // A virtual stream on `io_context` whose other end is this one
        std::shared_ptr<memory_socket> open(boost::asio::io_context& io_context) {
            auto self = std::make_shared<std::weak_ptr<memory_socket>>();
            auto socket = std::make_shared<memory_socket>(io_context,
                [this, self](std::string data) {
                    // called on the io_context of the socket: handed over to this thread
                    boost::asio::post(ioc, [this, self, data = std::move(data)]() mutable {
                        auto socket = self->lock();
                        if (!socket) return;
                        received += data.size();
                        if (data == "bye") socket->push_eof();
                        else socket->push(std::move(data));
                    });
                },
                [this] { closed = true; });
            *self = socket;
            return socket;
        }
    };

}

TEST_CASE("Custom socket: reads and writes built on read_some() and write()", "[socket][custom][integration]") {
    boost::asio::io_context ioc;
    std::string written;
    auto socket = std::make_shared<memory_socket>(ioc, [&](std::string data) { written += data; });

    // data arriving from another thread, in pieces
    std::thread producer([socket] {
        for (auto piece : {"GET / HT", "TP/1.1\r\nHost: x\r", "\n\r\n", "0123", "456789", "tail"}) {
            socket->push(piece);
            std::this_thread::sleep_for(5ms);
        }
        socket->push_eof();
    });

    auto result = co_spawn(ioc, [socket]() -> awaitable<std::vector<std::string>> {
        std::vector<std::string> results;
        boost::asio::streambuf buffer;
        auto [ec, bytes] = co_await socket->read_until(buffer, "\r\n\r\n");
        results.push_back(ec ? ec.message() : std::string(static_cast<const char*>(buffer.data().data()), bytes));
        buffer.consume(bytes);

        uint8_t exact[4];
        auto [read_ec, read_bytes] = co_await socket->read(exact, sizeof(exact));
        results.push_back(read_ec ? read_ec.message() : std::string(reinterpret_cast<char*>(exact), read_bytes));

        auto [buffer_ec, buffer_bytes] = co_await socket->read(buffer, 6);
        results.push_back(buffer_ec ? buffer_ec.message() : std::string(static_cast<const char*>(buffer.data().data()), buffer.size()));
        buffer.consume(buffer.size());

        // the end of the stream ends an exact read too, with what was read
        uint8_t rest[16];
        auto [end_ec, end_bytes] = co_await socket->read(rest, sizeof(rest));
        results.push_back(std::string(reinterpret_cast<char*>(rest), end_bytes) + "|" +
                          (end_ec == boost::asio::error::eof ? "eof" : end_ec.message()));

        co_await socket->write(std::string_view("one,"));
        co_await socket->write(std::vector<boost::asio::const_buffer>{boost::asio::buffer("two,", 4), boost::asio::buffer("three", 5)});
        co_return results;
    }, boost::asio::use_future);

    ioc.run();
    producer.join();
    auto results = result.get();
    REQUIRE(results.size() == 4);
    REQUIRE(results[0] == "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(results[1] == "0123");
    REQUIRE(results[2] == "456789");
    REQUIRE(results[3] == "tail|eof");
    REQUIRE(written == "one,two,three");

    // the defaults of the methods it does not implement
    REQUIRE_FALSE(socket->is_secure());
    REQUIRE(socket->get_remote_ip().empty());
    REQUIRE(socket->available() == 0);
}

TEST_CASE("Custom socket: cancel() and close() end the pending read", "[socket][custom][integration]") {
    boost::asio::io_context ioc;
    auto socket = std::make_shared<memory_socket>(ioc, nullptr);

    auto result = co_spawn(ioc, [socket]() -> awaitable<std::vector<boost::system::error_code>> {
        std::vector<boost::system::error_code> errors;
        uint8_t buffer[16];
        auto executor = co_await boost::asio::this_coro::executor;

        // cancel(): aborted, the socket stays usable
        boost::asio::steady_timer timer(executor, 20ms);
        timer.async_wait([socket](auto) { socket->cancel(); });
        auto [cancel_ec, cancel_bytes] = co_await socket->read_some(buffer, sizeof(buffer));
        errors.push_back(cancel_ec);

        socket->push("data");
        auto [data_ec, data_bytes] = co_await socket->read_some(buffer, sizeof(buffer));
        errors.push_back(data_ec);

        // per-operation cancellation, as the awaitable operators do
        boost::asio::steady_timer race(executor, 20ms);
        auto outcome = co_await (socket->read_some(buffer, sizeof(buffer)) || race.async_wait(use_nothrow_awaitable));
        errors.push_back(outcome.index() == 1 ? boost::system::error_code{} : boost::asio::error::fault);

        // close(): ends it, and the socket is closed
        boost::asio::steady_timer closer(executor, 20ms);
        closer.async_wait([socket](auto) { socket->close(); });
        auto [close_ec, close_bytes] = co_await socket->read_some(buffer, sizeof(buffer));
        errors.push_back(close_ec);
        co_return errors;
    }, boost::asio::use_future);

    ioc.run();
    auto errors = result.get();
    REQUIRE(errors.size() == 4);
    REQUIRE(errors[0] == boost::asio::error::operation_aborted);
    REQUIRE_FALSE(errors[1]);
    REQUIRE_FALSE(errors[2]);
    REQUIRE(errors[3]);
    REQUIRE_FALSE(socket->is_open());
}

TEST_CASE("Custom socket: tunnel with a WebSocket", "[socket][custom][websocket][socket-pipe][integration]") {
    remote_end remote;
    http::server server;
    std::atomic<bool> pipe_ended{false};
    server.get("/virtual", [&](http::request& req, http::response& res) {
        res.upgrade_websocket_stream({.protocols = {"binary"}}, [&](std::shared_ptr<asio::websocket> ws, std::string) {
            // the virtual stream lives on the io_context of the WebSocket
            auto stream = remote.open(ws->get_io_context());
            auto pipe = std::make_shared<asio::socket_pipe>(ws, stream);
            pipe->set_on_end([&pipe_ended] { pipe_ended = true; });
            pipe->start();
        });
    });
    running_server running(server);

    raw_ws_client client(running.port);
    REQUIRE(client.handshake("/virtual", "Sec-WebSocket-Protocol: binary\r\n").starts_with("HTTP/1.1 101"));

    SECTION("Large data both ways, closed by the WebSocket end") {
        auto data = pattern(1024 * 1024, 5);
        constexpr size_t chunk = 32 * 1024;
        for (size_t offset = 0; offset < data.size(); offset += chunk) {
            auto piece = std::string_view(data).substr(offset, chunk);
            client.send_frame(OP_BINARY, piece.substr(0, 100), false);
            client.send_frame(OP_CONTINUATION, piece.substr(100), true);
            REQUIRE(client.read_data(piece.size()) == piece);
        }
        REQUIRE(remote.received == data.size());

        client.send_frame(OP_CLOSE, close_payload(1000));
        auto close = client.read_frame();
        REQUIRE(close);
        REQUIRE(close->opcode == OP_CLOSE);
        REQUIRE(client.closed_within(2s));
        REQUIRE(wait_for(pipe_ended));
        REQUIRE(wait_for(remote.closed));
    }

    SECTION("Closed by the virtual stream end") {
        client.send_frame(OP_BINARY, "hello");
        REQUIRE(client.read_data(5) == "hello");
        client.send_frame(OP_BINARY, "bye");
        auto close = client.read_frame();
        REQUIRE(close);
        REQUIRE(close->opcode == OP_CLOSE);
        REQUIRE(close->close_code() == 1000);
        client.send_frame(OP_CLOSE, close_payload(1000));
        REQUIRE(client.closed_within(2s));
        REQUIRE(wait_for(pipe_ended));
    }
}

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/sse_connection.hpp>
#include <thinger/http/server/websocket_connection.hpp>
#include <boost/asio/use_future.hpp>

#include "server_test_helpers.hpp"
#include "../fixtures/stream_test_helpers.hpp"

#include <atomic>
#include <future>
#include <thread>

// The flow control of websocket_connection and sse_connection is the same queue: with the same
// write limits, both cross the same watermarks, told once each, and a producer waiting for the
// queue to drain resumes at the same point

using namespace thinger;
using namespace std::chrono_literals;
using server_test::running_server;

namespace {

    template<typename F>
    bool eventually(F condition, std::chrono::milliseconds timeout = 10s) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!condition()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    // A WebSocket pushing binary messages, and its raw client
    struct websocket_push {
        using connection = http::websocket_connection;

        static void route(http::server& server, std::function<void(std::shared_ptr<connection>)> setup) {
            server.get("/push", [setup](http::request&, http::response& res) {
                res.upgrade_websocket({}, [setup](std::shared_ptr<connection> ws) { setup(std::move(ws)); });
            });
        }

        static void send(connection& ws, std::string data) { ws.send_binary(std::move(data)); }

        struct client {
            stream_test::raw_ws_client ws;

            explicit client(const http::test::transport_endpoint& to) : ws(to) {
                REQUIRE(ws.handshake("/push").starts_with("HTTP/1.1 101"));
            }

            // Whether `count` messages of `size` bytes arrive
            bool read(size_t count, size_t size) {
                for (size_t i = 0; i < count;) {
                    auto frame = ws.read_frame(10s);
                    if (!frame) return false;
                    if (frame->opcode == stream_test::OP_PING || frame->opcode == stream_test::OP_PONG) continue;
                    if (frame->opcode != stream_test::OP_BINARY || frame->payload.size() != size) return false;
                    ++i;
                }
                return true;
            }
        };
    };

    // An SSE stream pushing events, and its raw client
    struct sse_push {
        using connection = http::sse_connection;

        static void route(http::server& server, std::function<void(std::shared_ptr<connection>)> setup) {
            server.get("/push", [setup](http::request&, http::response& res) {
                res.start_sse([setup](std::shared_ptr<connection> sse) { setup(std::move(sse)); });
            });
        }

        static void send(connection& sse, std::string data) { sse.send(http::sse_event{.data = std::move(data)}); }

        struct client {
            http::test::raw_connection connection;
            std::string stream;

            explicit client(const http::test::transport_endpoint& to) : connection(to) {
                REQUIRE(connection.write("GET /push HTTP/1.1\r\nHost: localhost\r\nAccept: text/event-stream\r\n\r\n"));
                REQUIRE(connection.read_until(stream, "\r\n\r\n", 5s));
                stream.erase(0, stream.find("\r\n\r\n") + 4);
            }

            // Whether `count` events of `size` bytes of data ("data: " + data + "\n\n") arrive
            bool read(size_t count, size_t size) {
                auto total = count * (size + 8);
                auto deadline = std::chrono::steady_clock::now() + 10s;
                while (stream.size() < total && !connection.eof()) {
                    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                    if (connection.read_some(stream, left) == 0 && !connection.eof()) break;
                }
                return stream.size() == total;
            }
        };
    };

    // What the watermark callbacks told
    template<typename Connection>
    struct watermarks {
        std::atomic<int> highs{0};
        std::atomic<int> lows{0};
        std::atomic<size_t> queued_at_high{0};
        std::atomic<size_t> queued_at_low{0};
        std::promise<std::shared_ptr<Connection>> created;
    };

}

TEMPLATE_TEST_CASE("WebSocket and SSE cross the same watermarks", "[websocket][sse][flow][integration][matrix]",
                   websocket_push, sse_push) {
    MATRIX_TRANSPORT(kind);
    // more than the socket buffers take, with the client not reading
    constexpr size_t count = 400;
    constexpr size_t size = 64 * 1024;
    const http::write_limits limits{.max_messages = 0, .high_watermark = 1 << 20, .low_watermark = 256 << 10};

    using connection = typename TestType::connection;
    auto told = std::make_shared<watermarks<connection>>();
    http::server server;
    TestType::route(server, [told, limits](std::shared_ptr<connection> pushing) {
        pushing->set_write_limits(limits);
        std::weak_ptr<connection> weak = pushing;
        pushing->on_high_watermark([told, weak] {
            if (auto self = weak.lock()) told->queued_at_high = self->queued_bytes();
            ++told->highs;
        });
        pushing->on_low_watermark([told, weak] {
            if (auto self = weak.lock()) told->queued_at_low = self->queued_bytes();
            ++told->lows;
        });
        for (size_t i = 0; i < count; ++i) TestType::send(*pushing, std::string(size, 'x'));
        told->created.set_value(pushing);
    });
    running_server running(server, kind);

    typename TestType::client client(running.endpoint());
    auto pushing = told->created.get_future().get();
    // queued at once: high told once, afterwards
    REQUIRE(eventually([&] { return told->highs == 1; }));
    CHECK(told->queued_at_high > limits.high_watermark);

    // a producer on another thread waits while the client does not read
    boost::asio::io_context other;
    auto writable = co_spawn(other, pushing->wait_writable(), boost::asio::use_future);
    std::thread producer([&] { other.run_for(30s); });
    struct joined {
        boost::asio::io_context& context;
        std::thread& thread;
        ~joined() {
            context.stop();
            thread.join();
        }
    } join_producer{other, producer};
    CHECK(writable.wait_for(300ms) == std::future_status::timeout);
    CHECK(told->lows == 0);

    // the client reads everything: the producer resumes, and low is told once, at or below it
    CHECK(client.read(count, size));
    REQUIRE(writable.wait_for(10s) == std::future_status::ready);
    CHECK(writable.get());
    REQUIRE(eventually([&] { return told->lows == 1; }));
    CHECK(told->queued_at_low <= limits.low_watermark);
    CHECK(told->highs == 1);
    CHECK(eventually([&] { return pushing->queued_bytes() == 0; }));
}

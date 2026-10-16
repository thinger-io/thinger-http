#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>

#include "server_test_helpers.hpp"
#include "../fixtures/stream_test_helpers.hpp"

#include <future>
#include <mutex>
#include <vector>

// Flow control of the server websocket_connection: sends awaited until written, queued
// bytes and watermarks, pausing the reads, and the overflow policies

using namespace thinger;
using namespace std::chrono_literals;
using namespace stream_test;
using server_test::running_server;

namespace {

    using ws_ptr = std::shared_ptr<http::websocket_connection>;

    // Wait until `condition` holds, up to `timeout`
    template<typename F>
    bool eventually(F condition, std::chrono::milliseconds timeout = 10s) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!condition()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    // Run `f` on the thread of the websocket and return its result
    template<typename F>
    auto on_ws_thread(const ws_ptr& ws, F f) {
        std::promise<decltype(f())> result;
        ws->execute([&] { result.set_value(f()); });
        return result.get_future().get();
    }

    std::string indexed(uint32_t index, size_t size) {
        std::string data(size, char('a' + index % 26));
        data[0] = char(index >> 24);
        data[1] = char((index >> 16) & 0xFF);
        data[2] = char((index >> 8) & 0xFF);
        data[3] = char(index & 0xFF);
        return data;
    }

    uint32_t index_of(const std::string& data) {
        return (uint32_t(uint8_t(data[0])) << 24) | (uint32_t(uint8_t(data[1])) << 16) |
               (uint32_t(uint8_t(data[2])) << 8) | uint32_t(uint8_t(data[3]));
    }

    // Indexes of the data frames received until a close frame, `limit` frames, or the timeout
    std::vector<uint32_t> read_indexes(raw_ws_client& client, std::chrono::milliseconds timeout = 1s,
                                       size_t limit = SIZE_MAX) {
        std::vector<uint32_t> indexes;
        while (indexes.size() < limit) {
            auto frame = client.read_frame(timeout);
            if (!frame || frame->opcode == OP_CLOSE) break;
            indexes.push_back(index_of(frame->payload));
        }
        return indexes;
    }

    std::vector<uint32_t> range(uint32_t from, uint32_t to) {
        std::vector<uint32_t> values;
        for (auto i = from; i < to; ++i) values.push_back(i);
        return values;
    }

    // A producer awaiting each message, while sampling the bytes queued
    struct producer_state {
        std::atomic<uint32_t> sent{0};
        std::atomic<size_t> max_queued{0};
        std::atomic<bool> failed{false};
        std::atomic<bool> done{false};
        // weak: the state may outlive the server, the connection may not
        std::promise<std::weak_ptr<http::websocket_connection>> created;

        void sample(size_t queued) {
            if (queued > max_queued) max_queued = queued;
        }
    };

    // The producer is stalled: what it sent and the server queue (read from this thread:
    // queued_bytes() is atomic) unchanged over `samples` polls in a row, with something queued
    // up to `bound`
    bool stalled(const producer_state& state, const ws_ptr& ws, size_t bound, int samples = 20) {
        uint32_t last_sent = UINT32_MAX;
        size_t last_queued = SIZE_MAX;
        int same = 0;
        return eventually([&] {
            auto sent = state.sent.load();
            auto queued = ws->queued_bytes();
            bool stuck = sent == last_sent && queued == last_queued && queued > 0 && queued <= bound;
            same = stuck ? same + 1 : 0;
            last_sent = sent;
            last_queued = queued;
            return same >= samples;
        });
    }

    awaitable<void> awaited_producer(ws_ptr ws, std::shared_ptr<producer_state> state, uint32_t count, size_t size) {
        for (uint32_t i = 0; i < count; ++i) {
            if (!co_await ws->send_binary_async(indexed(i, size))) state->failed = true;
            state->sent = i + 1;
        }
        state->done = true;
    }

    awaitable<void> watermark_producer(ws_ptr ws, std::shared_ptr<producer_state> state, uint32_t count, size_t size) {
        for (uint32_t i = 0; i < count; ++i) {
            ws->send_binary(indexed(i, size));
            state->sample(ws->queued_bytes());
            if (!co_await ws->wait_writable()) state->failed = true;
            state->sent = i + 1;
        }
        state->done = true;
    }

    // Samples the bytes queued every millisecond, on the thread of the websocket
    awaitable<void> queue_sampler(ws_ptr ws, std::shared_ptr<producer_state> state) {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
        while (!state->done) {
            state->sample(ws->queued_bytes());
            timer.expires_after(1ms);
            co_await timer.async_wait(use_nothrow_awaitable);
        }
    }

    // Start `producer` and a sampler of the queue on the thread of the websocket
    awaitable<void> start_producer(awaitable<void> producer, ws_ptr ws, std::shared_ptr<producer_state> state) {
        state->created.set_value(ws);
        auto executor = co_await boost::asio::this_coro::executor;
        co_spawn(executor, queue_sampler(ws, state), detached);
        co_spawn(executor, std::move(producer), detached);
    }

    // Raw client writing without blocking: tells when the server stops taking data
    class nonblocking_writer {
    public:
        explicit nonblocking_writer(uint16_t port) : sock_(ioc_) {
            sock_.connect({net::ip::make_address("127.0.0.1"), port});
            std::string request = "GET /pause HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
            net::write(sock_, net::buffer(request));
            net::streambuf response;
            net::read_until(sock_, response, "\r\n\r\n");
            sock_.non_blocking(true);
        }

        // Write messages until the socket takes nothing for `stall`: whether it blocked before
        // `limit` bytes
        bool write_until_blocked(size_t message_size, std::chrono::milliseconds stall, size_t limit) {
            auto stalled_since = std::chrono::steady_clock::now();
            while (written_ < limit) {
                if (pending_.empty()) pending_ = make_frame(OP_BINARY, indexed(messages_++, message_size));
                boost::system::error_code ec;
                auto bytes = sock_.write_some(net::buffer(pending_), ec);
                if (ec == net::error::would_block) {
                    if (std::chrono::steady_clock::now() - stalled_since >= stall) return true;
                    std::this_thread::sleep_for(5ms);
                    continue;
                }
                if (ec) return false;
                pending_.erase(0, bytes);
                written_ += bytes;
                stalled_since = std::chrono::steady_clock::now();
            }
            return false;
        }

        // Finish the message being written, and write `count` more, blocking
        void finish(size_t message_size, uint32_t count) {
            sock_.non_blocking(false);
            if (!pending_.empty()) net::write(sock_, net::buffer(pending_));
            pending_.clear();
            for (uint32_t i = 0; i < count; ++i) {
                net::write(sock_, net::buffer(make_frame(OP_BINARY, indexed(messages_++, message_size))));
            }
        }

        uint32_t messages() const { return messages_; }
        size_t written() const { return written_; }

    private:
        net::io_context ioc_;
        tcp::socket sock_;
        std::string pending_;
        uint32_t messages_ = 0;
        size_t written_ = 0;
    };

}

TEST_CASE("WebSocket awaitable send with a slow reader", "[websocket][flow][integration]") {
    constexpr uint32_t count = 512;
    constexpr size_t size = 64 * 1024;

    http::server server;
    auto state = std::make_shared<producer_state>();
    server.get("/push", [state](http::request&, http::response& res) {
        res.upgrade_websocket([state](ws_ptr ws) -> awaitable<void> {
            co_await start_producer(awaited_producer(ws, state, count, size), ws, state);
        });
    });
    running_server running(server);

    raw_ws_client client(running.port);
    REQUIRE(client.handshake("/push").starts_with("HTTP/1.1 101"));

    // The client does not read: the producer is suspended once the socket buffers fill, with
    // one message queued (the one being written)
    auto ws = state->created.get_future().get().lock();
    REQUIRE(stalled(*state, ws, size));
    REQUIRE(state->sent.load() < count);
    REQUIRE(state->max_queued.load() == size);

    // It resumes as the client reads: everything arrives, in order
    std::vector<uint32_t> received;
    while (received.size() < count) {
        auto frame = client.read_frame(10s);
        REQUIRE(frame);
        REQUIRE(frame->opcode == OP_BINARY);
        REQUIRE(frame->payload.size() == size);
        received.push_back(index_of(frame->payload));
    }
    REQUIRE(received == range(0, count));
    REQUIRE(eventually([&] { return state->done.load(); }));
    REQUIRE_FALSE(state->failed.load());
    REQUIRE(state->max_queued.load() <= size);
}

TEST_CASE("WebSocket watermarks and queued bytes", "[websocket][flow][integration]") {
    constexpr uint32_t count = 400;
    constexpr size_t size = 64 * 1024;

    http::server server;
    std::mutex mutex;
    std::vector<std::string> events;
    std::promise<ws_ptr> created;
    server.get("/flow", [&](http::request&, http::response& res) {
        res.upgrade_websocket([&](ws_ptr ws) {
            ws->set_write_limits({.max_messages = 0, .high_watermark = 1024 * 1024, .low_watermark = 256 * 1024});
            ws->on_high_watermark([&, ws] {
                std::lock_guard lock(mutex);
                events.push_back("high " + std::to_string(ws->queued_bytes()));
            });
            ws->on_low_watermark([&, ws] {
                std::lock_guard lock(mutex);
                events.push_back("low " + std::to_string(ws->queued_bytes()));
            });
            // queued at once: the first one is being written, still counted
            for (uint32_t i = 0; i < count; ++i) ws->send_binary(indexed(i, size));
            {
                std::lock_guard lock(mutex);
                events.push_back("queued " + std::to_string(ws->queued_bytes()) + " " +
                                 std::to_string(ws->queued_messages()));
            }
            created.set_value(ws);
        });
    });
    running_server running(server);

    raw_ws_client client(running.port);
    REQUIRE(client.handshake("/flow").starts_with("HTTP/1.1 101"));
    auto ws = created.get_future().get();
    {
        std::lock_guard lock(mutex);
        // everything queued at once; the high watermark is told afterwards, not from a send
        REQUIRE(events.front() == "queued " + std::to_string(count * size) + " " + std::to_string(count));
    }

    // The client does not read: what is not written yet is accounted for exactly
    auto [bytes, messages] = on_ws_thread(ws, [&] { return std::pair{ws->queued_bytes(), ws->queued_messages()}; });
    REQUIRE(messages > 0);
    REQUIRE(bytes == messages * size);
    REQUIRE(on_ws_thread(ws, [&] { return ws->queue_full(); }) == false);   // no message limit

    auto received = read_indexes(client, 5s, count);
    REQUIRE(received == range(0, count));
    REQUIRE(eventually([&] { return on_ws_thread(ws, [&] { return ws->queued_bytes(); }) == 0; }));
    REQUIRE(on_ws_thread(ws, [&] { return ws->queued_messages(); }) == 0);
    REQUIRE(eventually([&] {
        std::lock_guard lock(mutex);
        return events.size() == 3;
    }));
    std::lock_guard lock(mutex);
    // high once, then low once: drained at or below the low watermark
    REQUIRE(events[1].starts_with("high "));
    REQUIRE(events[2].starts_with("low "));
    REQUIRE(std::stoul(events[2].substr(4)) <= 256 * 1024);
}

TEST_CASE("WebSocket producer waiting for the queue to drain", "[websocket][flow][integration]") {
    constexpr uint32_t count = 400;
    constexpr size_t size = 64 * 1024;
    constexpr size_t high = 1024 * 1024;

    http::server server;
    auto state = std::make_shared<producer_state>();
    server.get("/push", [state](http::request&, http::response& res) {
        res.upgrade_websocket([state](ws_ptr ws) -> awaitable<void> {
            ws->set_write_limits({.max_messages = 0, .high_watermark = high, .low_watermark = high / 4});
            co_await start_producer(watermark_producer(ws, state, count, size), ws, state);
        });
    });
    running_server running(server);

    raw_ws_client client(running.port);
    REQUIRE(client.handshake("/push").starts_with("HTTP/1.1 101"));
    // stalled at the high watermark
    auto ws = state->created.get_future().get().lock();
    REQUIRE(stalled(*state, ws, high));
    REQUIRE(state->sent.load() < count);
    REQUIRE(state->max_queued.load() == high);

    REQUIRE(read_indexes(client, 5s, count) == range(0, count));
    REQUIRE(eventually([&] { return state->done.load(); }));
    REQUIRE_FALSE(state->failed.load());
}

TEST_CASE("WebSocket awaitable send from another thread, and once closed", "[websocket][flow][integration]") {
    http::server server;
    std::promise<ws_ptr> created;
    server.get("/ws", [&](http::request&, http::response& res) {
        res.upgrade_websocket([&](ws_ptr ws) { created.set_value(ws); });
    });
    running_server running(server);

    raw_ws_client client(running.port);
    REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
    auto ws = created.get_future().get();

    // a coroutine on another io_context
    boost::asio::io_context other;
    auto send = [&](std::string text) {
        auto result = co_spawn(other, ws->send_text_async(std::move(text)), boost::asio::use_future);
        other.restart();
        other.run();
        return result.get();
    };
    REQUIRE(send("hello"));
    auto frame = client.read_frame();
    REQUIRE(frame);
    REQUIRE(frame->opcode == OP_TEXT);
    REQUIRE(frame->payload == "hello");

    client.send_frame(OP_CLOSE, close_payload(1000));
    REQUIRE(client.closed_within(2s));
    REQUIRE(eventually([&] { return !send("late"); }));
}

TEST_CASE("WebSocket pause_reading", "[websocket][flow][integration]") {
    constexpr size_t size = 16 * 1024;

    http::server server;
    std::mutex mutex;
    std::vector<uint32_t> received;
    std::promise<ws_ptr> created;
    server.get("/pause", [&](http::request&, http::response& res) {
        res.upgrade_websocket([&](ws_ptr ws) {
            ws->on_message([&, ws](std::string message, bool) {
                std::lock_guard lock(mutex);
                received.push_back(index_of(message));
                // the sink is congested after the first message
                if (received.size() == 1) ws->pause_reading();
            });
            created.set_value(ws);
        });
    });
    running_server running(server);
    auto received_count = [&] {
        std::lock_guard lock(mutex);
        return received.size();
    };

    SECTION("The client blocks, and nothing is lost on resume") {
        nonblocking_writer writer(running.port);
        auto ws = created.get_future().get();

        // TCP backpressure: the client cannot write any more
        REQUIRE(writer.write_until_blocked(size, 500ms, 256 * 1024 * 1024));
        REQUIRE(received_count() == 1);
        REQUIRE(on_ws_thread(ws, [&] { return ws->is_reading_paused(); }));

        ws->resume_reading();
        writer.finish(size, 10);
        auto total = writer.messages();
        REQUIRE(eventually([&] { return received_count() == total; }));
        std::lock_guard lock(mutex);
        REQUIRE(received == range(0, total));
    }

    SECTION("stop() closes it while paused") {
        raw_ws_client client(running.port);
        REQUIRE(client.handshake("/pause").starts_with("HTTP/1.1 101"));
        std::weak_ptr<http::websocket_connection> weak;
        {
            auto ws = created.get_future().get();
            weak = ws;
            for (uint32_t i = 0; i < 5; ++i) client.send_frame(OP_BINARY, indexed(i, size));
            REQUIRE(eventually([&] { return received_count() == 1; }));
            ws->stop();
        }
        auto close = client.read_frame(3s);
        REQUIRE(close);
        REQUIRE(close->opcode == OP_CLOSE);
        REQUIRE(close->close_code() == 1000);
        client.send_frame(OP_CLOSE, close_payload(1000));
        REQUIRE(client.closed_within(3s));
        REQUIRE(eventually([&] { return weak.expired(); }, 5s));
        REQUIRE(received_count() == 1);
    }
}

TEST_CASE("WebSocket overflow policies", "[websocket][flow][integration]") {
    http::server server;
    std::promise<ws_ptr> created;
    std::function<void(const ws_ptr&)> setup;
    server.get("/ws", [&](http::request&, http::response& res) {
        res.upgrade_websocket([&](ws_ptr ws) {
            setup(ws);
            created.set_value(ws);
        });
    });
    running_server running(server);
    raw_ws_client client(running.port);

    SECTION("drop_newest by default: 100 messages") {
        bool congested = false;
        http::write_limits limits;
        setup = [&](const ws_ptr& ws) {
            limits = ws->get_write_limits();
            // queued at once (the first one may be being written, still counted)
            for (uint32_t i = 0; i < 150; ++i) ws->send_text(indexed(i, 10));
            congested = ws->queue_full() && ws->congested_connection();
        };
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        created.get_future().get();
        REQUIRE(limits.max_messages == 100);
        REQUIRE(limits.max_bytes == 0);
        REQUIRE(limits.policy == http::overflow_policy::drop_newest);
        REQUIRE(congested);
        REQUIRE(read_indexes(client) == range(0, 100));
    }

    SECTION("drop_newest with a byte limit") {
        size_t queued = 0;
        setup = [&](const ws_ptr& ws) {
            // (the first message is being written as the others are queued: still counted)
            ws->set_write_limits({.max_messages = 0, .max_bytes = 1000});
            for (uint32_t i = 0; i < 5; ++i) ws->send_binary(indexed(i, 300));   // 3 fit
            ws->send_binary(indexed(5, 50));                                      // fits: 950
            ws->send_binary(indexed(6, 51));                                      // 1001: dropped
            queued = ws->queued_bytes();
        };
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        created.get_future().get();
        REQUIRE(queued == 950);
        REQUIRE(read_indexes(client) == std::vector<uint32_t>{0, 1, 2, 5});
    }

    SECTION("An empty queue takes a message larger than the byte limit") {
        setup = [&](const ws_ptr& ws) {
            ws->set_write_limits({.max_messages = 0, .max_bytes = 100});
            ws->send_binary(indexed(0, 1000));
            ws->send_binary(indexed(1, 10));   // dropped
        };
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        created.get_future().get();
        REQUIRE(read_indexes(client) == std::vector<uint32_t>{0});
    }

    SECTION("drop_oldest") {
        size_t queued = 0;
        setup = [&](const ws_ptr& ws) {
            ws->set_write_limits({.max_messages = 10, .policy = http::overflow_policy::drop_oldest});
            for (uint32_t i = 0; i < 30; ++i) ws->send_binary(indexed(i, 100));
            queued = ws->queued_bytes();
        };
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        created.get_future().get();
        REQUIRE(queued == 1000);
        // the first message may be being written as the others are queued (co_spawn starts
        // the writer inline since Boost 1.80): then it is not dropped
        auto in_flight = range(21, 30);
        in_flight.insert(in_flight.begin(), 0);
        auto received = read_indexes(client);
        REQUIRE((received == range(20, 30) || received == in_flight));
    }

    SECTION("close: 1013 Try Again Later") {
        setup = [&](const ws_ptr& ws) {
            ws->set_write_limits({.max_messages = 10, .policy = http::overflow_policy::close});
            for (uint32_t i = 0; i < 11; ++i) ws->send_binary(indexed(i, 100));
        };
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        std::weak_ptr<http::websocket_connection> weak = created.get_future().get();
        // the message being written is completed (the first one), then the close frame
        auto frame = client.read_frame(3s);
        REQUIRE(frame);
        if (frame->opcode == OP_BINARY) {
            REQUIRE(index_of(frame->payload) == 0);
            frame = client.read_frame(3s);
            REQUIRE(frame);
        }
        REQUIRE(frame->opcode == OP_CLOSE);
        REQUIRE(frame->close_code() == 1013);
        client.send_frame(OP_CLOSE, close_payload(1013));
        REQUIRE(client.closed_within(3s));
        REQUIRE(eventually([&] { return weak.expired(); }, 5s));
    }
}

TEST_CASE("WebSocket drop_oldest falls back to drop_newest", "[websocket][flow][integration]") {
    // awaited messages are never dropped: with nothing else to drop, the new message goes
    static constexpr size_t big = 16 * 1024 * 1024;
    http::server server;
    std::promise<size_t> queued;
    server.get("/ws", [&](http::request&, http::response& res) {
        res.upgrade_websocket([&](ws_ptr ws) -> awaitable<void> {
            ws->set_write_limits({.max_messages = 3, .policy = http::overflow_policy::drop_oldest});
            auto executor = co_await boost::asio::this_coro::executor;
            // larger than the socket buffers: the first one is still being written below
            for (uint32_t i = 0; i < 3; ++i) {
                co_spawn(executor, [ws, i]() -> awaitable<void> { co_await ws->send_binary_async(indexed(i, big)); },
                         detached);
            }
            // the three are queued (started now, or before this resumes)
            co_await boost::asio::post(executor, use_awaitable);
            ws->send_binary(indexed(3, 10));
            queued.set_value(ws->queued_messages());
        });
    });
    running_server running(server);
    raw_ws_client client(running.port);
    REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
    REQUIRE(queued.get_future().get() == 3);
    REQUIRE(read_indexes(client, 5s) == range(0, 3));
}

TEST_CASE("WebSocket watermark callbacks that send", "[websocket][flow][integration]") {
    http::server server;
    std::promise<int> lows_in_handler;
    auto lows = std::make_shared<std::atomic<int>>(0);
    std::promise<std::weak_ptr<http::websocket_connection>> created;
    std::function<void(const ws_ptr&)> setup;
    server.get("/ws", [&](http::request&, http::response& res) {
        res.upgrade_websocket([&](ws_ptr ws) {
            ws->on_low_watermark([lows, weak = std::weak_ptr(ws)] {
                ++*lows;
                // re-enters the queue: never while it is being changed
                if (auto ws = weak.lock()) {
                    for (int i = 0; i < 4000; ++i) ws->send_binary("x");
                }
            });
            setup(ws);
            lows_in_handler.set_value(lows->load());
            created.set_value(ws);
        });
    });
    running_server running(server);
    raw_ws_client client(running.port);

    SECTION("during drop_oldest") {
        setup = [](const ws_ptr& ws) {
            ws->set_write_limits({.max_messages = 0, .max_bytes = 640 << 10, .high_watermark = 500 << 10,
                                  .low_watermark = 450 << 10, .policy = http::overflow_policy::drop_oldest});
            for (int i = 0; i < 10; ++i) ws->send_binary(std::string(64 * 1024, 'y'));
            // drops five messages: below the low watermark
            ws->send_binary(std::string(300 * 1024, 'z'));
        };
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        REQUIRE(lows_in_handler.get_future().get() == 0);
        // told afterwards; what it sends arrives
        REQUIRE(eventually([&] { return lows->load() >= 1; }));
        size_t ones = 0;
        while (auto frame = client.read_frame(2s)) {
            if (frame->payload == "x" && ++ones == 4000) break;
        }
        REQUIRE(ones == 4000);
    }

    SECTION("never once it is closing") {
        setup = [](const ws_ptr& ws) {
            ws->set_write_limits({.max_messages = 0, .high_watermark = 1 << 20, .low_watermark = 512 << 10});
            for (int i = 0; i < 200; ++i) ws->send_binary(std::string(64 * 1024, 'y'));
            ws->stop();
        };
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        REQUIRE(lows_in_handler.get_future().get() == 0);
        auto weak = created.get_future().get();
        // the client reads up to the close frame, and answers it
        std::optional<ws_frame> frame;
        while ((frame = client.read_frame(5s)) && frame->opcode != OP_CLOSE) {}
        REQUIRE(frame);
        client.send_frame(OP_CLOSE, close_payload(1000));
        REQUIRE(client.closed_within(5s));
        REQUIRE(eventually([&] { return weak.expired(); }));
        REQUIRE(lows->load() == 0);
    }
}

TEST_CASE("WebSocket limits changed while congested", "[websocket][flow][integration]") {
    http::server server;
    std::promise<ws_ptr> created;
    auto lows = std::make_shared<std::atomic<int>>(0);
    server.get("/ws", [&](http::request&, http::response& res) {
        res.upgrade_websocket([&](ws_ptr ws) {
            ws->set_write_limits({.max_messages = 0, .high_watermark = 1 << 20, .low_watermark = 256 << 10});
            ws->on_low_watermark([lows] { ++*lows; });
            for (int i = 0; i < 400; ++i) ws->send_binary(std::string(64 * 1024, 'y'));
            created.set_value(ws);
        });
    });
    running_server running(server);
    raw_ws_client client(running.port);
    REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
    auto ws = created.get_future().get();

    // congested: a producer on another thread waits, until the watermarks are disabled
    boost::asio::io_context other;
    auto writable = co_spawn(other, ws->wait_writable(), boost::asio::use_future);
    std::thread producer([&] { other.run(); });
    REQUIRE(writable.wait_for(200ms) == std::future_status::timeout);
    ws->set_write_limits({.max_messages = 0});
    REQUIRE(writable.wait_for(5s) == std::future_status::ready);
    REQUIRE(writable.get());
    producer.join();
    REQUIRE(eventually([&] { return lows->load() == 1; }));
}

TEST_CASE("WebSocket write timeout", "[websocket][flow][integration]") {
    constexpr size_t size = 64 * 1024;
    http::server server;
    auto state = std::make_shared<producer_state>();
    auto writable = std::make_shared<std::promise<bool>>();
    server.get("/push", [state, writable](http::request&, http::response& res) {
        res.upgrade_websocket([state, writable](ws_ptr ws) -> awaitable<void> {
            ws->set_write_limits({.max_messages = 0, .high_watermark = 1 << 20, .write_timeout = 300ms});
            auto executor = co_await boost::asio::this_coro::executor;
            // a producer waiting for the queue to drain, and another one awaiting its sends
            co_spawn(executor, [ws, writable]() -> awaitable<void> {
                for (uint32_t i = 0; i < 400; ++i) ws->send_binary(indexed(i, size));
                writable->set_value(co_await ws->wait_writable());
            }, detached);
            co_await start_producer(awaited_producer(ws, state, 400, size), ws, state);
        });
    });
    running_server running(server);
    raw_ws_client client(running.port);
    REQUIRE(client.handshake("/push").starts_with("HTTP/1.1 101"));

    // The client does not read: the write in progress times out, and both producers are told
    REQUIRE(eventually([&] { return state->done.load(); }));
    REQUIRE(state->failed.load());
    auto result = writable->get_future();
    REQUIRE(result.wait_for(5s) == std::future_status::ready);
    REQUIRE_FALSE(result.get());
    // closed with 1013, after the rest of the frame being written
    std::optional<ws_frame> frame;
    while ((frame = client.read_frame(5s)) && frame->opcode != OP_CLOSE) {}
    REQUIRE(frame);
    REQUIRE(frame->close_code() == 1013);
}

TEST_CASE("WebSocket close codes of messages not delivered", "[websocket][flow][integration]") {
    http::server server;
    server.get("/ws", [](http::request&, http::response& res) {
        res.upgrade_websocket([](ws_ptr ws) { ws->on_message([](std::string, bool) {}); });
    });
    running_server running(server);
    raw_ws_client client(running.port);
    REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));

    SECTION("Invalid UTF-8: 1007") {
        client.send_frame(OP_TEXT, "\xC0\xAF");
        auto frame = client.read_frame(3s);
        REQUIRE(frame);
        REQUIRE(frame->opcode == OP_CLOSE);
        REQUIRE(frame->close_code() == 1007);
    }

    SECTION("Larger than the buffer: 1009") {
        // the header of a 32 MB frame, and the start of its payload
        auto frame = make_frame(OP_BINARY, std::string(32 * 1024 * 1024, 'x'));
        client.send_raw(frame.substr(0, 64 * 1024));
        auto close = client.read_frame(3s);
        REQUIRE(close);
        REQUIRE(close->opcode == OP_CLOSE);
        REQUIRE(close->close_code() == 1009);
    }
}

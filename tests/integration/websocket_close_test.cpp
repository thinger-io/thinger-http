#include <catch2/catch_test_macros.hpp>
#include <thinger/asio/async_event.hpp>
#include <thinger/asio/io_context_of.hpp>
#include <thinger/asio/sockets/websocket.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/websocket_connection.hpp>

#include "server_test_helpers.hpp"
#include "../fixtures/awaited_shutdown.hpp"
#include "../fixtures/stream_test_helpers.hpp"

#include <boost/asio/use_future.hpp>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

// Closing a server websocket_connection with a code and a reason of its own, as ws.close(code,
// reason) in Node: what the peer reads, what on_close() is told, and the arguments refused

using namespace thinger;
using namespace std::chrono_literals;
using namespace stream_test;
using server_test::running_server;
using thinger::http::test::awaited_shutdown;
using thinger::http::test::transport;

namespace {

    using ws_ptr = std::shared_ptr<http::websocket_connection>;

    constexpr size_t LARGE_MESSAGE_SIZE = 16 * 1024 * 1024;    // more than the socket buffers hold

    template<typename F>
    bool eventually(F condition, std::chrono::milliseconds timeout = 5s) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!condition()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(2ms);
        }
        return true;
    }

    // What on_close() of a connection was told
    class close_record {
    public:
        void told(const http::websocket_close& close, bool on_executor) {
            std::scoped_lock lock(mutex_);
            if (closes_.empty()) first_at_ = std::chrono::steady_clock::now();
            closes_.push_back(close);
            if (!on_executor) off_executor_ = true;
        }

        // When the first close was told
        std::chrono::steady_clock::time_point first_at() const {
            std::scoped_lock lock(mutex_);
            return first_at_;
        }

        size_t count() const {
            std::scoped_lock lock(mutex_);
            return closes_.size();
        }

        bool off_executor() const {
            std::scoped_lock lock(mutex_);
            return off_executor_;
        }

        // The first close told, waiting for it
        std::optional<http::websocket_close> wait(std::chrono::milliseconds timeout = 5s) const {
            if (!eventually([&] { return count() > 0; }, timeout)) return std::nullopt;
            std::scoped_lock lock(mutex_);
            return closes_.front();
        }

    private:
        mutable std::mutex mutex_;
        std::vector<http::websocket_close> closes_;
        std::chrono::steady_clock::time_point first_at_;
        bool off_executor_ = false;
    };

    struct accepted {
        ws_ptr ws;
        std::shared_ptr<close_record> closes;
    };

    // A server whose /ws records every connection and what its on_close() is told
    class close_server {
    public:
        explicit close_server(transport kind = transport::tcp, http::websocket_accept_options options = {}) {
            server.get("/ws", [this, options](http::request&, http::response& res) {
                res.upgrade_websocket(options, [this](ws_ptr ws) {
                    accepted connection{ws, std::make_shared<close_record>()};
                    ws->on_close([closes = connection.closes, executor = ws->get_executor()](const http::websocket_close& close) {
                        closes->told(close, asio::running_in_this_thread(executor));
                    });
                    if (setup) setup(connection);
                    std::scoped_lock lock(mutex_);
                    accepted_.push_back(std::move(connection));
                });
            });
            running_.emplace(server, kind);
        }

        const http::test::transport_endpoint& endpoint() const { return running_->endpoint(); }

        // The next connection accepted
        accepted next() {
            accepted connection;
            auto found = eventually([&] {
                std::scoped_lock lock(mutex_);
                if (accepted_.empty()) return false;
                connection = std::move(accepted_.front());
                accepted_.pop_front();
                return true;
            });
            REQUIRE(found);
            return connection;
        }

        http::server server;
        // In the handler that accepts each connection, before start(). Set before connecting
        std::function<void(const accepted&)> setup;

    private:
        std::mutex mutex_;
        std::deque<accepted> accepted_;
        std::optional<running_server> running_;
    };

    std::string reason_of(const ws_frame& frame) { return frame.payload.substr(2); }

    // The next close frame, skipping pings and data
    std::optional<ws_frame> read_close(raw_ws_client& client, std::chrono::milliseconds timeout = 5s) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) return std::nullopt;
            auto frame = client.read_frame(left);
            if (!frame || frame->opcode == OP_CLOSE) return frame;
        }
    }

    // The connection is open and usable: a message sent gets to the client, with no close before
    void require_open(const ws_ptr& ws, raw_ws_client& client) {
        ws->send_text("alive");
        auto frame = client.read_frame(3s);
        REQUIRE(frame);
        REQUIRE(frame->opcode == OP_TEXT);
        REQUIRE(frame->payload == "alive");
    }

    // The close frame carries `code` and `reason`; once answered with the same code, on_close()
    // is told it, once, on the websocket thread, and the connection ends
    void require_closes_with(const accepted& connection, raw_ws_client& client, uint16_t code,
                             const std::string& reason = "") {
        auto frame = read_close(client);
        REQUIRE(frame);
        REQUIRE(frame->close_code() == code);
        REQUIRE(reason_of(*frame) == reason);
        REQUIRE(connection.closes->count() == 0);

        client.send_frame(OP_CLOSE, close_payload(code));
        auto closed = connection.closes->wait();
        REQUIRE(closed);
        REQUIRE(closed->code == code);
        REQUIRE_FALSE(closed->remote);
        REQUIRE(client.closed_within(3s));

        std::this_thread::sleep_for(20ms);
        REQUIRE(connection.closes->count() == 1);
        REQUIRE_FALSE(connection.closes->off_executor());
    }

    // A frame written by a websocket in any role, with its payload unmasked: `bytes` holds it whole
    struct written_frame {
        uint8_t opcode = 0;
        bool masked = false;
        std::string payload;
    };

    written_frame take_frame(std::string& bytes) {
        REQUIRE(bytes.size() >= 2);
        written_frame frame;
        frame.opcode = uint8_t(bytes[0]) & 0x0F;
        frame.masked = uint8_t(bytes[1]) & 0x80;
        size_t size = uint8_t(bytes[1]) & 0x7F;
        REQUIRE(size <= 125);
        size_t header = frame.masked ? 6 : 2;
        REQUIRE(bytes.size() >= header + size);
        frame.payload = bytes.substr(header, size);
        if (frame.masked) {
            for (size_t i = 0; i < size; ++i) frame.payload[i] ^= bytes[2 + i % 4];
        }
        bytes.erase(0, header + size);
        return frame;
    }

    // Its writes wait until the gate opens: a write in progress, as long as a test needs it
    class gated_socket : public memory_socket {
    public:
        gated_socket(net::io_context& io_context, std::function<void(std::string)> on_write)
            : memory_socket(io_context, std::move(on_write)), gate(io_context.get_executor()) {}

        using memory_socket::write;

        thinger::awaitable<thinger::io_result> write(const uint8_t buffer[], size_t size) override {
            co_await gate.wait();
            co_return co_await memory_socket::write(buffer, size);
        }

        asio::async_event gate;
    };

    // The closing handshake has CLOSE_TIMEOUT from the call: no earlier than it (minus this, for
    // a close that ends at the deadline), and no later than it plus the tolerance of a test
    constexpr auto EARLY = 100ms;
    constexpr auto TOLERANCE = 2s;

    void check_at_close_deadline(std::chrono::steady_clock::duration elapsed) {
        INFO("elapsed " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms");
        CHECK(elapsed >= asio::websocket::CLOSE_TIMEOUT - EARLY);
        CHECK(elapsed <= asio::websocket::CLOSE_TIMEOUT + TOLERANCE);
    }

    // What a peer does once the connection is told to close (no peer here answers it)
    enum class peer_behaviour {
        silent,         // reads the close frame, nothing else
        no_read,        // reads nothing, with a message larger than the socket buffers in flight
        trickle,        // the same, reading 1 KiB every 100 ms
        late_reader     // the same, reading it all once the close was waiting for a while
    };

    // One close under a hostile peer
    struct hostile_close {
        const char* name;
        peer_behaviour peer;
        // stop(1008, "x"), or else stop()
        bool with_code = true;
        http::write_limits limits{};
        // Sends without waiting once closing
        bool sends_while_closing = false;

        std::unique_ptr<raw_ws_client> client{};
        accepted connection{};
        std::chrono::steady_clock::time_point called{};
        std::optional<ws_frame> frame{};

        uint16_t code_asked() const { return with_code ? 1008 : 1000; }
        std::string reason_asked() const { return with_code ? "x" : ""; }
        bool frame_gets_out() const { return peer == peer_behaviour::silent || peer == peer_behaviour::late_reader; }

        // On a thread of its own
        void run_peer(const std::atomic<bool>& done) {
            switch (peer) {
                case peer_behaviour::no_read:
                    return;
                case peer_behaviour::trickle:
                    while (!done) {
                        client->read_at_most(1024, 50ms);
                        std::this_thread::sleep_for(100ms);
                    }
                    return;
                case peer_behaviour::late_reader:
                    std::this_thread::sleep_for(1500ms);
                    [[fallthrough]];
                case peer_behaviour::silent:
                    frame = read_close(*client, asio::websocket::CLOSE_TIMEOUT + TOLERANCE);
                    return;
            }
        }
    };

}

TEST_CASE("WebSocket stop(code, reason): the peer reads them and on_close() is told", "[websocket][close][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    close_server fixture(kind);
    raw_ws_client client(fixture.endpoint());
    REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
    auto connection = fixture.next();

    SECTION("From the websocket thread") {
        connection.ws->execute([ws = connection.ws] { ws->stop(1008, "token expired"); });
        require_closes_with(connection, client, 1008, "token expired");
    }

    SECTION("From another thread") {
        connection.ws->stop(1008, "token expired");
        require_closes_with(connection, client, 1008, "token expired");
    }

    SECTION("Without a reason: the frame has only the code") {
        connection.ws->stop(4000);
        auto frame = client.read_frame();
        REQUIRE(frame);
        REQUIRE(frame->opcode == OP_CLOSE);
        REQUIRE(frame->payload.size() == 2);
        REQUIRE(frame->close_code() == 4000);
        client.send_frame(OP_CLOSE, close_payload(4000));
        auto closed = connection.closes->wait();
        REQUIRE(closed);
        REQUIRE(closed->code == 4000);
    }

    SECTION("stop() without arguments is 1000, without a reason") {
        connection.ws->stop();
        auto frame = client.read_frame();
        REQUIRE(frame);
        REQUIRE(frame->opcode == OP_CLOSE);
        REQUIRE(frame->payload.size() == 2);
        REQUIRE(frame->close_code() == 1000);
        client.send_frame(OP_CLOSE, close_payload(1000));
        auto closed = connection.closes->wait();
        REQUIRE(closed);
        REQUIRE(closed->code == 1000);
        REQUIRE_FALSE(closed->remote);
    }
}

TEST_CASE("WebSocket stop(code, reason) with producers waiting", "[websocket][close][integration]") {
    close_server fixture;
    raw_ws_client client(fixture.endpoint());
    REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
    auto [ws, closes] = fixture.next();

    // The client does not read yet: the first message stays in flight, the second one queued
    auto written = co_spawn(ws->get_executor(), ws->send_binary_async(std::string(LARGE_MESSAGE_SIZE, 'x')), boost::asio::use_future);
    auto waiting = co_spawn(ws->get_executor(), ws->send_text_async("never written"), boost::asio::use_future);
    REQUIRE(eventually([&, &ws = ws] { return ws->queued_messages() == 2; }));
    ws->stop(1008, "x");

    // The message in flight is completed, and the close frame goes right after it
    auto message = client.read_frame(30s);
    REQUIRE(message);
    REQUIRE(message->opcode == OP_BINARY);
    REQUIRE(message->payload.size() == LARGE_MESSAGE_SIZE);
    auto frame = client.read_frame();
    REQUIRE(frame);
    REQUIRE(frame->opcode == OP_CLOSE);
    REQUIRE(frame->close_code() == 1008);

    REQUIRE(written.wait_for(5s) == std::future_status::ready);
    REQUIRE(written.get());
    REQUIRE(waiting.wait_for(5s) == std::future_status::ready);
    REQUIRE_FALSE(waiting.get());

    // Told false, the handshake goes on: the code told is the one of the answer
    REQUIRE(closes->count() == 0);
    client.send_frame(OP_CLOSE, close_payload(1008, "understood"));
    auto closed = closes->wait();
    REQUIRE(closed);
    REQUIRE(closed->code == 1008);
    REQUIRE(closed->reason == "understood");
    REQUIRE_FALSE(closed->remote);
    REQUIRE(ws->queued_messages() == 0);
}

TEST_CASE("WebSocket stop(code, reason) with reading paused", "[websocket][close][integration]") {
    close_server fixture;
    fixture.setup = [](const accepted& connection) { connection.ws->pause_reading(); };
    raw_ws_client client(fixture.endpoint());
    REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
    auto connection = fixture.next();

    // What the client sends is not read, nor delivered
    std::atomic<int> delivered{0};
    connection.ws->execute([&, ws = connection.ws] {
        ws->on_message([&](std::string, bool) { ++delivered; });
    });
    client.send_frame(OP_BINARY, "ignored");

    SECTION("It closes") {
        connection.ws->stop(1008, "x");
        require_closes_with(connection, client, 1008, "x");
    }

    SECTION("Resuming while closing does not read at the same time as the handshake") {
        connection.ws->stop(1008, "x");
        auto frame = read_close(client);
        REQUIRE(frame);
        REQUIRE(frame->close_code() == 1008);
        connection.ws->resume_reading();
        client.send_frame(OP_BINARY, "ignored too");
        client.send_frame(OP_CLOSE, close_payload(1008));
        auto closed = connection.closes->wait();
        REQUIRE(closed);
        REQUIRE(closed->code == 1008);
        REQUIRE(client.closed_within(3s));
    }

    REQUIRE(delivered == 0);
}

TEST_CASE("WebSocket stop(code, reason) from on_shutdown()", "[websocket][close][shutdown][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    close_server fixture(kind);
    fixture.setup = [](const accepted& connection) {
        connection.ws->on_shutdown([weak = std::weak_ptr(connection.ws)] {
            if (auto ws = weak.lock()) ws->stop(1012, "restarting");
        });
    };
    raw_ws_client client(fixture.endpoint());
    REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
    auto connection = fixture.next();

    // (not 1001; and the server drains once it closes, well before the grace period)
    awaited_shutdown shutdown(fixture.server, 8s);
    require_closes_with(connection, client, 1012, "restarting");
    REQUIRE(shutdown.ended_within(3s));
    REQUIRE(shutdown.get());
}

TEST_CASE("WebSocket stop(code, reason) refuses what a close frame may not carry", "[websocket][close][integration]") {
    close_server fixture;
    raw_ws_client client(fixture.endpoint());
    REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
    auto connection = fixture.next();
    const auto& ws = connection.ws;

    SECTION("Codes that may not be sent") {
        for (uint16_t code : {0, 999, 1004, 1005, 1006, 1015, 1016, 2999, 5000}) {
            INFO("code " << code);
            REQUIRE_THROWS_AS(ws->stop(code), std::invalid_argument);
            REQUIRE_THROWS_AS(ws->stop(code, "a reason"), std::invalid_argument);
        }
        require_open(ws, client);
    }

    SECTION("A reason above 123 bytes") {
        REQUIRE_THROWS_AS(ws->stop(1008, std::string(124, 'r')), std::invalid_argument);
        // counted in bytes, not in characters: 62 characters of 2 bytes
        std::string characters;
        for (int i = 0; i < 62; ++i) characters += "\xC3\xA9";
        REQUIRE_THROWS_AS(ws->stop(1008, characters), std::invalid_argument);
        require_open(ws, client);
    }

    SECTION("A reason not valid as UTF-8") {
        for (std::string reason : {"\xC3", "\xFF", "ok\xC0\x80"}) {
            INFO("reason of " << reason.size() << " bytes");
            REQUIRE_THROWS_AS(ws->stop(1008, reason), std::invalid_argument);
        }
        require_open(ws, client);
    }

    SECTION("A reason of exactly 123 bytes arrives whole") {
        std::string reason(123, 'r');
        ws->stop(1008, reason);
        require_closes_with(connection, client, 1008, reason);
    }

    SECTION("A reason of 123 bytes in characters of several") {
        std::string reason = "r";
        for (int i = 0; i < 61; ++i) reason += "\xC3\xA9";
        REQUIRE(reason.size() == 123);
        ws->stop(1008, reason);
        require_closes_with(connection, client, 1008, reason);
    }

    SECTION("Validated the same once closed") {
        ws->stop(1008, "x");
        require_closes_with(connection, client, 1008, "x");
        REQUIRE_THROWS_AS(ws->stop(1005), std::invalid_argument);
        REQUIRE_THROWS_AS(ws->stop(1008, "\xFF"), std::invalid_argument);
    }
}

TEST_CASE("WebSocket stop(code, reason) with every code that may be sent", "[websocket][close][integration]") {
    close_server fixture;
    for (uint16_t code : {1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 3000, 4999}) {
        INFO("code " << code);
        raw_ws_client client(fixture.endpoint());
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        auto connection = fixture.next();
        REQUIRE_NOTHROW(connection.ws->stop(code));
        require_closes_with(connection, client, code);
    }
}

TEST_CASE("WebSocket stop(code, reason): the first close decides", "[websocket][close][integration]") {
    close_server fixture;
    raw_ws_client client(fixture.endpoint());
    REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
    auto connection = fixture.next();
    const auto& ws = connection.ws;

    SECTION("Called again while closing") {
        ws->stop(1008, "first");
        ws->stop(1011, "second");
        ws->stop();
        auto frame = read_close(client);
        REQUIRE(frame);
        REQUIRE(frame->close_code() == 1008);
        REQUIRE(reason_of(*frame) == "first");
        // nothing else comes, and the handshake still waits for the answer
        REQUIRE_FALSE(client.read_frame(300ms));
        REQUIRE_FALSE(client.eof());
        REQUIRE(connection.closes->count() == 0);
        ws->stop(1011, "third");
        client.send_frame(OP_CLOSE, close_payload(1008));
        auto closed = connection.closes->wait();
        REQUIRE(closed);
        REQUIRE(closed->code == 1008);
        REQUIRE(client.closed_within(3s));
    }

    SECTION("Called once closed") {
        ws->stop(1008, "first");
        require_closes_with(connection, client, 1008, "first");
        ws->stop(1011, "late");
        ws->stop();
        std::this_thread::sleep_for(50ms);
        REQUIRE(connection.closes->count() == 1);
    }

    SECTION("From several threads at once") {
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (uint16_t code : {3000, 3001, 3002, 3003}) {
            threads.emplace_back([&, code] {
                while (!go) std::this_thread::yield();
                ws->stop(code, std::to_string(code));
            });
        }
        go = true;
        for (auto& thread : threads) thread.join();
        auto frame = read_close(client);
        REQUIRE(frame);
        auto code = frame->close_code();
        REQUIRE(code >= 3000);
        REQUIRE(code <= 3003);
        REQUIRE(reason_of(*frame) == std::to_string(code));
        REQUIRE_FALSE(client.read_frame(300ms));
        client.send_frame(OP_CLOSE, close_payload(code));
        auto closed = connection.closes->wait();
        REQUIRE(closed);
        REQUIRE(closed->code == code);
        REQUIRE(client.closed_within(3s));
        std::this_thread::sleep_for(20ms);
        REQUIRE(connection.closes->count() == 1);
    }

    SECTION("A server shutdown afterwards does not close again") {
        ws->stop(1008, "first");
        auto frame = read_close(client);
        REQUIRE(frame);
        REQUIRE(frame->close_code() == 1008);
        awaited_shutdown shutdown(fixture.server, 3s);
        REQUIRE_FALSE(client.read_frame(300ms));
        client.send_frame(OP_CLOSE, close_payload(1008));
        auto closed = connection.closes->wait();
        REQUIRE(closed);
        REQUIRE(closed->code == 1008);
        REQUIRE(shutdown.ended_within(3s));
        REQUIRE(shutdown.get());
    }

    SECTION("The peer answers with another code: told that one, and its reason") {
        ws->stop(1008, "first");
        REQUIRE(read_close(client));
        client.send_frame(OP_CLOSE, close_payload(1000, "bye"));
        auto closed = connection.closes->wait();
        REQUIRE(closed);
        REQUIRE(closed->code == 1000);
        REQUIRE(closed->reason == "bye");
        REQUIRE_FALSE(closed->remote);
    }

    SECTION("The peer closed first: told its code, and stop() does nothing") {
        client.send_frame(OP_CLOSE, close_payload(4001, "mine"));
        auto closed = connection.closes->wait();
        REQUIRE(closed);
        REQUIRE(closed->code == 4001);
        REQUIRE(closed->reason == "mine");
        REQUIRE(closed->remote);
        ws->stop(1008, "late");
        auto frame = read_close(client);
        REQUIRE(frame);
        REQUIRE(frame->close_code() == 4001);
        REQUIRE_FALSE(client.read_frame(300ms));
        REQUIRE(connection.closes->count() == 1);
    }

    SECTION("The peer closes at the same time: one frame each way, told its code, once") {
        client.send_frame(OP_CLOSE, close_payload(4001, "mine"));
        ws->stop(1008, "x");
        auto closed = connection.closes->wait();
        REQUIRE(closed);
        REQUIRE(closed->code == 4001);
        REQUIRE(closed->reason == "mine");
        auto frame = read_close(client);
        REQUIRE(frame);
        REQUIRE((frame->close_code() == 4001 || frame->close_code() == 1008));
        REQUIRE_FALSE(client.read_frame(300ms));
        REQUIRE(connection.closes->count() == 1);
    }
}

TEST_CASE("WebSocket stop(code, reason) from the callbacks and before start()", "[websocket][close][integration]") {
    close_server fixture;

    SECTION("In the handler that accepts it, before start()") {
        fixture.setup = [](const accepted& connection) { connection.ws->stop(1008, "early"); };
        raw_ws_client client(fixture.endpoint());
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        require_closes_with(fixture.next(), client, 1008, "early");
    }

    SECTION("From on_message(): on_close() comes after it") {
        std::atomic<bool> told_within_the_call{false};
        fixture.setup = [&](const accepted& connection) {
            connection.ws->on_message([&, connection](std::string, bool) {
                connection.ws->stop(1008, "bad message");
                if (connection.closes->count() > 0) told_within_the_call = true;
            });
        };
        raw_ws_client client(fixture.endpoint());
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        auto connection = fixture.next();
        client.send_frame(OP_TEXT, "a message");
        require_closes_with(connection, client, 1008, "bad message");
        REQUIRE_FALSE(told_within_the_call);
    }

    SECTION("From a watermark callback") {
        fixture.setup = [](const accepted& connection) {
            connection.ws->set_write_limits({.max_messages = 0, .high_watermark = 64 * 1024, .low_watermark = 1024});
            connection.ws->on_high_watermark([connection] { connection.ws->stop(1008, "too slow"); });
        };
        raw_ws_client client(fixture.endpoint());
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        auto connection = fixture.next();
        connection.ws->send_binary(std::string(128 * 1024, 'x'));
        auto frame = read_close(client);
        REQUIRE(frame);
        REQUIRE(frame->close_code() == 1008);
        REQUIRE(reason_of(*frame) == "too slow");
        client.send_frame(OP_CLOSE, close_payload(1008));
        auto closed = connection.closes->wait();
        REQUIRE(closed);
        REQUIRE(closed->code == 1008);
    }
}

TEST_CASE("WebSocket stop(code, reason) stops the keepalive", "[websocket][close][integration]") {
    http::websocket_accept_options options;
    options.ping_interval = 100ms;
    options.idle_timeout = 300ms;
    close_server fixture(transport::tcp, options);

    // Several intervals after the close frame: no ping behind it, and the idle timeout does not
    // cut the wait for the answer, which still gets the code asked for
    auto no_keepalive_after_close = [](const accepted& connection, raw_ws_client& client) {
        auto frame = read_close(client);
        REQUIRE(frame);
        REQUIRE(frame->close_code() == 1008);
        REQUIRE_FALSE(client.read_frame(800ms));
        REQUIRE_FALSE(client.eof());
        REQUIRE(connection.closes->count() == 0);
        client.send_frame(OP_CLOSE, close_payload(1008));
        auto closed = connection.closes->wait();
        REQUIRE(closed);
        REQUIRE(closed->code == 1008);
    };

    SECTION("Started") {
        raw_ws_client client(fixture.endpoint());
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        auto connection = fixture.next();
        connection.ws->stop(1008, "x");
        no_keepalive_after_close(connection, client);
    }

    SECTION("Before start(), which does not start it") {
        fixture.setup = [](const accepted& connection) { connection.ws->stop(1008, "x"); };
        raw_ws_client client(fixture.endpoint());
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        no_keepalive_after_close(fixture.next(), client);
    }

    SECTION("Resuming while closing does not restart it") {
        fixture.setup = [](const accepted& connection) { connection.ws->pause_reading(); };
        raw_ws_client client(fixture.endpoint());
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        auto connection = fixture.next();
        connection.ws->stop(1008, "x");
        connection.ws->resume_reading();
        no_keepalive_after_close(connection, client);
    }
}

TEST_CASE("WebSocket stream: close_graceful(code, reason)", "[websocket][close][stream]") {
    const bool server = GENERATE(true, false);
    INFO((server ? "server" : "client") << " role");
    boost::asio::io_context ioc;
    std::string written;
    auto socket = std::make_shared<gated_socket>(ioc, [&](std::string data) { written += data; });
    auto ws = std::make_shared<asio::websocket>(socket, true, server);

    auto close_graceful = [&](uint16_t code, std::string reason) {
        co_spawn(ioc, [ws, code, reason = std::move(reason)]() -> awaitable<void> {
            co_await ws->close_graceful(code, reason);
        }, detached);
    };

    SECTION("The close frame carries the reason") {
        socket->gate.set();
        close_graceful(1008, "token expired");
        ioc.run_for(100ms);
        auto frame = take_frame(written);
        REQUIRE(frame.opcode == OP_CLOSE);
        REQUIRE(frame.masked == !server);
        REQUIRE(frame.payload == close_payload(1008, "token expired"));
        REQUIRE(ws->get_close_code() == 1008);
    }

    SECTION("A reason that does not fit is left out, never a frame the peer refuses") {
        socket->gate.set();
        close_graceful(1008, std::string(124, 'r'));
        ioc.run_for(100ms);
        auto frame = take_frame(written);
        REQUIRE(frame.opcode == OP_CLOSE);
        REQUIRE(frame.payload == close_payload(1008));
    }

    SECTION("Another close while the frame waits for its turn sends that frame, by the handshake on its own") {
        co_spawn(ioc, [ws]() -> awaitable<void> { co_await ws->write_message("hi"); }, detached);
        close_graceful(1008, "first");
        boost::asio::post(ioc, [&] {
            REQUIRE(ws->is_open());
            ws->close(asio::websocket::CLOSE_INTERNAL_ERROR);
            socket->gate.set();
        });
        ioc.run_for(100ms);
        auto message = take_frame(written);
        REQUIRE(message.opcode == OP_BINARY);
        REQUIRE(message.payload == "hi");
        auto frame = take_frame(written);
        REQUIRE(frame.opcode == OP_CLOSE);
        REQUIRE(frame.masked == !server);
        REQUIRE(frame.payload == close_payload(1008, "first"));
        REQUIRE(ws->get_close_code() == 1008);
    }

    REQUIRE(written.empty());
    // (what is left of the handshake ends with the io_context; the socket is closed)
    socket->close();
}

// Each close of the table at once, on one server: a run takes one CLOSE_TIMEOUT
TEST_CASE("WebSocket stop() ends within CLOSE_TIMEOUT whatever the peer does", "[websocket][close][hostile_peer][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    close_server fixture(kind);

    std::deque<hostile_close> closes;
    closes.push_back({.name = "silent", .peer = peer_behaviour::silent});
    closes.push_back({.name = "silent, stop()", .peer = peer_behaviour::silent, .with_code = false});
    closes.push_back({.name = "not reading, write timeout above the deadline", .peer = peer_behaviour::no_read});
    closes.push_back({.name = "not reading, stop()", .peer = peer_behaviour::no_read, .with_code = false});
    closes.push_back({.name = "not reading, write timeout below the deadline", .peer = peer_behaviour::no_read,
                      .limits = {.write_timeout = 1500ms}});
    closes.push_back({.name = "not reading, no write timeout", .peer = peer_behaviour::no_read,
                      .limits = {.write_timeout = 0ms}});
    closes.push_back({.name = "not reading, overflowing while closing, with the close policy", .peer = peer_behaviour::no_read,
                      .limits = {.max_messages = 1, .policy = http::overflow_policy::close}, .sends_while_closing = true});
    closes.push_back({.name = "trickling", .peer = peer_behaviour::trickle});
    closes.push_back({.name = "trickling, stop()", .peer = peer_behaviour::trickle, .with_code = false});
    closes.push_back({.name = "reading too late for the deadline to start over", .peer = peer_behaviour::late_reader});

    for (auto& close : closes) {
        INFO(close.name);
        close.client = std::make_unique<raw_ws_client>(fixture.endpoint());
        REQUIRE(close.client->handshake("/ws").starts_with("HTTP/1.1 101"));
        close.connection = fixture.next();
        close.connection.ws->set_write_limits(close.limits);
        if (close.peer != peer_behaviour::silent) close.connection.ws->send_binary(std::string(LARGE_MESSAGE_SIZE, 'x'));
    }
    // The writes are stuck by now
    std::this_thread::sleep_for(200ms);

    std::atomic<bool> done{false};
    std::vector<std::thread> peers;
    struct joined_at_the_end {
        std::atomic<bool>& done;
        std::vector<std::thread>& peers;
        ~joined_at_the_end() {
            done = true;
            for (auto& peer : peers) peer.join();
        }
    } joined{done, peers};
    for (auto& close : closes) peers.emplace_back([&close, &done] { close.run_peer(done); });

    for (auto& close : closes) {
        close.called = std::chrono::steady_clock::now();
        if (close.with_code) close.connection.ws->stop(close.code_asked(), close.reason_asked());
        else close.connection.ws->stop();
        if (close.sends_while_closing) {
            for (int i = 0; i < 3; ++i) close.connection.ws->send_text("while closing");
        }
    }

    // Told at the deadline: the code asked for if the frame got out whole, 1006 if not; never
    // 1013, nor after a write timeout that is later
    for (auto& close : closes) {
        INFO(close.name);
        auto left = close.called + asio::websocket::CLOSE_TIMEOUT + TOLERANCE - std::chrono::steady_clock::now();
        auto told = close.connection.closes->wait(std::chrono::duration_cast<std::chrono::milliseconds>(left) + 500ms);
        REQUIRE(told);
        check_at_close_deadline(close.connection.closes->first_at() - close.called);
        CHECK(told->code == (close.frame_gets_out() ? close.code_asked() : asio::websocket::CLOSE_ABNORMAL));
        CHECK(told->reason.empty());
        CHECK_FALSE(told->remote);
        CHECK_FALSE(close.connection.closes->off_executor());
        CHECK(close.connection.ws->queued_messages() == 0);
    }
    done = true;
    for (auto& peer : peers) peer.join();
    peers.clear();

    // What each peer read, and the connection ended for it: no frame behind one cut in the middle
    for (auto& close : closes) {
        INFO(close.name);
        if (close.frame_gets_out()) {
            REQUIRE(close.frame);
            CHECK(close.frame->close_code() == close.code_asked());
            CHECK(reason_of(*close.frame) == close.reason_asked());
        } else {
            CHECK_FALSE(read_close(*close.client));
        }
        CHECK(close.client->closed_within(5s));
        CHECK(close.connection.closes->count() == 1);
    }
}

TEST_CASE("WebSocket stop(code, reason): a peer that ends the connection without answering", "[websocket][close][hostile_peer][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    close_server fixture(kind);
    raw_ws_client client(fixture.endpoint());
    REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
    auto connection = fixture.next();

    auto told_soon = [&](std::chrono::steady_clock::time_point called) {
        auto told = connection.closes->wait(TOLERANCE);
        REQUIRE(told);
        CHECK_FALSE(told->remote);
        CHECK(told->reason.empty());
        CHECK(connection.closes->first_at() - called < asio::websocket::CLOSE_TIMEOUT);
        return told->code;
    };

    auto called = std::chrono::steady_clock::now();

    SECTION("Closing it, once the close frame is out: told the code sent") {
        connection.ws->stop(1008, "x");
        REQUIRE(read_close(client));
        client.close();
        CHECK(told_soon(called) == 1008);
    }

    SECTION("Resetting it, once the close frame is out: told the code sent") {
        connection.ws->stop(1008, "x");
        REQUIRE(read_close(client));
        client.reset();
        CHECK(told_soon(called) == 1008);
    }

    SECTION("Resetting it, with the close frame waiting behind a message: lost") {
        connection.ws->send_binary(std::string(LARGE_MESSAGE_SIZE, 'x'));
        std::this_thread::sleep_for(100ms);
        connection.ws->stop(1008, "x");
        std::this_thread::sleep_for(100ms);
        client.reset();
        CHECK(told_soon(called) == asio::websocket::CLOSE_ABNORMAL);
    }

    std::this_thread::sleep_for(20ms);
    CHECK(connection.closes->count() == 1);
}

TEST_CASE("WebSocket stop(code, reason): a write timeout while closing does not cut the handshake", "[websocket][close][hostile_peer][integration]") {
    close_server fixture;
    raw_ws_client client(fixture.endpoint());
    REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
    auto connection = fixture.next();
    connection.ws->set_write_limits({.write_timeout = 200ms});

    connection.ws->send_binary(std::string(LARGE_MESSAGE_SIZE, 'x'));
    connection.ws->stop(1008, "x");

    // The write timed out (and its watch of the backlog), still within the deadline of the close
    std::this_thread::sleep_for(1s);
    REQUIRE(connection.closes->count() == 0);

    // The peer reads again: the message, the close frame asked for, and the answer ends it
    auto message = client.read_frame(3s);
    REQUIRE(message);
    REQUIRE(message->opcode == OP_BINARY);
    REQUIRE(message->payload.size() == LARGE_MESSAGE_SIZE);
    auto frame = client.read_frame();
    REQUIRE(frame);
    REQUIRE(frame->opcode == OP_CLOSE);
    REQUIRE(frame->close_code() == 1008);
    REQUIRE(reason_of(*frame) == "x");
    client.send_frame(OP_CLOSE, close_payload(1008));
    auto told = connection.closes->wait();
    REQUIRE(told);
    REQUIRE(told->code == 1008);
    REQUIRE_FALSE(told->remote);
}

TEST_CASE("WebSocket stream: close_graceful() has one deadline, its turn to write included", "[websocket][close][hostile_peer][stream]") {
    boost::asio::io_context ioc;
    std::string written;
    auto silent_socket = std::make_shared<memory_socket>(ioc, [&](std::string data) { written += data; });
    auto stuck_socket = std::make_shared<gated_socket>(ioc, [](std::string) {});
    auto silent = std::make_shared<asio::websocket>(silent_socket);
    auto stuck = std::make_shared<asio::websocket>(stuck_socket);

    struct told_close {
        std::optional<asio::websocket_close> close;
        std::chrono::steady_clock::time_point at;
    };
    told_close silent_told, stuck_told;
    auto told = [&](told_close& record) {
        return [&](const asio::websocket_close& close) {
            record = {close, std::chrono::steady_clock::now()};
            if (silent_told.close && stuck_told.close) ioc.stop();
        };
    };
    silent->on_close(told(silent_told));
    stuck->on_close(told(stuck_told));

    auto called = std::chrono::steady_clock::now();
    // A close frame that waits for its turn behind a write that never ends
    co_spawn(ioc, [stuck]() -> awaitable<void> { co_await stuck->write_message("hi"); }, detached);
    co_spawn(ioc, stuck->close_graceful(1008, "x"), detached);
    // A second call while waiting for the answer starts nothing over, nor sends anything else
    co_spawn(ioc, silent->close_graceful(1008, "first"), detached);
    boost::asio::steady_timer later(ioc, 2s);
    later.async_wait([&](const boost::system::error_code&) {
        co_spawn(ioc, silent->close_graceful(1011, "second"), detached);
    });
    ioc.run_for(asio::websocket::CLOSE_TIMEOUT + TOLERANCE + 500ms);

    REQUIRE(silent_told.close);
    check_at_close_deadline(silent_told.at - called);
    CHECK(silent_told.close->code == 1008);
    CHECK_FALSE(silent->is_open());
    auto frame = take_frame(written);
    CHECK(frame.opcode == OP_CLOSE);
    CHECK(frame.payload == close_payload(1008, "first"));
    CHECK(written.empty());

    REQUIRE(stuck_told.close);
    check_at_close_deadline(stuck_told.at - called);
    CHECK(stuck_told.close->code == asio::websocket::CLOSE_ABNORMAL);
    CHECK_FALSE(stuck->is_open());
    CHECK(stuck->get_close_code() == asio::websocket::CLOSE_ABNORMAL);
}

TEST_CASE("WebSocket stream: what is sent once closed is not an overflow, the read loop not ended yet", "[websocket][close][dropped][stream]") {
    boost::asio::io_context ioc;
    auto socket = std::make_shared<gated_socket>(ioc, [](std::string) {});
    auto raw = std::make_shared<asio::websocket>(socket);
    auto ws = std::make_shared<http::websocket_connection>(raw);

    uint64_t dropped_open = 0;
    uint64_t dropped_closed = 0;
    boost::asio::post(ioc, [&] {
        ws->set_write_limits({.max_messages = 2});
        // the first one stuck being written, the queue full: the third one is dropped
        for (int i = 0; i < 3; ++i) ws->send_text("message");
        dropped_open = ws->dropped_messages();
        // closed under the connection, as the keepalive does: nothing ended the queue yet
        raw->close(asio::websocket::CLOSE_GOING_AWAY);
        for (int i = 0; i < 5; ++i) ws->send_text("message");
        dropped_closed = ws->dropped_messages();
        socket->gate.set();
    });
    ioc.run_for(100ms);

    CHECK(dropped_open == 1);
    CHECK(dropped_closed == 1);
    CHECK(ws->queued_messages() == 0);
}

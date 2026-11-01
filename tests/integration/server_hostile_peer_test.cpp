#include "server_test_helpers.hpp"
#include "../fixtures/hostile_peer.hpp"
#include "../fixtures/listener_helpers.hpp"
#include "../fixtures/stream_test_helpers.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/reverse_proxy.hpp>
#include <thinger/http/server/server_connection.hpp>
#include <thinger/http/server/sse_connection.hpp>
#include <thinger/asio/sockets/tcp_socket.hpp>
#include <thinger/http/server/websocket_connection.hpp>
#include <thinger/http/client/executor_client.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <thread>

// Every server wait on a client ends within its deadline when the client is hostile (see
// hostile_client in hostile_peer.hpp): the server-role counterpart of hostile_peer_test.cpp, one
// case per wait, on every transport. Each case asserts the outcome (a 408, or a close) and when
// it arrives: no earlier than the deadline minus 100 ms, no later than the deadline plus the
// tolerance (1 s). The client gives up well past it, so a server that never acts fails the case
// instead of hanging it. The event streams and WebSockets come next: a client that stops reading
// them is closed at their write deadline, one that reads slowly but steadily never is. The proxy
// tunnels come last: one with nothing forwarded either way is closed at its idle deadline, one
// with any traffic never is.

using namespace thinger;
using namespace thinger::http::test;
using namespace std::chrono_literals;
using namespace server_test;

namespace {

    using clock_type = std::chrono::steady_clock;

    constexpr auto EARLY = 100ms;
    constexpr auto TOLERANCE = 1s;
    // Where a hostile client gives up: past the deadline and its tolerance
    constexpr auto GIVE_UP = 2s;

    std::string lowercase(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
        return text;
    }

    // The head of the response the client got (none: empty)
    std::string head_of(const hostile_outcome& outcome) {
        return outcome.data.substr(0, outcome.data.find("\r\n\r\n"));
    }

    // The outcome came at the deadline: not before it (minus 100 ms), within the tolerance
    void check_at_deadline(clock_type::duration elapsed, clock_type::duration deadline,
                           clock_type::duration tolerance = TOLERANCE) {
        INFO("elapsed " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms");
        CHECK(elapsed >= deadline - EARLY);
        CHECK(elapsed <= deadline + tolerance);
    }

    template<typename Condition>
    bool eventually(Condition condition, clock_type::duration timeout) {
        auto deadline = clock_type::now() + timeout;
        while (!condition()) {
            if (clock_type::now() > deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    // A head announcing a body far longer than a trickling client ever sends
    const std::string upload_head = "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 1000000\r\n\r\n";

    // A response far larger than the buffers of a connection (on every transport)
    constexpr size_t large_size = 16 << 20;

    // A server with `options`, counting the requests its routes handle: GET /, GET /large, which
    // answers large_size bytes, and POST /upload, which reads its body to its end before answering
    struct deadline_server {
        http::server server;
        std::atomic<int> handled{0};
        std::optional<running_server> running;

        deadline_server(transport kind, const http::connection_options& options) {
            server.set_connection_options(options);
            server.get("/", [this](http::response& res) {
                ++handled;
                res.send("ok");
            });
            server.get("/large", [this](http::response& res) {
                ++handled;
                res.send(std::string(large_size, 'x'));
            });
            server.post("/upload", [this](http::request& req, http::response& res) -> thinger::awaitable<void> {
                if (!co_await req.read_body()) co_return;
                ++handled;
                res.send("ok");
            });
            running.emplace(server, kind);
        }

        const transport_endpoint& endpoint() const { return running->endpoint(); }
    };

}

// Slowloris without a byte: a connection that sends nothing holds the server until its header
// deadline, which counts from the start of serving it (after the TLS handshake)
TEST_CASE("A silent client is closed at the header deadline, without a response",
          "[server][hostile_peer][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::connection_options options;
    options.header_timeout = 1s;
    deadline_server fixture(kind, options);

    hostile_client client(fixture.endpoint());
    auto outcome = client.stay_silent(1s + TOLERANCE + GIVE_UP);
    INFO("received: " << outcome.data);
    REQUIRE(outcome.closed);
    CHECK(outcome.data.empty());
    check_at_deadline(outcome.elapsed, 1s);
    CHECK(fixture.handled == 0);
}

// Slowloris: a head trickled one byte per 200 ms, each byte moving the idle deadline, is answered
// 408 at its header deadline, whatever its pace, and its connection closed
TEST_CASE("A trickled request head is answered 408 at the header deadline",
          "[server][hostile_peer][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::connection_options options;
    options.header_timeout = 1s;
    deadline_server fixture(kind, options);

    hostile_client client(fixture.endpoint(), 200ms);
    auto outcome = client.trickle_head(1s + TOLERANCE + GIVE_UP);
    INFO("received: " << outcome.data << " (" << outcome.trickled << " bytes trickled)");
    REQUIRE(outcome.closed);
    REQUIRE(outcome.answered_with("HTTP/1.1 408"));
    CHECK(lowercase(head_of(outcome)).find("\r\nconnection: close") != std::string::npos);
    check_at_deadline(outcome.answered, 1s);
    check_at_deadline(outcome.elapsed, 1s);
    // It kept trickling until then
    CHECK(outcome.trickled >= 3);
    CHECK(fixture.handled == 0);
}

// Slow POST: a whole head, then a body trickled one byte per 200 ms, each byte moving the idle
// deadline, is answered 408 at the request deadline, whatever its pace. The connection then closes
// gracefully, as the body may still come: within the linger bounds
TEST_CASE("A trickled request body is answered 408 at the request deadline",
          "[server][hostile_peer][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::connection_options options;
    options.request_timeout = 2s;
    options.linger_timeout = 500ms;
    deadline_server fixture(kind, options);

    hostile_client client(fixture.endpoint(), 200ms);
    auto outcome = client.trickle_body(upload_head, 2s + 500ms + TOLERANCE + GIVE_UP);
    INFO("received: " << outcome.data << " (" << outcome.trickled << " bytes trickled)");
    REQUIRE(outcome.closed);
    REQUIRE(outcome.answered_with("HTTP/1.1 408"));
    CHECK(lowercase(head_of(outcome)).find("\r\nconnection: close") != std::string::npos);
    check_at_deadline(outcome.answered, 2s);
    check_at_deadline(outcome.elapsed, 2s, 500ms + TOLERANCE);
    // It kept trickling until then
    CHECK(outcome.trickled >= 8);
    CHECK(fixture.handled == 0);
}

// A client that stops reading: a response it takes no byte of for the write deadline closes its
// connection, whether it was to be kept alive or closed after the response (which stops reading
// the connection). The server first fills the buffers of the connection: a second more. The
// client sees the close once it reads, before the whole response
TEST_CASE("A response the client stops reading is closed at the write deadline",
          "[server][hostile_peer][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::connection_options options;
    options.write_timeout = 1s;
    deadline_server fixture(kind, options);
    std::string request = "GET /large HTTP/1.1\r\nHost: x\r\n\r\n";
    SECTION("Kept alive") {}
    SECTION("Closed after the response") {
        request = "GET /large HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    }

    hostile_client client(fixture.endpoint());
    REQUIRE(client.send_without_reading(request));
    auto start = clock_type::now();
    REQUIRE(eventually([&] { return fixture.handled == 1; }, GIVE_UP));
    bool closed = eventually([&] { return fixture.server.stats().http_connections == 0; },
                             1s + 1s + TOLERANCE + GIVE_UP);
    auto elapsed = clock_type::now() - start;
    REQUIRE(closed);
    check_at_deadline(elapsed, 1s, 1s + TOLERANCE);

    auto outcome = client.read_until_closed(5s);
    INFO("received " << outcome.data.size() << " bytes");
    REQUIRE(outcome.closed);
    CHECK(outcome.answered_with("HTTP/1.1 200"));
    CHECK(outcome.data.size() < large_size);
}

namespace {

    using sse_ptr = std::shared_ptr<http::sse_connection>;
    using ws_ptr = std::shared_ptr<http::websocket_connection>;

    const std::string sse_request = "GET /events HTTP/1.1\r\nHost: localhost\r\nAccept: text/event-stream\r\n\r\n";
    const std::string ws_request = "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                   "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

    // The write deadline of the push cases, and what a slow client reads per period: 640 KiB/s,
    // taken every 100 ms. The client taking bytes from the socket is watched once the socket
    // took nothing for the deadline, for a quarter of it (250 ms), and over loopback its TCP
    // opens its window again about once per segment (64 KiB): a few times within that quarter
    constexpr auto WRITE_TIMEOUT = 1s;
    constexpr auto SLOW_PERIOD = 100ms;
    // A stalled write closes once its socket took nothing for the deadline, and its client
    // nothing from the socket for a quarter of it more
    constexpr auto STALL_BOUND = WRITE_TIMEOUT + WRITE_TIMEOUT / 4;
    constexpr size_t SLOW_READ = 64 << 10;

    // The receive buffer of a client that reads nothing (or stops reading) while events keep
    // coming, and the send buffer of its server connection: the system would grow both on a local
    // TCP connection to several MiB, which the flood (1.6 MiB/s) takes longer than the second
    // allowed to fill. The send buffer grows to its maximum (tcp_wmem, 4 MiB) once earlier
    // connections on the host left their metrics, so the client's alone does not bound it
    constexpr int NO_READ_BUFFER = 64 << 10;

    // The receive buffer of a slow client that keeps reading: grown by the system to several MiB,
    // the client's TCP would open its window again only once a sixteenth of it is free (receiver
    // silly-window avoidance), at SLOW_READ per period more than the second allowed, so the
    // server's write would see no progress that long although the client never stopped reading
    constexpr int SLOW_READ_BUFFER = 256 << 10;

    // A time marked on one thread, read on another
    struct moment {
        std::atomic<clock_type::rep> ticks{0};

        void mark() { ticks = clock_type::now().time_since_epoch().count(); }
        bool marked() const { return ticks != 0; }
        clock_type::time_point get() const { return clock_type::time_point(clock_type::duration(ticks.load())); }
    };

    std::chrono::milliseconds millis(clock_type::duration duration) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    }

    // Data of `size` bytes, its index first
    std::string indexed_data(size_t index, size_t size) {
        auto data = std::to_string(index) + ":";
        data.resize(size, 'x');
        return data;
    }

    std::string data_event(const std::string& data) { return "data: " + data + "\n\n"; }

    // What follows the head of a response (nothing yet: empty)
    std::string body_of(const std::string& data) {
        auto end = data.find("\r\n\r\n");
        return end == std::string::npos ? std::string{} : data.substr(end + 4);
    }

    constexpr std::string_view keepalive_comment = ": keep-alive\n";

    // An event stream without its keep-alive comments (written when nothing else is, e.g. after
    // a long event)
    std::string without_keepalives(std::string stream) {
        for (auto at = stream.find(keepalive_comment); at != std::string::npos; at = stream.find(keepalive_comment, at)) {
            stream.erase(at, keepalive_comment.size());
        }
        return stream;
    }

    // The write limits by default, with the write deadline of the push cases
    http::write_limits push_limits() {
        http::write_limits limits;
        limits.write_timeout = WRITE_TIMEOUT;
        return limits;
    }

    // A server whose GET /events streams what `stream` sends, spawned on the connection's
    // executor, with `limits` and `options`, and over TCP and TLS a `send_buffer` (bytes, 0: the
    // system's) set on its connection before the stream starts; what its connection lived
    // through, told from its thread: how many times on_close() ran, and when it first did
    struct sse_server {
        using stream_type = std::function<awaitable<void>(sse_ptr)>;

        http::server server;
        stream_type stream;
        std::promise<sse_ptr> created;
        std::atomic<int> closes{0};
        moment closed;
        std::optional<running_server> running;

        sse_server(transport kind, stream_type producer, http::write_limits limits = push_limits(),
                   http::sse_options options = {}, int send_buffer = 0)
            : stream(std::move(producer)) {
            server.get("/events", [this, limits, options, send_buffer](http::request&, http::response& res) {
                auto connection = res.get_connection();
                if (send_buffer > 0 && connection) {
                    // (an ssl_socket is one too, over the same descriptor)
                    if (auto* tcp = dynamic_cast<thinger::asio::tcp_socket*>(connection->get_socket().get())) {
                        tcp->get_socket().set_option(boost::asio::socket_base::send_buffer_size(send_buffer));
                    }
                }
                res.start_sse([this, limits](sse_ptr sse) {
                    sse->set_write_limits(limits);
                    sse->on_close([this] {
                        if (!closed.marked()) closed.mark();
                        ++closes;
                    });
                    co_spawn(sse->get_executor(), stream(sse), detached);
                    created.set_value(sse);
                }, options);
            });
            running.emplace(server, kind);
        }

        const transport_endpoint& endpoint() const { return running->endpoint(); }

        // Whether the connection closed within `timeout`
        bool closes_within(clock_type::duration timeout) const {
            return eventually([&] { return closed.marked(); }, timeout);
        }
    };

    // Events of 16 KiB, one per 10 ms (from the start, so a timer late on a loaded host does not
    // slow the stream), sent without waiting, until the connection closes. When a
    // write last finished, in `written`, seen within 10 ms: the queue holds fewer events than
    // after the last send, and the connection still open (a close empties the queue, writing
    // nothing). No earlier than the last progress of the connection's writes
    awaitable<void> flood(sse_ptr sse, moment& written) {
        boost::asio::steady_timer timer(sse->get_executor());
        auto start = clock_type::now();
        size_t after = 0;
        for (size_t i = 0;; ++i) {
            // (on the executor of the connection, as its close: seen closed if that emptied it)
            auto finished = sse->queued_messages() < after;
            // (no watermarks: false only once closed)
            if (!co_await sse->wait_writable()) co_return;
            if (finished) written.mark();
            // (queued, or dropped, at once)
            sse->send_data(indexed_data(i, 16 << 10));
            after = sse->queued_messages();
            timer.expires_at(start + (i + 1) * 10ms);
            co_await timer.async_wait(use_nothrow_awaitable);
        }
    }

    // Read slowly, SLOW_READ per period, until the stream after the head holds `size` bytes, the
    // server closes or `limit`
    hostile_outcome& read_slowly(hostile_client& client, hostile_outcome& outcome, size_t size,
                                 std::chrono::milliseconds limit) {
        return client.read_at_rate(outcome, SLOW_READ, limit, [size](const std::string& data) {
            auto end = data.find("\r\n\r\n");
            return end != std::string::npos && data.size() - (end + 4) >= size;
        });
    }

    // Never enough: read until the limit
    constexpr auto forever = [](const std::string&) { return false; };

}

// No-read on an event stream: events keep coming while the client reads nothing, so its write
// stalls once the buffers of the connection are full (a second at most, both buffers bounded).
// The connection closes at the write deadline after its last progress, on_close() runs once, the
// close is logged, and the client sees it once it reads
TEST_CASE("An SSE client that stops reading is closed at the write deadline",
          "[sse][server][hostile_peer][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    log_capture logs;
    moment written;
    sse_server fixture(kind, [&written](sse_ptr sse) { return flood(sse, written); }, push_limits(), {},
                       NO_READ_BUFFER);

    hostile_client client(fixture.endpoint(), 200ms, NO_READ_BUFFER);
    auto sent = clock_type::now();
    REQUIRE(client.send_without_reading(sse_request));
    REQUIRE(fixture.closes_within(STALL_BOUND + 1s + TOLERANCE + GIVE_UP));
    auto closed = fixture.closed.get();
    // no event written whole: the first one stalled
    auto progress = written.marked() ? written.get() : sent;
    INFO("closed " << millis(closed - progress).count() << " ms after the last event written, "
                   << millis(closed - sent).count() << " ms after the request");
    CHECK(closed - progress >= WRITE_TIMEOUT - EARLY);
    CHECK(closed - sent <= STALL_BOUND + 1s + TOLERANCE);
    CHECK(logs.logged("warning", "closing sse connection: its write timed out"));

    auto outcome = client.read_until_closed(10s);
    REQUIRE(outcome.closed);
    CHECK(outcome.answered_with("HTTP/1.1 200"));
    // once
    std::this_thread::sleep_for(200ms);
    CHECK(fixture.closes == 1);
    CHECK(logs.count("warning", "closing sse connection: its write timed out") == 1);
}

// A slow client that keeps reading: the write deadline bounds a stall, not the time to write the
// stream or one event, so a client taking 640 KiB/s gets every event awaited, in order, however
// long that takes (3 MiB of small events, about 5 s; one event of 8 MiB, about 13 s)
TEST_CASE("A slow SSE client that keeps reading is not cut by the write deadline",
          "[sse][server][hostile_peer][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    std::string expected;
    sse_server::stream_type stream;
    SECTION("200 events of 16 KiB") {
        for (size_t i = 0; i < 200; ++i) expected += data_event(indexed_data(i, 16 << 10));
        stream = [](sse_ptr sse) -> awaitable<void> {
            for (size_t i = 0; i < 200; ++i) {
                if (!co_await sse->send_async({.data = indexed_data(i, 16 << 10)})) co_return;
            }
        };
    }
    SECTION("One event of 8 MiB") {
        expected = data_event(indexed_data(0, 8 << 20));
        stream = [](sse_ptr sse) -> awaitable<void> {
            co_await sse->send_async({.data = indexed_data(0, 8 << 20)});
        };
    }
    log_capture logs;
    sse_server fixture(kind, stream);

    hostile_client client(fixture.endpoint(), SLOW_PERIOD, SLOW_READ_BUFFER);
    auto start = clock_type::now();
    REQUIRE(client.send_without_reading(sse_request));
    hostile_outcome outcome;
    read_slowly(client, outcome, expected.size(), 60s);
    auto took = clock_type::now() - start;
    // (the transport too: the INFO of MATRIX_TRANSPORT is not shown past the sections)
    INFO(kind << ": received " << outcome.data.size() << " bytes in " << millis(took).count() << " ms, "
              << logs.count("warning", "its write timed out") << " write timeouts logged, closed by the server at "
              << (fixture.closed.marked() ? millis(fixture.closed.get() - start).count() : -1) << " ms");
    REQUIRE_FALSE(outcome.closed);
    CHECK(outcome.answered_with("HTTP/1.1 200"));
    CHECK(without_keepalives(body_of(outcome.data)) == expected);
    CHECK(took > 3 * WRITE_TIMEOUT);
    CHECK(fixture.closes == 0);
}

// A slow client that then stops reading for good: closed within the write deadline and a quarter
// of its last read, as one that never read (both buffers bounded as there: grown by the system, the
// flood would still fill them for more than the second allowed after the last read)
TEST_CASE("A slow SSE client that stops reading is closed at the write deadline after its last read",
          "[sse][server][hostile_peer][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    moment written;
    sse_server fixture(kind, [&written](sse_ptr sse) { return flood(sse, written); }, push_limits(), {},
                       NO_READ_BUFFER);

    hostile_client client(fixture.endpoint(), SLOW_PERIOD, NO_READ_BUFFER);
    REQUIRE(client.send_without_reading(sse_request));
    hostile_outcome outcome;
    client.read_at_rate(outcome, SLOW_READ, 3 * WRITE_TIMEOUT, forever);
    REQUIRE_FALSE(outcome.closed);
    CHECK(fixture.closes == 0);
    auto last_read = client.last_read();

    REQUIRE(fixture.closes_within(STALL_BOUND + 1s + TOLERANCE + GIVE_UP));
    auto closed = fixture.closed.get() - last_read;
    INFO("closed " << millis(closed).count() << " ms after the last read");
    // what it took before the socket's deadline is not seen (it is watched from then): never
    // closed before the watch after it
    CHECK(closed >= WRITE_TIMEOUT / 4 - EARLY);
    CHECK(closed <= STALL_BOUND + 1s + TOLERANCE);
    REQUIRE(client.read_until_closed(10s).closed);
}

// The keep-alive neither hides a stalled write nor puts its deadline off: due every 200 ms while
// one event of 16 MiB is stalled, none is written, and the connection closes at the write
// deadline. On a quiet stream a client reading gets one every 200 ms, written as any event, and
// the connection stays open
TEST_CASE("The SSE keep-alive does not hide a stalled write",
          "[sse][server][hostile_peer][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::sse_options options{.keepalive = 200ms};

    SECTION("A stalled write closes at its deadline") {
        sse_server fixture(kind, [](sse_ptr sse) -> awaitable<void> {
            sse->send_data(std::string(16 << 20, 'x'));
            co_return;
        }, push_limits(), options);
        hostile_client client(fixture.endpoint());
        auto sent = clock_type::now();
        REQUIRE(client.send_without_reading(sse_request));
        REQUIRE(fixture.closes_within(STALL_BOUND + 1s + TOLERANCE + GIVE_UP));
        auto closed = fixture.closed.get() - sent;
        INFO("closed " << millis(closed).count() << " ms after the request");
        CHECK(closed >= WRITE_TIMEOUT - EARLY);
        CHECK(closed <= STALL_BOUND + 1s + TOLERANCE);
        auto outcome = client.read_until_closed(10s);
        REQUIRE(outcome.closed);
        CHECK(outcome.data.find(": keep-alive") == std::string::npos);
        CHECK(fixture.closes == 1);
    }

    SECTION("A quiet stream gets its keep-alives and stays open") {
        sse_server fixture(kind, [](sse_ptr) -> awaitable<void> { co_return; }, push_limits(), options);
        hostile_client client(fixture.endpoint());
        REQUIRE(client.send_without_reading(sse_request));
        hostile_outcome outcome;
        client.read_at_rate(outcome, SLOW_READ, 3 * WRITE_TIMEOUT, forever);
        auto keepalives = server_test::count(body_of(outcome.data), std::string(keepalive_comment));
        INFO("received: " << outcome.data);
        REQUIRE_FALSE(outcome.closed);
        // nothing else
        CHECK(without_keepalives(body_of(outcome.data)).empty());
        CHECK(keepalives >= 10);
        CHECK(keepalives <= 16);
        CHECK(fixture.closes == 0);
    }
}

namespace {

    // A server whose GET /ws upgrades to a WebSocket with the write limits of the push cases,
    // which sends one binary message of `size` bytes and awaits it: when that send ended and
    // whether it was written, how many times on_close() ran and the code it told
    struct ws_server {
        http::server server;
        std::atomic<int> closes{0};
        std::atomic<int> close_code{0};
        moment sent;
        std::atomic<bool> written{false};
        std::optional<running_server> running;

        ws_server(transport kind, size_t size) {
            server.get("/ws", [this, size](http::request&, http::response& res) {
                res.upgrade_websocket({}, [this, size](ws_ptr ws) {
                    ws->set_write_limits(push_limits());
                    ws->on_close([this](const asio::websocket_close& close) {
                        close_code = close.code;
                        ++closes;
                    });
                    co_spawn(ws->get_executor(), [this, ws, size]() -> awaitable<void> {
                        written = co_await ws->send_binary_async(std::string(size, 'y'));
                        sent.mark();
                    }, detached);
                });
            });
            running.emplace(server, kind);
        }

        const transport_endpoint& endpoint() const { return running->endpoint(); }
    };

}

// No-read on a WebSocket: a message of 16 MiB the client takes nothing of stalls once the buffers
// are full, and its send fails at the write deadline: the WebSocket closes with 1013 (Try Again
// Later). The client, reading then, gets the rest of the frame and the close frame; over TLS
// the deadline cut the TLS record being written, after which nothing can be sent: it may only
// see the connection end
TEST_CASE("A WebSocket client that stops reading is closed with 1013 at the write deadline",
          "[websocket][server][hostile_peer][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    ws_server fixture(kind, 16 << 20);

    stream_test::raw_ws_client client(fixture.endpoint());
    auto start = clock_type::now();
    REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
    REQUIRE(eventually([&] { return fixture.sent.marked(); }, STALL_BOUND + 1s + TOLERANCE + GIVE_UP));
    auto failed = fixture.sent.get() - start;
    INFO("send failed " << millis(failed).count() << " ms after the handshake");
    CHECK_FALSE(fixture.written);
    CHECK(failed >= WRITE_TIMEOUT - EARLY);
    CHECK(failed <= STALL_BOUND + 1s + TOLERANCE);

    REQUIRE(eventually([&] { return fixture.closes == 1; }, 5s));
    CHECK(fixture.close_code == 1013);

    std::optional<stream_test::ws_frame> frame;
    while ((frame = client.read_frame(10s)) && frame->opcode != stream_test::OP_CLOSE) {}
    if (kind == transport::tls && !frame) {
        CHECK(client.eof());
    } else {
        REQUIRE(frame);
        CHECK(frame->close_code() == 1013);
    }
    CHECK(fixture.closes == 1);
}

// A slow WebSocket client that keeps reading: one message of 4 MiB, far more than the client
// takes within the write deadline (640 KiB), is written whole, in about 6 s, and nothing closes
TEST_CASE("A slow WebSocket client that keeps reading is not cut by the write deadline",
          "[websocket][server][hostile_peer][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    constexpr size_t size = 4 << 20;
    ws_server fixture(kind, size);

    hostile_client client(fixture.endpoint(), SLOW_PERIOD, SLOW_READ_BUFFER);
    auto start = clock_type::now();
    REQUIRE(client.send_without_reading(ws_request));
    // the binary frame: its header (a 64-bit length) and the payload
    hostile_outcome outcome;
    read_slowly(client, outcome, 10 + size, 30s);
    auto took = clock_type::now() - start;
    INFO("received " << outcome.data.size() << " bytes in " << millis(took).count() << " ms");
    REQUIRE_FALSE(outcome.closed);
    CHECK(outcome.answered_with("HTTP/1.1 101"));
    auto frame = body_of(outcome.data);
    REQUIRE(frame.size() == 10 + size);
    CHECK(frame.substr(0, 2) == "\x82\x7f");
    CHECK(frame.find_first_not_of('y', 10) == std::string::npos);
    CHECK(took > 3 * WRITE_TIMEOUT);
    CHECK(eventually([&] { return fixture.sent.marked(); }, 2s));
    CHECK(fixture.written);
    CHECK(fixture.closes == 0);
}

namespace {

    // The tunnel deadline of the proxy cases, and the period of their trickles
    constexpr auto TUNNEL_TIMEOUT = 1s;
    constexpr auto TRICKLE = 300ms;
    // How long the WebSocket of the ping case sends nothing: four tunnel deadlines
    constexpr auto QUIET = 4s;

    std::string upgrade_request(const std::string& path) {
        return "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nUpgrade: echo\r\nConnection: Upgrade\r\n\r\n";
    }

    // Send back what is read, until the connection ends
    awaitable<void> echo_until_closed(std::shared_ptr<asio::socket> socket, std::string buffered, moment& ended) {
        bool open = true;
        if (!buffered.empty()) {
            auto [ec, written] = co_await socket->write(buffered);
            open = !ec;
        }
        uint8_t buffer[8192];
        while (open) {
            auto [ec, bytes] = co_await socket->read_some(buffer, sizeof(buffer));
            if (ec || bytes == 0) break;
            auto [write_ec, written] = co_await socket->write(buffer, bytes);
            open = !write_ec;
        }
        ended.mark();
    }

    // Write a letter (a to z, then again) per TRICKLE, the first one after a TRICKLE, reading
    // nothing, until the connection ends
    awaitable<void> talk_until_closed(std::shared_ptr<asio::socket> socket) {
        boost::asio::steady_timer timer(socket->get_io_context());
        for (size_t i = 0;; ++i) {
            timer.expires_after(TRICKLE);
            co_await timer.async_wait(use_nothrow_awaitable);
            char letter = static_cast<char>('a' + i % 26);
            auto [ec, written] = co_await socket->write(std::string_view(&letter, 1));
            if (ec) co_return;
        }
    }

    // An upgrade target, over TCP: GET /echo answers 101, then sends back what it reads (silent
    // until then); GET /talk answers 101, then writes a letter per TRICKLE, reading nothing.
    // When the session of an echo ended: its connection from the proxy ended
    struct tunnel_target {
        moment ended;
        http::server server;
        std::optional<running_server> running;

        tunnel_target() {
            server.get("/echo", [this](http::response& res) {
                switch_protocols(res);
                res.take_over([this](http::upgraded_connection conn) {
                    co_spawn(conn.socket->get_io_context(),
                             echo_until_closed(conn.socket, std::move(conn.buffered), ended), detached);
                });
            });
            server.get("/talk", [](http::response& res) {
                switch_protocols(res);
                res.take_over([](http::upgraded_connection conn) {
                    co_spawn(conn.socket->get_io_context(), talk_until_closed(conn.socket), detached);
                });
            });
            running.emplace(server);
        }

        static void switch_protocols(http::response& res) {
            res.status(http::http_response::status::switching_protocols);
            res.header("Upgrade", "echo");
            res.header("Connection", "Upgrade");
        }
    };

    http::proxy_options with_tunnel_timeout(std::chrono::milliseconds timeout) {
        http::proxy_options options;
        options.tunnel_idle_timeout = timeout;
        return options;
    }

    // A proxy listening on a transport of the matrix, forwarding every request to the target on
    // `port` (over TCP), with the options of the proxy `defaults`, or those of each forward()
    // call `per_call`; and the tunnels it holds
    struct tunnel_proxy {
        http::reverse_proxy proxy;
        http::server server;
        std::optional<running_server> running;

        tunnel_proxy(transport kind, uint16_t port, http::proxy_options defaults,
                     std::optional<http::proxy_options> per_call = std::nullopt)
            : proxy(std::move(defaults)) {
            auto target = http::proxy_target::from_url("http://127.0.0.1:" + std::to_string(port));
            REQUIRE(target);
            server.set_not_found_handler([this, target = *target, per_call](http::request& req,
                                                                            http::response& res) -> awaitable<void> {
                if (per_call) {
                    co_await proxy.forward(req, res, target, *per_call);
                } else {
                    co_await proxy.forward(req, res, target);
                }
            });
            running.emplace(server, kind);
        }

        const transport_endpoint& endpoint() const { return running->endpoint(); }

        uint64_t tunnels() { return server.stats().tunnel_connections; }
    };

    // A client keeping silent once its tunnel to the echo of `target` is open sees the proxy
    // close it at the tunnel deadline (from the 101), and so does the target
    void check_silent_tunnel_closed(tunnel_proxy& proxy, tunnel_target& target) {
        hostile_client client(proxy.endpoint());
        auto answer = client.upgrade(upgrade_request("/echo"));
        REQUIRE(answer.answered_with("HTTP/1.1 101"));
        auto opened = clock_type::now();
        auto outcome = client.read_until_closed(TUNNEL_TIMEOUT + TOLERANCE + GIVE_UP);
        INFO("received: " << outcome.data);
        REQUIRE(outcome.closed);
        CHECK(outcome.data.empty());
        check_at_deadline(outcome.elapsed, TUNNEL_TIMEOUT);
        REQUIRE(eventually([&] { return target.ended.marked(); }, GIVE_UP));
        check_at_deadline(target.ended.get() - opened, TUNNEL_TIMEOUT);
        CHECK(eventually([&] { return proxy.tunnels() == 0; }, GIVE_UP));
    }

}

// A silent tunnel: neither the client nor the target sends anything once the 101 is through, so
// nothing is forwarded, and the proxy closes both its connections at the tunnel deadline
TEST_CASE("A silent proxy tunnel is closed at its idle deadline",
          "[proxy][server][hostile_peer][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    tunnel_target target;
    tunnel_proxy proxy(kind, target.running->port, with_tunnel_timeout(TUNNEL_TIMEOUT));
    check_silent_tunnel_closed(proxy, target);
}

// No-read through a tunnel: a client sending 64 KiB after 64 KiB without reading the echo fills
// the buffers both ways (a second at most), then nothing can be forwarded: the tunnel is idle,
// and closed at its deadline after the last write of the client that went through
TEST_CASE("A proxy tunnel whose client does not read is closed at its idle deadline",
          "[proxy][server][hostile_peer][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    tunnel_target target;
    tunnel_proxy proxy(kind, target.running->port, with_tunnel_timeout(TUNNEL_TIMEOUT));

    hostile_client client(proxy.endpoint());
    REQUIRE(client.upgrade(upgrade_request("/echo")).answered_with("HTTP/1.1 101"));
    auto outcome = client.flood(std::string(64 << 10, 'z'), 1s + TUNNEL_TIMEOUT + TOLERANCE + GIVE_UP);
    auto closed = clock_type::now();
    auto last_write = client.last_write();
    INFO("closed " << millis(closed - last_write).count() << " ms after the last write, "
                   << millis(outcome.elapsed).count() << " ms after the 101");
    REQUIRE(outcome.closed);
    CHECK(outcome.elapsed >= TUNNEL_TIMEOUT - EARLY);
    CHECK(closed - last_write <= 1s + TUNNEL_TIMEOUT + TOLERANCE);
    REQUIRE(eventually([&] { return target.ended.marked(); }, GIVE_UP));
    CHECK(target.ended.get() - last_write <= 1s + TUNNEL_TIMEOUT + TOLERANCE);
    CHECK(eventually([&] { return proxy.tunnels() == 0; }, GIVE_UP));
    CHECK(client.read_until_closed(10s).closed);
}

// Traffic keeps a tunnel open, at a pace slower than its deadline would allow without it: a byte
// every 300 ms, for 4 s (four deadlines), echoed in order; or sent by the target only, the client
// reading and never writing
TEST_CASE("A proxy tunnel with traffic stays open",
          "[proxy][server][hostile_peer][deadline][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    tunnel_target target;
    tunnel_proxy proxy(kind, target.running->port, with_tunnel_timeout(TUNNEL_TIMEOUT));
    hostile_client client(proxy.endpoint(), TRICKLE);
    const std::string letters = "abcdefghijklm";

    SECTION("A trickle both ways") {
        auto answer = client.upgrade(upgrade_request("/echo"));
        REQUIRE(answer.answered_with("HTTP/1.1 101"));
        auto outcome = client.trickle_data(letters);
        INFO(kind << ": received " << outcome.data << " in " << millis(outcome.elapsed).count() << " ms");
        REQUIRE_FALSE(outcome.closed);
        CHECK(outcome.trickled == letters.size());
        CHECK(body_of(answer.data) + outcome.data == letters);
    }

    SECTION("A trickle from the target only") {
        auto answer = client.upgrade(upgrade_request("/talk"));
        REQUIRE(answer.answered_with("HTTP/1.1 101"));
        auto outcome = client.read_until_closed(letters.size() * TRICKLE);
        auto data = body_of(answer.data) + outcome.data;
        INFO(kind << ": received " << data);
        REQUIRE_FALSE(outcome.closed);
        // a letter per 300 ms, in order
        CHECK(data.size() >= letters.size() - 2);
        CHECK(letters.starts_with(data));
    }
    CHECK(proxy.tunnels() == 1);
    CHECK_FALSE(target.ended.marked());
}

// The tunnel deadline is its own: not turned off by idle_timeout = 0 (the exchange deadline),
// nor set by it; 0 turns it off; and the options of a forward() call set it as the others
TEST_CASE("The proxy tunnel deadline is its own, set per call, and 0 turns it off",
          "[proxy][server][hostile_peer][deadline][integration]") {
    http::proxy_options defaults;
    CHECK(defaults.tunnel_idle_timeout == 120s);
    CHECK(defaults.idle_timeout == 60s);
    tunnel_target target;

    SECTION("idle_timeout = 0 does not turn it off") {
        auto options = with_tunnel_timeout(TUNNEL_TIMEOUT);
        options.idle_timeout = 0ms;
        tunnel_proxy proxy(transport::tcp, target.running->port, options);
        check_silent_tunnel_closed(proxy, target);
    }

    SECTION("The options of a forward() call") {
        tunnel_proxy proxy(transport::tcp, target.running->port, defaults, with_tunnel_timeout(TUNNEL_TIMEOUT));
        check_silent_tunnel_closed(proxy, target);
    }

    SECTION("0 keeps a silent tunnel open, with a short idle_timeout too") {
        auto options = with_tunnel_timeout(0ms);
        options.idle_timeout = 500ms;
        tunnel_proxy proxy(transport::tcp, target.running->port, options);
        hostile_client client(proxy.endpoint());
        REQUIRE(client.upgrade(upgrade_request("/echo")).answered_with("HTTP/1.1 101"));
        auto outcome = client.read_until_closed(3s);
        REQUIRE_FALSE(outcome.closed);
        CHECK(outcome.data.empty());
        // both ways still
        REQUIRE(client.connection().write("ping"));
        std::string echo;
        CHECK(client.connection().read_until(echo, "ping", 2s));
        CHECK(proxy.tunnels() == 1);
        CHECK_FALSE(target.ended.marked());
    }
}

// A quiet WebSocket behind the proxy: its target pings every 300 ms and the client answers, which
// keeps the tunnel open for 4 s (four deadlines) with no message sent; a message then gets its echo
TEST_CASE("WebSocket pings alone keep a proxy tunnel open",
          "[proxy][websocket][server][hostile_peer][deadline][integration]") {
    http::server target;
    target.get("/ws", [](http::response& res) {
        http::websocket_accept_options options;
        options.ping_interval = TRICKLE;
        res.upgrade_websocket(options, [](ws_ptr ws) {
            ws->on_message([weak = std::weak_ptr<http::websocket_connection>(ws)](const std::string& message, bool) {
                if (auto ws = weak.lock()) ws->send_text("echo:" + message);
            });
        });
    });
    running_server running(target);
    tunnel_proxy proxy(transport::tcp, running.port, with_tunnel_timeout(TUNNEL_TIMEOUT));
    auto url = "ws://127.0.0.1:" + std::to_string(proxy.running->port) + "/ws";

    bool opened = false;
    std::string echo;
    clock_type::duration took{};
    boost::asio::io_context context;
    co_spawn(context, [&]() -> awaitable<void> {
        auto client = co_await http::executor_client::for_current_coroutine();
        auto connected = co_await client->websocket(url);
        if (!connected) co_return;
        opened = true;
        auto ws = std::make_shared<http::websocket_client>(std::move(*connected));
        auto start = clock_type::now();
        // nothing sent for QUIET: the read below answers the pings meanwhile
        co_spawn(context, [ws]() -> awaitable<void> {
            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, QUIET);
            co_await timer.async_wait(use_nothrow_awaitable);
            co_await ws->send_text_async("hello");
        }, detached);
        echo = (co_await ws->receive_async()).first;
        took = clock_type::now() - start;
        co_await ws->close_async();
    }, detached);
    context.run_for(QUIET + TOLERANCE + GIVE_UP);

    INFO("echo after " << millis(took).count() << " ms");
    REQUIRE(opened);
    CHECK(echo == "echo:hello");
    CHECK(took >= QUIET);
}

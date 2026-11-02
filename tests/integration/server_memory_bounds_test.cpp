#include "server_test_helpers.hpp"
#include "../fixtures/hostile_peer.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/websocket_connection.hpp>
#include "../fixtures/awaited_shutdown.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(__GLIBC__)
#include <malloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#endif

// What one hostile client makes the server hold: memory and queued work stay near what the
// client sent, however much its requests announce. Each case asserts the bound itself (bytes
// allocated, handlers run), not only that the connection closes in the end.

using namespace thinger;
using namespace thinger::http::test;
using namespace std::chrono_literals;
using namespace server_test;

namespace {

    using clock_type = std::chrono::steady_clock;

    constexpr size_t MiB = 1024 * 1024;

    template<typename Condition>
    bool eventually(Condition condition, clock_type::duration timeout) {
        auto deadline = clock_type::now() + timeout;
        while (!condition()) {
            if (clock_type::now() > deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    constexpr bool sanitized = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    constexpr bool sanitized = true;
#else
    constexpr bool sanitized = false;
#endif
#else
    constexpr bool sanitized = false;
#endif

    // The bytes the process has allocated from the C allocator (where operator new takes them),
    // whether their pages were touched or not. Not under the sanitizers, whose allocator is
    // their own, nor where the allocator does not tell (false)
    bool allocated_bytes(size_t& bytes) {
        if constexpr (sanitized) return false;
#if defined(__GLIBC__)
        auto info = mallinfo2();
        bytes = info.uordblks + info.hblkhd;
        return true;
#elif defined(__APPLE__)
        malloc_statistics_t stats{};
        malloc_zone_statistics(nullptr, &stats);
        bytes = stats.size_in_use;
        return true;
#else
        return false;
#endif
    }

}

// Many heads announcing the largest body allowed, and no byte of it: the server takes memory for
// the bytes that arrive, not for those announced (v0.9.0 allocated each body whole, 512 MiB here)
TEST_CASE("Heads announcing large bodies take memory only for what arrives",
          "[server][hostile_peer][memory][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    constexpr int connections = 64;
    constexpr size_t announced = 8 * MiB;   // the default body limit

    http::server server;
    // Counted as the handler starts reading the body, which it reads to its end
    std::atomic<int> reading{0};
    server.post("/upload", [&](http::request& req, http::response& res) -> thinger::awaitable<void> {
        ++reading;
        if (!co_await req.read_body()) co_return;
        res.send("ok");
    });
    running_server running(server, kind);

    size_t before = 0;
    bool measured = allocated_bytes(before);

    // Connected first (over TLS, the handshakes take memory of their own on both ends)
    std::vector<std::unique_ptr<hostile_client>> clients;
    for (int i = 0; i < connections; ++i) clients.push_back(std::make_unique<hostile_client>(running.endpoint()));
    REQUIRE(eventually([&] { return server.stats().http_connections == static_cast<uint64_t>(connections); }, 10s));
    size_t connected = 0;
    measured = measured && allocated_bytes(connected);

    const std::string head = "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(announced) + "\r\n\r\n";
    for (auto& client : clients) REQUIRE(client->send_without_reading(head));
    REQUIRE(eventually([&] { return reading == connections; }, 10s));
    // Each handler went on to wait for its body, past where it counted itself (on the server thread)
    std::this_thread::sleep_for(200ms);

    size_t after = 0;
    if (!measured || !allocated_bytes(after)) {
        SKIP("the allocator does not tell what it allocated here");
    }
    auto growth = [](size_t from, size_t to) { return to > from ? to - from : 0; };
    INFO("allocated " << growth(before, after) / 1024 << " KiB more, " << growth(connected, after) / 1024
                      << " KiB for the " << connections << " heads announcing " << announced / MiB << " MiB each");
    CHECK(growth(before, after) < 32 * MiB);
    // The heads alone: a request each, and the first piece of its body (64 KiB)
    CHECK(growth(connected, after) < 16 * MiB);
    CHECK(server.stats().http_connections == static_cast<uint64_t>(connections));
}

namespace {

    std::string websocket_upgrade(const std::string& path) {
        return "GET " + path + " HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    }

    // The head of a binary frame from a client announcing `size` bytes: masked with a zero key,
    // so the payload bytes after it go as they are
    std::string frame_header_announcing(uint64_t size) {
        std::string header{char(0x82), char(0x80 | 127)};
        for (int i = 7; i >= 0; --i) header.push_back(char((size >> (i * 8)) & 0xFF));
        return header.append(4, '\0');
    }

}

// Many WebSocket frame headers announcing the largest message allowed, followed by one byte of
// it or by none: the server takes memory for the bytes that arrive, not for those announced
// (v0.9.1 reserved each message whole once a byte of it arrived, 1 GiB here), and every
// connection waits for the rest
TEST_CASE("WebSocket frame headers announcing large messages take memory only for what arrives",
          "[server][websocket][hostile_peer][memory][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    bool with_byte = GENERATE(true, false);
    INFO((with_byte ? "one byte of the payload" : "no byte of the payload"));
    constexpr int connections = 64;
    constexpr uint64_t announced = 16 * MiB - 1;   // within the default max_message_size

    http::server server;
    std::atomic<int> opened{0};
    std::atomic<int> received{0};
    server.get("/ws", [&](http::request&, http::response& res) {
        res.upgrade_websocket({}, [&](std::shared_ptr<http::websocket_connection> ws) {
            ws->on_message([&](std::string, bool) { ++received; });
            ++opened;
        });
    });
    running_server running(server, kind);

    size_t before = 0;
    bool measured = allocated_bytes(before);

    // Upgraded first (over TLS, the handshakes take memory of their own on both ends)
    std::vector<std::unique_ptr<hostile_client>> clients;
    for (int i = 0; i < connections; ++i) {
        clients.push_back(std::make_unique<hostile_client>(running.endpoint()));
        REQUIRE(clients.back()->upgrade(websocket_upgrade("/ws"), 5s).answered_with("HTTP/1.1 101"));
    }
    REQUIRE(eventually([&] { return opened == connections; }, 10s));
    REQUIRE(eventually([&] { return server.stats().websocket_connections == static_cast<uint64_t>(connections); }, 10s));
    size_t connected = 0;
    measured = measured && allocated_bytes(connected);

    auto frame = frame_header_announcing(announced) + (with_byte ? "x" : "");
    for (auto& client : clients) REQUIRE(client->send_without_reading(frame));
    // Each connection read its frame header, and waits for the rest of the payload
    std::this_thread::sleep_for(300ms);

    size_t after = 0;
    if (!measured || !allocated_bytes(after)) {
        SKIP("the allocator does not tell what it allocated here");
    }
    auto growth = [](size_t from, size_t to) { return to > from ? to - from : 0; };
    INFO("allocated " << growth(before, after) / 1024 << " KiB more, " << growth(connected, after) / 1024
                      << " KiB for the " << connections << " frames announcing " << announced << " bytes each");
    CHECK(growth(connected, after) < 16 * MiB);
    CHECK(received == 0);
    CHECK(server.stats().websocket_connections == static_cast<uint64_t>(connections));
}

// Many connections that each received one large message: once delivered, the connections give
// its buffer up (v0.9.1 kept each at its peak until the connection closed, 256 MiB here)
TEST_CASE("WebSocket connections give up the buffer of a large message once delivered",
          "[server][websocket][memory][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    constexpr int connections = 64;
    constexpr size_t message_size = 4 * MiB;

    http::server server;
    std::atomic<int> opened{0};
    std::atomic<int> received{0};
    std::atomic<int> wrong{0};
    server.get("/ws", [&](http::request&, http::response& res) {
        res.upgrade_websocket({}, [&](std::shared_ptr<http::websocket_connection> ws) {
            ws->on_message([&](std::string message, bool) {
                if (message.size() != message_size) ++wrong;
                ++received;
            });
            ++opened;
        });
    });
    running_server running(server, kind);

    std::vector<std::unique_ptr<hostile_client>> clients;
    for (int i = 0; i < connections; ++i) {
        clients.push_back(std::make_unique<hostile_client>(running.endpoint()));
        REQUIRE(clients.back()->upgrade(websocket_upgrade("/ws"), 5s).answered_with("HTTP/1.1 101"));
    }
    REQUIRE(eventually([&] { return opened == connections; }, 10s));
    REQUIRE(eventually([&] { return server.stats().websocket_connections == static_cast<uint64_t>(connections); }, 10s));
    size_t connected = 0;
    bool measured = allocated_bytes(connected);

    {   // (the frame the clients send is not counted)
        auto frame = frame_header_announcing(message_size) + std::string(message_size, 'x');
        for (auto& client : clients) REQUIRE(client->send_without_reading(frame));   // 10 s each
    }
    REQUIRE(eventually([&] { return received == connections; }, 30s));
    CHECK(wrong == 0);
    // (each connection gives its buffer up right after its on_message() returns)
    std::this_thread::sleep_for(200ms);

    size_t after = 0;
    if (!measured || !allocated_bytes(after)) {
        SKIP("the allocator does not tell what it allocated here");
    }
    size_t growth = after > connected ? after - connected : 0;
    INFO("allocated " << growth / 1024 << " KiB more after " << connections << " messages of "
                      << message_size << " bytes");
    CHECK(growth < 8 * MiB);
    CHECK(server.stats().websocket_connections == static_cast<uint64_t>(connections));
}

namespace {

    constexpr int pipelined = 100;
    // The requests read ahead of the responses written out (see server_connection)
    constexpr int queue_bound = 16;

    // A response far larger than the buffers of a connection, on every transport
    constexpr size_t large_size = 16 * MiB;
    // A response of its own for each pipelined request, larger than the buffers of a client with
    // a small receive buffer: its first bytes tell which request it answers
    constexpr size_t numbered_size = 1 * MiB;
    constexpr size_t number_digits = 8;

    std::string numbered_body(int number) {
        auto digits = std::to_string(number);
        std::string body(numbered_size, 'x');
        body.replace(number_digits - digits.size(), digits.size(), digits);
        std::fill_n(body.begin(), number_digits - digits.size(), '0');
        return body;
    }

    // The responses complete in `data`, in order: the numbers their bodies start with (-1 for one
    // not 200 or without a Content-Length)
    std::vector<int> numbered_responses(const std::string& data) {
        std::vector<int> numbers;
        for (size_t at = 0;;) {
            auto end = data.find("\r\n\r\n", at);
            if (end == std::string::npos) return numbers;
            std::string head = data.substr(at, end - at);
            std::transform(head.begin(), head.end(), head.begin(), [](unsigned char c) { return std::tolower(c); });
            auto length = head.find("\r\ncontent-length: ");
            if (!head.starts_with("http/1.1 200") || length == std::string::npos) {
                numbers.push_back(-1);
                return numbers;
            }
            size_t size = std::stoul(head.substr(length + 18));
            if (data.size() < end + 4 + size) return numbers;
            numbers.push_back(size >= number_digits ? std::stoi(data.substr(end + 4, number_digits)) : -1);
            at = end + 4 + size;
        }
    }

    std::string pipeline(const std::string& path, int count, bool numbered = false) {
        std::string requests;
        for (int i = 0; i < count; ++i) {
            requests += "GET " + path + (numbered ? std::to_string(i) : "") + " HTTP/1.1\r\nHost: x\r\n\r\n";
        }
        return requests;
    }

    // A server with `options`, counting the requests its routes handle: GET /large answers
    // large_size bytes, GET /numbered/:n a numbered body, GET /small a few bytes, and GET /hold a
    // chunked response held open until release()
    struct pipelining_server {
        http::server server;
        std::atomic<int> large{0};
        std::atomic<int> small{0};
        std::array<std::atomic<int>, pipelined> numbered{};
        std::atomic<bool> held{false};
        std::mutex mutex;
        std::optional<http::response> held_response;
        std::optional<running_server> running;

        pipelining_server(transport kind, const http::connection_options& options) {
            server.set_connection_options(options);
            server.get("/large", [this](http::response& res) {
                ++large;
                res.send(std::string(large_size, 'x'));
            });
            server.get("/numbered/:n", [this](http::request& req, http::response& res) {
                int number = std::stoi(req["n"]);
                ++numbered.at(number);
                res.send(numbered_body(number));
            });
            server.get("/small", [this](http::response& res) {
                ++small;
                res.send("ok");
            });
            server.get("/hold", [this](http::response& res) {
                res.start_chunked("text/plain");
                res.write_chunk("held");
                std::scoped_lock lock(mutex);
                held_response = res;
                held = true;
            });
            running.emplace(server, kind);
        }

        ~pipelining_server() { release(); }

        void release() {
            std::optional<http::response> res;
            {
                std::scoped_lock lock(mutex);
                res = std::move(held_response);
                held_response.reset();
            }
            if (res) res->end_chunked();
        }

        const transport_endpoint& endpoint() const { return running->endpoint(); }
        uint64_t connections() { return server.stats().http_connections; }
    };

}

// A client pipelining requests for large responses and reading none: the server handles the
// first, whose write waits for the socket, and reads no further request until its client takes
// it, or the write deadline closes the connection (v0.9.0 built every response, 1.6 GB here)
TEST_CASE("Pipelined requests wait for a response the client does not read",
          "[server][hostile_peer][memory][pipelining][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::connection_options options;
    options.write_timeout = 2s;
    pipelining_server fixture(kind, options);

    hostile_client client(fixture.endpoint(), 200ms, 4096);
    REQUIRE(client.send_without_reading(pipeline("/large", pipelined)));
    REQUIRE(eventually([&] { return fixture.large >= 1; }, 5s));
    auto first = clock_type::now();
    std::this_thread::sleep_for(1s);
    CHECK(fixture.large <= 2);

    // At the write deadline (once the buffers of the connection are full), no later
    REQUIRE(eventually([&] { return fixture.connections() == 0; }, 2s + 2s + 2s));
    auto elapsed = clock_type::now() - first;
    INFO("closed after " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms");
    CHECK(elapsed >= 2s - 100ms);
    CHECK(fixture.large <= 2);
}

// A response held open ahead of pipelined requests, none read: the server handles those behind
// it up to its queue bound, and the rest once the held response ends (v0.9.0 handled all 100
// at once)
TEST_CASE("Pipelined requests behind a held response stop at the queue bound",
          "[server][hostile_peer][memory][pipelining][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    pipelining_server fixture(kind, {});

    hostile_client client(fixture.endpoint());
    REQUIRE(client.send_without_reading("GET /hold HTTP/1.1\r\nHost: x\r\n\r\n" + pipeline("/small", pipelined)));
    REQUIRE(eventually([&] { return fixture.held.load(); }, 5s));
    REQUIRE(eventually([&] { return fixture.small > 0; }, 5s));
    std::this_thread::sleep_for(1s);
    INFO("handled behind the held response: " << fixture.small);
    CHECK(fixture.small <= queue_bound);

    // Released: the rest is read and answered, in order, and the connection kept
    fixture.release();
    hostile_outcome outcome;
    client.read_at_rate(outcome, SIZE_MAX, 10s, [](const std::string& data) {
        size_t answered = 0;
        for (size_t at = 0; (at = data.find("\r\n\r\nok", at)) != std::string::npos; at += 6) ++answered;
        return answered == static_cast<size_t>(pipelined);
    });
    CHECK_FALSE(outcome.closed);
    CHECK(fixture.small == pipelined);
}

// A client pipelining requests whose responses do not fit in the buffers, read slowly and then
// whole: the pause loses nothing, and every handler runs once, in order
TEST_CASE("Pipelined requests read slowly get every response, in order",
          "[server][memory][pipelining][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    pipelining_server fixture(kind, {});

    hostile_client client(fixture.endpoint(), 100ms, 64 * 1024);
    REQUIRE(client.send_without_reading(pipeline("/numbered/", pipelined, true)));
    auto complete = [](const std::string& data) { return numbered_responses(data).size() == pipelined; };
    hostile_outcome outcome;
    // Slowly, a second, then as fast as it comes
    client.read_at_rate(outcome, 16 * 1024, 1s, complete);
    client.read_at_rate(outcome, SIZE_MAX, 60s, complete);

    auto numbers = numbered_responses(outcome.data);
    REQUIRE(numbers.size() == pipelined);
    for (int i = 0; i < pipelined; ++i) {
        INFO("response " << i);
        CHECK(numbers[i] == i);
        CHECK(fixture.numbered[i] == 1);
    }
    CHECK_FALSE(outcome.closed);
}

// A connection paused by the bound, whether its front response waits for the socket or is held
// open: a reset, a stop or a shutdown ends it, and nothing stays waiting
TEST_CASE("A connection paused by the queue bound ends on reset, stop and shutdown",
          "[server][hostile_peer][memory][pipelining][shutdown][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::connection_options options;
    // Far longer than the case: only what it does ends the connection
    options.write_timeout = 60s;
    options.idle_timeout = 60s;
    pipelining_server fixture(kind, options);

    bool held = GENERATE(false, true);
    INFO((held ? "held response ahead" : "write waiting for the socket"));
    hostile_client client(fixture.endpoint(), 200ms, 4096);
    if (held) {
        REQUIRE(client.send_without_reading("GET /hold HTTP/1.1\r\nHost: x\r\n\r\n" + pipeline("/small", pipelined)));
        REQUIRE(eventually([&] { return fixture.small > 0; }, 5s));
    } else {
        REQUIRE(client.send_without_reading(pipeline("/large", pipelined)));
        REQUIRE(eventually([&] { return fixture.large >= 1; }, 5s));
    }
    std::this_thread::sleep_for(300ms);
    REQUIRE(fixture.connections() == 1);

    SECTION("Reset by the client") {
        // Told by the write that fails: only one that waits for the socket
        if (held) {
            SUCCEED("a held response writes nothing: its idle deadline bounds it");
            return;
        }
        client.connection().reset();
        CHECK(eventually([&] { return fixture.connections() == 0; }, 5s));
        CHECK(fixture.large <= 2);
    }
    SECTION("Server stopped") {
        // Its threads end (the closes it posts may not run once it stopped: no count to check)
        auto stopped = std::async(std::launch::async, [&] { fixture.running.reset(); });
        CHECK(stopped.wait_for(5s) == std::future_status::ready);
    }
    SECTION("Server shut down") {
        http::test::awaited_shutdown shutdown(fixture.server, 1s);
        REQUIRE(shutdown.ended_within(5s));
        // (the write it closed ends meanwhile, posted)
        CHECK(eventually([&] { return fixture.connections() == 0; }, 2s));
    }
}

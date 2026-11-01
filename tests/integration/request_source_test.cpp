#include "server_test_helpers.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_connection.hpp>
#include <thinger/http/server/http_stream.hpp>
#include <thinger/util/types.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace thinger;
using namespace std::chrono_literals;
using namespace server_test;

// The network detail::request_source (connection_source): a request reads its body from its
// connection through it. It asks for the body with a single 100 Continue the first time it is
// needed, keeps the connection alive while the body keeps arriving, reads nothing once the
// connection closes gracefully, and ends (EOF) once the connection is gone. The request knows
// its connection only through it, without keeping it (or its stream) alive.
// memory_source and a request without a source: unit/http/server/request_source_test.cpp

namespace {

    using steady = std::chrono::steady_clock;

    const std::string continue_line = "HTTP/1.1 100 Continue\r\n\r\n";

    awaitable<void> sleep_for(std::chrono::milliseconds time) {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, time);
        co_await timer.async_wait(use_nothrow_awaitable);
    }

    template<typename Condition>
    bool eventually(Condition condition, std::chrono::milliseconds timeout = 5s) {
        auto deadline = steady::now() + timeout;
        while (!condition()) {
            if (steady::now() > deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    std::string post_head(const std::string& path, size_t length, const std::string& headers = "") {
        return "POST " + path + " HTTP/1.1\r\nHost: localhost\r\n" + headers +
               "Content-Length: " + std::to_string(length) + "\r\n\r\n";
    }

    // The whole body, read by the handler in reads of `piece` bytes (each a read of the source,
    // once the read-ahead is used up)
    awaitable<std::string> read_all(http::request& req, size_t piece) {
        std::string body;
        std::vector<uint8_t> buffer(piece);
        while (size_t bytes = co_await req.read_some(buffer.data(), buffer.size())) {
            body.append(reinterpret_cast<char*>(buffer.data()), bytes);
        }
        co_return body;
    }

    // An io_context running on a thread of its own, apart from the server: where a handler
    // moves to read its body
    struct other_thread {
        boost::asio::io_context io_context;
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{io_context.get_executor()};
        std::thread thread{[this] { io_context.run(); }};

        // (stopped: a test failing may leave a read pending there)
        ~other_thread() {
            io_context.stop();
            thread.join();
        }

        // The whole body, read there in reads of `piece` bytes; `elsewhere` tells whether every
        // read resumed there
        awaitable<std::string> read_all(http::request& req, size_t piece, std::atomic<bool>& elsewhere) {
            co_return co_await boost::asio::co_spawn(io_context, [&]() -> awaitable<std::string> {
                std::string body;
                std::vector<uint8_t> buffer(piece);
                while (size_t bytes = co_await req.read_some(buffer.data(), buffer.size())) {
                    if (!io_context.get_executor().running_in_this_thread()) elsewhere = false;
                    body.append(reinterpret_cast<char*>(buffer.data()), bytes);
                }
                co_return body;
            }, boost::asio::use_awaitable);
        }
    };

    // What a handler saw reading its body late (after its route timed out), written on the
    // thread of the server before `done`
    struct late_read {
        std::atomic<bool> client_sent{false};  // the client sent more body (set by the test)
        bool lingering = false;                // the connection closed gracefully before the read
        bool connection_gone = false;          // the connection was gone before the read
        size_t bytes = 0;
        std::chrono::milliseconds took{0};
        http::connection_info info;
        std::string peer_ip;
        bool tls = false;
        std::atomic<bool> done{false};

        awaitable<void> read(http::request& req) {
            uint8_t buffer[4096];
            auto start = steady::now();
            bytes = co_await req.read_some(buffer, sizeof(buffer));
            took = std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - start);
            info = req.connection();
            peer_ip = req.get_peer_ip();
            tls = req.tls().has_value();
            done = true;
        }
    };

}

TEST_CASE("The source sends 100 Continue once, when the body is first read", "[server][request_source][expect][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    // Reads the body a while after it starts, in reads of 3 bytes
    server.post("/late-read", [](http::request& req, http::response& res) -> awaitable<void> {
        co_await sleep_for(300ms);
        res.send("body=[" + co_await read_all(req, 3) + "]");
    });
    running_server running(server, kind);
    http::test::raw_connection connection(running.endpoint());

    REQUIRE(connection.write(post_head("/late-read", 12, "Expect: 100-continue\r\n")));
    // Nothing while the handler does not read
    std::string received;
    connection.read_some(received, 150ms);
    REQUIRE(received.empty());
    // Asked for once it reads, before the client sent a byte
    REQUIRE(connection.read_until(received, continue_line, 3s));
    REQUIRE(received == continue_line);

    // The body arrives in pieces: many reads, no other 100 Continue
    for (std::string piece : {"hello", " wor", "ld!"}) {
        REQUIRE(connection.write(piece));
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(connection.read_until(received, "body=[hello world!]", 3s));
    INFO("received: " << received);
    REQUIRE(count(received, "100 Continue") == 1);
    REQUIRE(count(received, "HTTP/1.1 200") == 1);
}

TEST_CASE("The source keeps the connection alive while the body keeps arriving", "[server][request_source][timeout][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    http::connection_options options;
    options.idle_timeout = 1s;
    server.set_connection_options(options);
    server.post("/upload", [](http::request& req, http::response& res) -> awaitable<void> {
        auto body = co_await read_all(req, 4096);
        res.send("size=" + std::to_string(body.size()));
    });
    running_server running(server, kind);
    http::test::raw_connection connection(running.endpoint());
    std::string received;

    SECTION("A slow upload, longer than the connection timeout") {
        REQUIRE(connection.write(post_head("/upload", 500)));
        // 2 s of body, a piece every 400 ms
        for (int i = 0; i < 5; i++) {
            std::this_thread::sleep_for(400ms);
            REQUIRE(connection.write(std::string(100, 'x')));
        }
        REQUIRE(connection.read_until(received, "size=500", 3s));
        REQUIRE(received.starts_with("HTTP/1.1 200"));
    }

    SECTION("A body that stops arriving: the connection times out") {
        REQUIRE(connection.write(post_head("/upload", 500) + std::string(100, 'x')));
        auto start = steady::now();
        auto result = connection.read_until_closed(4s);
        INFO("received: " << result.data);
        REQUIRE(result.closed);
        REQUIRE(steady::now() - start >= 900ms);
        REQUIRE(result.data.find("HTTP/1.1 200") == std::string::npos);
    }
}

TEST_CASE("A handler on another executor reads its body through the thread of its connection",
          "[server][request_source][expect][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    other_thread other;
    std::atomic<bool> elsewhere{true};
    server.post("/elsewhere", [&](http::request& req, http::response& res) -> awaitable<void> {
        res.send("body=[" + co_await other.read_all(req, 3, elsewhere) + "]");
    });
    running_server running(server, kind);
    http::test::raw_connection connection(running.endpoint());
    std::string received;
    size_t continues = 0;

    SECTION("With Expect: 100-continue: asked for once") {
        REQUIRE(connection.write(post_head("/elsewhere", 12, "Expect: 100-continue\r\n")));
        REQUIRE(connection.read_until(received, continue_line, 3s));
        continues = 1;
    }
    SECTION("Without it") {
        REQUIRE(connection.write(post_head("/elsewhere", 12)));
    }
    // In pieces, each more than one read of the handler
    for (std::string piece : {"hello", " wor", "ld!"}) {
        std::this_thread::sleep_for(50ms);
        REQUIRE(connection.write(piece));
    }
    REQUIRE(connection.read_until(received, "body=[hello world!]", 3s));
    INFO("received: " << received);
    REQUIRE(count(received, "100 Continue") == continues);
    REQUIRE(count(received, "HTTP/1.1 200") == 1);
    REQUIRE(elsewhere);
}

TEST_CASE("A body read from another executor keeps the connection alive while it arrives",
          "[server][request_source][timeout][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    http::connection_options options;
    options.idle_timeout = 1s;
    server.set_connection_options(options);
    other_thread other;
    std::atomic<bool> elsewhere{true};
    server.post("/upload", [&](http::request& req, http::response& res) -> awaitable<void> {
        auto body = co_await other.read_all(req, 4096, elsewhere);
        res.send("size=" + std::to_string(body.size()));
    });
    running_server running(server, kind);
    http::test::raw_connection connection(running.endpoint());
    std::string received;

    // 2 s of body, a piece every 400 ms: longer than the connection timeout
    REQUIRE(connection.write(post_head("/upload", 500)));
    for (int i = 0; i < 5; i++) {
        std::this_thread::sleep_for(400ms);
        REQUIRE(connection.write(std::string(100, 'x')));
    }
    REQUIRE(connection.read_until(received, "size=500", 3s));
    REQUIRE(received.starts_with("HTTP/1.1 200"));
    REQUIRE(elsewhere);
}

TEST_CASE("The source reads nothing once the connection closes gracefully", "[server][request_source][linger][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    late_read probe;
    // Answered by its timeout while the body arrives, so the connection lingers; the handler
    // reads once it does, and the client sent more
    server.post("/linger", [&probe](http::request& req, http::response& res) -> awaitable<void> {
        auto connection = res.get_connection();
        auto deadline = steady::now() + 5s;
        while (connection && !(connection->lingering() && probe.client_sent) && steady::now() < deadline) {
            co_await sleep_for(10ms);
        }
        probe.lingering = connection && connection->lingering();
        co_await sleep_for(50ms);
        co_await probe.read(req);
    }).timeout(200ms);
    running_server running(server, kind);
    http::test::raw_connection connection(running.endpoint());

    // The head alone first: body bytes read with it would be the request's already, read ahead
    REQUIRE(connection.write(post_head("/linger", 64 * 1024)));
    std::this_thread::sleep_for(50ms);
    REQUIRE(connection.write(std::string(1024, 'x')));
    std::string received;
    REQUIRE(connection.read_until(received, "HTTP/1.1 504", 3s));
    REQUIRE(connection.write(std::string(16 * 1024, 'y')));
    probe.client_sent = true;

    REQUIRE(eventually([&] { return probe.done.load(); }));
    REQUIRE(probe.lingering);
    REQUIRE(probe.bytes == 0);
    REQUIRE(probe.took < 500ms);
    connection.close();
}

TEST_CASE("A request outliving its connection reads EOF and knows no connection", "[server][request_source][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    late_read probe;
    // Answered by its timeout; the handler reads once the client left and the connection is gone
    server.post("/gone", [&probe](http::request& req, http::response& res) -> awaitable<void> {
        std::weak_ptr<http::server_connection> connection = res.get_connection();
        auto deadline = steady::now() + 5s;
        while (!connection.expired() && steady::now() < deadline) co_await sleep_for(10ms);
        probe.connection_gone = connection.expired();
        co_await probe.read(req);
    }).timeout(200ms);
    running_server running(server, kind);

    {
        http::test::raw_connection connection(running.endpoint());
        REQUIRE(connection.write(post_head("/gone", 100)));
        std::this_thread::sleep_for(50ms);
        REQUIRE(connection.write(std::string(10, 'x')));
        std::string received;
        REQUIRE(connection.read_until(received, "HTTP/1.1 504", 3s));
        connection.close();
    }

    REQUIRE(eventually([&] { return probe.done.load(); }, 8s));
    REQUIRE(probe.connection_gone);
    REQUIRE(probe.bytes == 0);
    REQUIRE(probe.info.peer_ip.empty());
    REQUIRE(probe.info.peer_port.empty());
    REQUIRE(probe.info.local_port.empty());
    REQUIRE_FALSE(probe.info.secure);
    REQUIRE(probe.peer_ip.empty());
    REQUIRE_FALSE(probe.tls);
}

TEST_CASE("The connection of a request comes from its source", "[server][request_source][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    std::mutex mutex;
    std::string peer_ip;
    http::connection_info info;
    std::optional<asio::tls_session> tls;
    server.get("/peer", [&](http::request& req, http::response& res) {
        {
            std::scoped_lock lock(mutex);
            peer_ip = req.get_peer_ip();
            info = req.connection();
            tls = req.tls();
        }
        res.send("peer");
    });
    running_server running(server, kind);
    http::test::raw_connection connection(running.endpoint());
    // The port the client connects from (none over Unix)
    std::string client_port;
    connection.visit_stream([&](auto& stream) {
        if constexpr (requires { stream.lowest_layer().local_endpoint().port(); }) {
            client_port = std::to_string(stream.lowest_layer().local_endpoint().port());
        }
    });

    REQUIRE(connection.write("GET /peer HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
    auto result = connection.read_until_closed(3s);
    REQUIRE(result.data.find("peer") != std::string::npos);

    std::scoped_lock lock(mutex);
    // As at 0.8.1, the remote address of the socket
    REQUIRE(peer_ip == info.peer_ip);
    if (kind == transport::unix) {
        REQUIRE(info.peer_port == "0");
        REQUIRE(info.local_port == "0");
        REQUIRE_FALSE(info.secure);
        REQUIRE_FALSE(tls.has_value());
        return;
    }
    REQUIRE(peer_ip == "127.0.0.1");
    REQUIRE(info.peer_port == client_port);
    REQUIRE(info.local_port == std::to_string(running.port));
    REQUIRE(info.secure == (kind == transport::tls));
    REQUIRE(tls.has_value() == (kind == transport::tls));
    if (tls) REQUIRE(tls->version.starts_with("TLS"));
}

TEST_CASE("Requests do not keep their connection or stream alive, with the response hook", "[server][request_source][response_hook][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    http::server server;
    std::atomic<int> reported{0};
    server.on_response([&](const http::request&, const http::response_info&) { reported++; });
    std::mutex mutex;
    std::vector<std::weak_ptr<http::http_stream>> streams;
    std::weak_ptr<http::server_connection> connection;
    auto keep = [&](http::request& req, http::response& res) {
        std::scoped_lock lock(mutex);
        streams.push_back(req.get_http_stream());
        connection = res.get_connection();
    };
    server.get("/kept", [&](http::request& req, http::response& res) {
        keep(req, res);
        res.send("kept");
    });
    // Goes on past its timeout: the request outlives its response, and its connection
    server.post("/retained", [&](http::request& req, http::response& res) -> awaitable<void> {
        keep(req, res);
        co_await sleep_for(600ms);
    }).timeout(200ms);
    running_server running(server, kind);

    SECTION("Requests of a keep-alive connection") {
        http::test::raw_connection client(running.endpoint());
        std::string received;
        REQUIRE(client.write("GET /kept HTTP/1.1\r\nHost: localhost\r\n\r\n"));
        REQUIRE(client.read_until(received, "kept", 3s));
        received.clear();
        REQUIRE(client.write("GET /kept HTTP/1.1\r\nHost: localhost\r\n\r\n"));
        REQUIRE(client.read_until(received, "kept", 3s));
        REQUIRE(eventually([&] { return reported == 2; }));
        client.close();
    }

    SECTION("A request kept by its handler after its response and its connection") {
        http::test::raw_connection client(running.endpoint());
        std::string received;
        REQUIRE(client.write(post_head("/retained", 100) + std::string(10, 'x')));
        REQUIRE(client.read_until(received, "HTTP/1.1 504", 3s));
        REQUIRE(eventually([&] { return reported == 1; }));
        client.close();
    }

    REQUIRE(eventually([&] {
        std::scoped_lock lock(mutex);
        if (!connection.expired()) return false;
        for (auto& stream : streams) {
            if (!stream.expired()) return false;
        }
        return !streams.empty();
    }));
}

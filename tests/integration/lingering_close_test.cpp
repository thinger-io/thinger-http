#include "server_test_helpers.hpp"
#include <thinger/http/client/client.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/util/types.hpp>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace thinger;
using namespace std::chrono_literals;
using namespace server_test;

// Lingering close (RFC 9112 section 9.6): a connection closed after a response while the client
// is still sending its request (a body over the limit answered 413 before it was read, a 417, a
// 401 before the body, a request timeout...) stops sending, drops what keeps arriving, and closes
// once the client does, or at a byte limit or a timeout. Closed at once, the unread data made the
// system reset the connection, and a client still sending got EPIPE or a reset instead of the
// response. Node reads such a body to its end (keeping the connection); Go drains it up to a limit

namespace {

    constexpr size_t KiB = 1024;
    constexpr size_t MiB = 1024 * KiB;

    // The lingering close settings of the process, restored once the test ends
    struct linger_settings {
        linger_settings(size_t max_bytes, std::chrono::milliseconds timeout) {
            http::server::set_lingering_close(max_bytes, timeout);
        }
        ~linger_settings() {
            http::server::set_lingering_close(http::server_connection::default_linger_max_bytes,
                                              http::server_connection::default_linger_timeout);
        }
    };

    struct upload_options {
        std::string head;                       // the request head, blank line included
        size_t body_size = 0;                   // bytes of body sent after it
        bool chunked = false;                   // sent as chunks (the last one included)
        size_t piece = 64 * KiB;                // written at once
        std::chrono::milliseconds pace{0};      // pause between pieces
        std::chrono::milliseconds timeout{20s};
    };

    struct upload_result {
        size_t sent = 0;                            // body bytes written
        boost::system::error_code write_error;      // why writing ended early
        std::string response;
        boost::system::error_code read_error;       // how reading ended: eof if the server closed
        bool read_ended = false;
        // Since the response arrived: when writing failed
        std::chrono::milliseconds write_failed_after{-1};
    };

    // A client sending a request and its body as fast as the server takes it (or at the pace
    // set), reading the response meanwhile, as a client uploading a file does: it notices the
    // response only once it read it
    upload_result upload(uint16_t port, const upload_options& options) {
        boost::asio::io_context ioc;
        boost::asio::ip::tcp::socket socket(ioc);
        socket.connect({boost::asio::ip::make_address("127.0.0.1"), port});
        upload_result result;
        std::chrono::steady_clock::time_point response_arrived{};

        co_spawn(ioc, [&]() -> awaitable<void> {
            auto [ec, written] = co_await boost::asio::async_write(socket, boost::asio::buffer(options.head),
                                                                   use_nothrow_awaitable);
            if (ec) {
                result.write_error = ec;
                co_return;
            }
            std::string piece(options.piece, 'x');
            while (result.sent < options.body_size) {
                size_t size = std::min(options.piece, options.body_size - result.sent);
                char size_line[32];
                int line_size = std::snprintf(size_line, sizeof(size_line), "%zx\r\n", size);
                std::vector<boost::asio::const_buffer> buffers;
                if (options.chunked) buffers.emplace_back(boost::asio::buffer(size_line, line_size));
                buffers.emplace_back(boost::asio::buffer(piece.data(), size));
                if (options.chunked) buffers.emplace_back(boost::asio::buffer("\r\n", 2));
                auto [write_ec, bytes] = co_await boost::asio::async_write(socket, buffers, use_nothrow_awaitable);
                if (write_ec) {
                    result.write_error = write_ec;
                    if (response_arrived != std::chrono::steady_clock::time_point{}) {
                        result.write_failed_after = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - response_arrived);
                    }
                    co_return;
                }
                result.sent += size;
                if (options.pace.count() > 0) {
                    boost::asio::steady_timer timer(ioc, options.pace);
                    co_await timer.async_wait(use_nothrow_awaitable);
                }
            }
            if (options.chunked) {
                auto [last_ec, bytes] = co_await boost::asio::async_write(socket, boost::asio::buffer("0\r\n\r\n", 5),
                                                                          use_nothrow_awaitable);
                result.write_error = last_ec;
            }
        }, boost::asio::detached);

        co_spawn(ioc, [&]() -> awaitable<void> {
            char buffer[16 * KiB];
            for (;;) {
                auto [ec, bytes] = co_await socket.async_read_some(boost::asio::buffer(buffer), use_nothrow_awaitable);
                if (bytes > 0 && result.response.empty()) response_arrived = std::chrono::steady_clock::now();
                result.response.append(buffer, bytes);
                if (ec) {
                    result.read_error = ec;
                    result.read_ended = true;
                    co_return;
                }
            }
        }, boost::asio::detached);

        ioc.run_for(options.timeout);
        return result;
    }

    std::string post_head(const std::string& path, const std::string& headers) {
        return "POST " + path + " HTTP/1.1\r\nHost: localhost\r\n" + headers + "\r\n";
    }

    std::string length_head(const std::string& path, size_t length, const std::string& headers = "") {
        return post_head(path, headers + "Content-Length: " + std::to_string(length) + "\r\n");
    }

    // The client got the whole response, and then the end of the connection (a FIN, not a reset)
    void require_answered(const upload_result& result, const std::string& status) {
        INFO("response: " << result.response.substr(0, 300) << " | write error: " << result.write_error.message()
             << " | read error: " << result.read_error.message() << " | sent: " << result.sent);
        REQUIRE(result.response.starts_with("HTTP/1.1 " + status));
        REQUIRE(result.response.find("Connection: Close") != std::string::npos);
        REQUIRE(result.read_ended);
        REQUIRE(result.read_error == boost::asio::error::eof);
    }

    // Resident memory of the process (Linux), in bytes
    size_t resident_memory() {
        std::ifstream statm("/proc/self/statm");
        size_t pages = 0, resident = 0;
        statm >> pages >> resident;
        return resident * 4096;
    }

    struct limited_server {
        http::server server;
        std::unique_ptr<running_server> running;

        explicit limited_server(bool tls = false) {
            server.enable_ssl(tls);
            server.set_max_body_size(64 * KiB);
            server.post("/limited", [](http::request& req, http::response& res) {
                res.send("size=" + std::to_string(req.body().size()));
            }).max_body_size(64 * KiB);
            // Waits without reading the body, past its timeout
            server.post("/slow", [](http::request&, http::response&) -> awaitable<void> {
                boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 30s);
                co_await timer.async_wait(use_nothrow_awaitable);
            }).timeout(300ms).max_body_size(64 * MiB);
            // A middleware answering 401 before the body is read
            server.use([](http::request& req, http::response& res, std::function<void()> next) {
                if (req.get_http_request()->get_path() == "/private") {
                    res.error(http::http_response::status::unauthorized, "Unauthorized");
                    return;
                }
                next();
            });
            server.post("/private", [](http::response& res) { res.send("private"); });
            server.get("/health", [](http::response& res) { res.send("healthy"); });
            running = std::make_unique<running_server>(server);
        }

        uint16_t port() const { return running->port; }
    };

}

TEST_CASE("A body over the limit gets its 413 before the connection closes", "[server][linger][413][integration]") {
    limited_server fixture;

    SECTION("Content-Length: 4 MiB to a route of 64 KiB") {
        auto result = upload(fixture.port(), {.head = length_head("/limited", 4 * MiB), .body_size = 4 * MiB});
        require_answered(result, "413");
        // The server took the whole body, dropping it
        REQUIRE_FALSE(result.write_error);
        REQUIRE(result.sent == 4 * MiB);
    }

    SECTION("Chunked: 4 MiB to a route of 64 KiB") {
        auto result = upload(fixture.port(), {.head = post_head("/limited", "Transfer-Encoding: chunked\r\n"),
                                              .body_size = 4 * MiB, .chunked = true, .piece = 16 * KiB});
        require_answered(result, "413");
        REQUIRE_FALSE(result.write_error);
        REQUIRE(result.sent == 4 * MiB);
    }

    SECTION("Within the limit, the body is read and the connection kept") {
        auto result = raw_exchange(fixture.port(), length_head("/limited", 1000, "Connection: close\r\n") + std::string(1000, 'x'));
        REQUIRE(result.data.starts_with("HTTP/1.1 200"));
        REQUIRE(result.data.find("size=1000") != std::string::npos);
    }
}

TEST_CASE("Other answers sent before the body was read reach the client", "[server][linger][integration]") {
    limited_server fixture;

    SECTION("417 for an expectation that cannot be met") {
        auto result = upload(fixture.port(), {.head = length_head("/limited", 4 * MiB, "Expect: something-else\r\n"),
                                              .body_size = 4 * MiB});
        require_answered(result, "417");
        REQUIRE(result.sent == 4 * MiB);
    }

    SECTION("401 from a middleware, for a body over the limit, too large to drop") {
        auto result = upload(fixture.port(), {.head = length_head("/private", 4 * MiB), .body_size = 4 * MiB});
        INFO("response: " << result.response.substr(0, 200));
        REQUIRE(result.response.starts_with("HTTP/1.1 401"));
        REQUIRE(result.read_error == boost::asio::error::eof);
        REQUIRE(result.sent == 4 * MiB);
    }

    SECTION("401 from a middleware, for a body within the limit: dropped, and the connection kept") {
        auto result = raw_exchange(fixture.port(),
            length_head("/private", 1000) + std::string(1000, 'x') + "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(result.data.starts_with("HTTP/1.1 401"));
        REQUIRE(count(result.data, "HTTP/1.1 ") == 2);
        REQUIRE(result.data.find("healthy") != std::string::npos);
    }

    SECTION("A request timeout while the body is still arriving") {
        auto result = upload(fixture.port(), {.head = length_head("/slow", 4 * MiB), .body_size = 4 * MiB,
                                              .piece = 64 * KiB, .pace = 10ms});
        require_answered(result, "504");
        REQUIRE(result.sent == 4 * MiB);
    }

    SECTION("400 for a request that cannot be parsed, with data after it") {
        auto result = upload(fixture.port(), {.head = "POST /limited HTTP/1.1\r\nHost: localhost\r\nBad Header\r\n\r\n",
                                              .body_size = 4 * MiB});
        INFO("response: " << result.response.substr(0, 200));
        REQUIRE(result.response.starts_with("HTTP/1.1 400"));
        REQUIRE(result.read_error == boost::asio::error::eof);
    }
}

TEST_CASE("Expect: 100-continue over the limit is answered without the body", "[server][linger][expect][integration]") {
    limited_server fixture;
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::socket socket(ioc);
    socket.connect({boost::asio::ip::make_address("127.0.0.1"), fixture.port()});
    boost::asio::write(socket, boost::asio::buffer(length_head("/limited", 4 * MiB, "Expect: 100-continue\r\n")));

    // The body is never sent: the answer comes, then the end of the connection
    auto result = read_until_closed(ioc, socket, 3s);
    INFO("response: " << result.data);
    REQUIRE(result.closed);
    REQUIRE(result.data.starts_with("HTTP/1.1 413"));
    REQUIRE(result.data.find("100 Continue") == std::string::npos);
    REQUIRE(result.data.find("Connection: Close") != std::string::npos);
}

TEST_CASE("A lingering close is bounded", "[server][linger][integration]") {
    SECTION("A client that keeps sending slowly is cut at the timeout") {
        linger_settings settings(8 * MiB, 500ms);
        limited_server fixture;
        auto result = upload(fixture.port(), {.head = length_head("/limited", 1024 * MiB), .body_size = 1024 * MiB,
                                              .piece = 1 * KiB, .pace = 20ms, .timeout = 10s});
        INFO("sent: " << result.sent << " | write error: " << result.write_error.message()
             << " | after: " << result.write_failed_after.count() << " ms");
        REQUIRE(result.response.starts_with("HTTP/1.1 413"));
        REQUIRE(result.read_error == boost::asio::error::eof);
        // Writing fails once the server closed, about the timeout after it answered
        REQUIRE(result.write_error);
        REQUIRE(result.write_failed_after >= 400ms);
        REQUIRE(result.write_failed_after < 3s);
    }

    SECTION("A client that sends fast is cut at the byte limit") {
        linger_settings settings(1 * MiB, 30s);
        limited_server fixture;
        auto result = upload(fixture.port(), {.head = length_head("/limited", 1024 * MiB), .body_size = 1024 * MiB,
                                              .timeout = 10s});
        INFO("sent: " << result.sent << " | write error: " << result.write_error.message());
        REQUIRE(result.response.starts_with("HTTP/1.1 413"));
        REQUIRE(result.write_error);
        // What the server dropped, and what the socket buffers of both ends held
        REQUIRE(result.sent < 64 * MiB);
        REQUIRE(result.write_failed_after < 5s);
    }

    SECTION("Disabled: the connection closes at once") {
        linger_settings settings(0, 0ms);
        limited_server fixture;
        auto result = upload(fixture.port(), {.head = length_head("/limited", 64 * MiB), .body_size = 64 * MiB,
                                              .timeout = 10s});
        INFO("sent: " << result.sent << " | write error: " << result.write_error.message());
        // The unread body resets the connection
        REQUIRE(result.write_error);
        REQUIRE(result.sent < 64 * MiB);
    }
}

#ifdef __linux__
TEST_CASE("The memory of the server does not grow while it drops a body", "[server][linger][memory][integration]") {
    linger_settings settings(1024 * MiB, 60s);
    limited_server fixture;
    // Warm up: the first connection and response allocate what they keep
    REQUIRE(upload(fixture.port(), {.head = length_head("/limited", 1 * MiB), .body_size = 1 * MiB}).sent == 1 * MiB);

    auto before = resident_memory();
    auto result = upload(fixture.port(), {.head = length_head("/limited", 256 * MiB), .body_size = 256 * MiB,
                                          .timeout = 60s});
    auto after = resident_memory();
    INFO("resident before: " << before / KiB << " KiB, after: " << after / KiB << " KiB");
    require_answered(result, "413");
    REQUIRE(result.sent == 256 * MiB);
    REQUIRE(after < before + 16 * MiB);
}
#endif

TEST_CASE("The client reads a response sent before its request was written whole", "[client][linger][integration]") {
    limited_server fixture;
    http::client client;
    client.timeout(10s);
    std::string body(32 * MiB, 'x');
    auto url = "http://127.0.0.1:" + std::to_string(fixture.port()) + "/limited";

    SECTION("The server drops the body: the request is written whole") {
        auto response = client.post(url, body, "application/octet-stream");
        INFO("error: " << response.error());
        REQUIRE(response.status() == 413);
    }

    SECTION("The server closes at once: writing fails, and the response is read anyway") {
        linger_settings settings(0, 0ms);
        auto response = client.post(url, body, "application/octet-stream");
        INFO("error: " << response.error());
        REQUIRE(response.status() == 413);
    }

    // The connection is not reused after a response read that way
    auto response = client.get("http://127.0.0.1:" + std::to_string(fixture.port()) + "/health");
    REQUIRE(response.status() == 200);
}

TEST_CASE("A body over the limit gets its 413 over TLS", "[server][linger][413][ssl][integration]") {
    limited_server fixture(true);
    http::client client;
    client.timeout(10s).verify_ssl(false);
    auto response = client.post("https://127.0.0.1:" + std::to_string(fixture.port()) + "/limited",
                                 std::string(4 * MiB, 'x'), "application/octet-stream");
    INFO("error: " << response.error());
    REQUIRE(response.status() == 413);
}

#include "server_test_helpers.hpp"
#include "../fixtures/awaited_shutdown.hpp"
#include <thinger/http/server/http_stream.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_connection.hpp>
#include <thinger/http/server/websocket_connection.hpp>
#include <thinger/util/types.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// The Connection header of a response never contradicts what the connection does after it
// (design §4.5): one row per close_reason, on every transport of the matrix, reads the raw
// head and the end of the connection, and the reason its stream recorded. A reason that comes
// once the head was written (a drain or an abort mid-response) cannot amend it: those rows
// record the head as written first, and still see the connection closed. And the frames that
// must not carry the header: a head kept alive, a 100 Continue, the body, a protocol switch

using namespace thinger;
using namespace std::chrono_literals;
using namespace server_test;
using http::detail::after_response;
using http::detail::close_reason;

namespace {

    using clock = std::chrono::steady_clock;

    constexpr size_t KiB = 1024;
    constexpr size_t MiB = 1024 * KiB;

    const char* to_string(close_reason reason) {
        switch (reason) {
            case close_reason::client_asked: return "client_asked";
            case close_reason::http10: return "http10";
            case close_reason::draining: return "draining";
            case close_reason::continue_skipped: return "continue_skipped";
            case close_reason::body_unread: return "body_unread";
            case close_reason::body_error: return "body_error";
            case close_reason::handler_failed: return "handler_failed";
            case close_reason::route_timeout: return "route_timeout";
            case close_reason::request_timeout: return "request_timeout";
            case close_reason::framing_by_close: return "framing_by_close";
            case close_reason::aborted: return "aborted";
        }
        return "?";
    }

    std::string to_string(const std::optional<close_reason>& reason) {
        return reason ? to_string(*reason) : "none";
    }

    bool eventually(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 5s) {
        auto deadline = clock::now() + timeout;
        while (clock::now() < deadline) {
            if (condition()) return true;
            std::this_thread::sleep_for(5ms);
        }
        return condition();
    }

    // What the stream of a request had decided once its response ended, by path: read by the
    // response hook, on the thread of the connection
    struct disposition_probe {
        struct seen {
            std::optional<close_reason> reason;
            after_response disposition = after_response::keep_alive;
        };

        std::mutex mutex;
        std::map<std::string, seen> streams;

        void record(const http::request& req) {
            auto stream = req.get_http_stream();
            if (!stream) return;
            std::scoped_lock lock(mutex);
            streams[req.get_http_request()->get_path()] = {stream->reason(), stream->disposition()};
        }

        // What the stream of `path` decided, once its response ended
        std::optional<seen> of(const std::string& path) {
            std::optional<seen> result;
            eventually([&] {
                std::scoped_lock lock(mutex);
                auto it = streams.find(path);
                if (it != streams.end()) result = it->second;
                return result.has_value();
            });
            return result;
        }
    };

    // A chunked response held open by the test, with its connection: written up to a first
    // chunk, ended once released (from any thread)
    struct held_response {
        std::mutex mutex;
        std::optional<http::response> response;
        std::shared_ptr<http::server_connection> connection;
        std::atomic<bool> held{false};

        void hold(http::response& res) {
            res.start_chunked("text/plain");
            res.write_chunk("held");
            std::scoped_lock lock(mutex);
            response = res;
            connection = res.get_connection();
            held = true;
        }

        void release() {
            std::optional<http::response> res;
            std::shared_ptr<http::server_connection> keep;
            {
                std::scoped_lock lock(mutex);
                res = std::move(response);
                keep = std::move(connection);
                response.reset();
            }
            if (res) res->end_chunked();
        }

        // Release it once the handler running now on the connection thread returns, whatever
        // it decides before (the post runs after it)
        void release_after(http::response& res) {
            if (auto connection = res.get_connection()) {
                boost::asio::post(connection->get_socket()->get_io_context(), [this] { release(); });
            }
        }
    };

    // A server with a route for each way a connection closes after its response, on `kind`. Its
    // handlers run on the thread of the server: they assert nothing
    struct table_server {
        http::server server;
        disposition_probe probe;
        held_response held;
        std::atomic<bool> slow_started{false};
        std::unique_ptr<running_server> running;
        // A drain some rows start (it ends before the server stops)
        std::optional<http::test::awaited_shutdown> shutdown;

        explicit table_server(transport kind) {
            server.set_max_body_size(64 * KiB);
            server.on_response([this](const http::request& req, const http::response_info&) { probe.record(req); });

            server.get("/ok", [](http::response& res) { res.send("ok"); });
            // Reads the body before the handler (asking for it with 100 Continue if expected)
            server.post("/echo", [](http::request& req, http::response& res) {
                res.send("size=" + std::to_string(req.body().size()));
            });
            // Answers past the start of a drain
            server.get("/slow", [this](http::response& res) -> awaitable<void> {
                slow_started = true;
                boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 300ms);
                co_await timer.async_wait(use_nothrow_awaitable);
                res.send("done");
            });
            // Answers without reading the body (a coroutine reads it itself)
            server.post("/denied", [](http::request&, http::response& res) -> awaitable<void> {
                res.error(http::http_response::status::unauthorized, "Unauthorized");
                co_return;
            });
            // Times out while its body is still to come
            server.post("/timeout", [](http::request&, http::response& res) { res.send("late"); }).timeout(200ms);
            // A response held open, ahead of a pipelined request
            server.get("/hold", [this](http::response& res) { held.hold(res); });
            // Fails after responding, behind the held response
            server.get("/fail", [this](http::response& res) {
                res.send("sent");
                held.release_after(res);
                throw std::runtime_error("failed after responding");
            });
            // Aborts its streamed response, behind the held response
            server.get("/abort", [this](http::response& res) -> awaitable<void> {
                res.start_stream(http::http_response{});
                res.abort_stream();
                held.release_after(res);
                co_return;
            });
            // Aborts its streamed response once its head was written
            server.get("/abort-now", [](http::response& res) -> awaitable<void> {
                res.start_stream(http::http_response{});
                co_await res.write_stream("part");
                res.abort_stream();
            });
            // A streamed response: chunked, or ended by the close for an HTTP/1.0 client
            server.get("/stream", [](http::response& res) -> awaitable<void> {
                res.start_stream(http::http_response{});
                co_await res.write_stream("streamed");
                co_await res.end_stream();
            });
            // A body after the head of a connection taken over, until it closes
            server.get("/raw", [](http::response& res) {
                res.take_over([](http::upgraded_connection conn) {
                    auto socket = std::move(conn.socket);
                    co_spawn(socket->get_io_context(), [socket]() -> awaitable<void> {
                        co_await socket->write(std::string_view("raw body"));
                        socket->close();
                    }, detached);
                });
            });
            server.get("/chunks", [](http::response& res) {
                res.start_chunked("text/plain");
                res.write_chunk("one");
                res.write_chunk("two");
                res.end_chunked();
            });
            server.get("/ws", [](http::response& res) {
                res.upgrade_websocket({}, [](std::shared_ptr<http::websocket_connection> ws) {
                    ws->on_message([](std::string, bool) {});
                });
            });
            // A middleware answering before the body is read
            server.use([](http::request& req, http::response& res, std::function<void()> next) {
                if (req.get_http_request()->get_path() == "/private") {
                    res.error(http::http_response::status::unauthorized, "Unauthorized");
                    return;
                }
                next();
            });
            server.post("/private", [](http::response& res) { res.send("private"); });

            running = std::make_unique<running_server>(server, kind);
        }

        const transport_endpoint& endpoint() const { return running->endpoint(); }
    };

    std::string get(const std::string& path, const std::string& headers = "", const std::string& version = "1.1") {
        return "GET " + path + " HTTP/" + version + "\r\nHost: localhost\r\n" + headers + "\r\n";
    }

    std::string post(const std::string& path, const std::string& headers) {
        return "POST " + path + " HTTP/1.1\r\nHost: localhost\r\n" + headers + "\r\n";
    }

    // The head of the last response in `data` (a 100 Continue before it is not one)
    std::string last_head(const std::string& data) {
        auto start = data.rfind("HTTP/1.1 ");
        if (start == std::string::npos) return {};
        auto end = data.find("\r\n\r\n", start);
        return data.substr(start, end == std::string::npos ? std::string::npos : end + 4 - start);
    }

    bool contains(const std::string& data, const std::string& needle) {
        return data.find(needle) != std::string::npos;
    }

    // One way a connection closes after a response
    struct close_row {
        const char* name;
        close_reason reason;
        // Whether the head says so: false when the reason comes once the head was written
        bool head_announces = true;
        // Send what provokes it; the path whose response closes the connection
        std::function<std::string(table_server&, http::test::raw_connection&)> provoke;
    };

    std::vector<close_row> close_rows() {
        return {
            {"Connection: close", close_reason::client_asked, true, [](table_server&, auto& connection) {
                REQUIRE(connection.write(get("/ok", "Connection: close\r\n")));
                return std::string("/ok");
            }},
            {"HTTP/1.0 without keep-alive", close_reason::http10, true, [](table_server&, auto& connection) {
                REQUIRE(connection.write(get("/ok", "", "1.0")));
                return std::string("/ok");
            }},
            {"A drain starting while the request is handled", close_reason::draining, true,
             [](table_server& fixture, auto& connection) {
                REQUIRE(connection.write(get("/slow")));
                REQUIRE(eventually([&] { return fixture.slow_started.load(); }));
                fixture.shutdown.emplace(fixture.server, 5s);
                return std::string("/slow");
            }},
            {"Answered before the 100 Continue its client waits for", close_reason::continue_skipped, true,
             [](table_server&, auto& connection) {
                REQUIRE(connection.write(post("/denied", "Expect: 100-continue\r\nContent-Length: 1000\r\n")));
                return std::string("/denied");
            }},
            {"A body left unread: an expectation that cannot be met", close_reason::body_unread, true,
             [](table_server&, auto& connection) {
                REQUIRE(connection.write(post("/echo", "Expect: something-else\r\nContent-Length: 1000\r\n")));
                return std::string("/echo");
            }},
            {"A body that cannot be read: a malformed chunk", close_reason::body_error, true,
             [](table_server&, auto& connection) {
                REQUIRE(connection.write(post("/echo", "Transfer-Encoding: chunked\r\n") + "zz\r\nbody\r\n"));
                return std::string("/echo");
            }},
            {"A handler failing after responding, behind a pipelined response", close_reason::handler_failed, true,
             [](table_server&, auto& connection) {
                REQUIRE(connection.write(get("/hold") + get("/fail")));
                return std::string("/fail");
            }},
            {"A route timeout with the body still to come", close_reason::route_timeout, true,
             [](table_server&, auto& connection) {
                REQUIRE(connection.write(post("/timeout", "Content-Length: 1000\r\n")));
                return std::string("/timeout");
            }},
            {"A streamed body ended by the close (HTTP/1.0 keep-alive client)", close_reason::framing_by_close, true,
             [](table_server&, auto& connection) {
                REQUIRE(connection.write(get("/stream", "Connection: keep-alive\r\n", "1.0")));
                return std::string("/stream");
            }},
            {"A body ended by the close, on a connection taken over", close_reason::framing_by_close, true,
             [](table_server&, auto& connection) {
                REQUIRE(connection.write(get("/raw")));
                return std::string("/raw");
            }},
            {"A streamed response aborted, behind a pipelined response", close_reason::aborted, true,
             [](table_server&, auto& connection) {
                REQUIRE(connection.write(get("/hold") + get("/abort")));
                return std::string("/abort");
            }},
            // The reason comes once the head was written: it cannot be amended
            {"A streamed response aborted once its head was written", close_reason::aborted, false,
             [](table_server&, auto& connection) {
                REQUIRE(connection.write(get("/abort-now")));
                return std::string("/abort-now");
            }},
            {"A drain starting while a response is written", close_reason::draining, false,
             [](table_server& fixture, auto& connection) {
                REQUIRE(connection.write(get("/hold")));
                REQUIRE(eventually([&] { return fixture.held.held.load(); }));
                fixture.shutdown.emplace(fixture.server, 5s);
                // The drain reaches the connection on its thread
                std::this_thread::sleep_for(200ms);
                fixture.held.release();
                return std::string("/hold");
            }},
            {"A body too large to drop, after a middleware answered", close_reason::body_unread, false,
             [](table_server&, auto& connection) {
                REQUIRE(connection.write(post("/private", "Content-Length: " + std::to_string(4 * MiB) + "\r\n")));
                return std::string("/private");
            }},
        };
    }

}

TEST_CASE("The table covers every close reason", "[server][disposition][integration]") {
    std::set<close_reason> announced;
    for (const auto& row : close_rows()) {
        if (row.head_announces) announced.insert(row.reason);
    }
    REQUIRE(announced.size() == 10);
}

TEST_CASE("The Connection header tells the close, for every reason", "[server][disposition][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    auto row = GENERATE(from_range(close_rows()));
    INFO("row: " << row.name << " (" << to_string(row.reason) << ")");

    table_server fixture(kind);
    http::test::raw_connection connection(fixture.endpoint());
    auto path = row.provoke(fixture, connection);

    // The server closes the connection after the response
    auto result = connection.read_until_closed(5s);
    INFO("received: " << result.data.substr(0, 600));
    REQUIRE(result.closed);

    // For the reason of the row, the first one its stream got
    auto seen = fixture.probe.of(path);
    REQUIRE(seen);
    INFO("recorded: " << to_string(seen->reason));
    REQUIRE(seen->reason == row.reason);

    // The head of that response says so, once
    auto head = last_head(result.data);
    REQUIRE_FALSE(head.empty());
    REQUIRE(count(head, "Connection:") == 1);
    if (row.head_announces) {
        REQUIRE(contains(head, "Connection: Close\r\n"));
    } else {
        // Written before the reason came (§4.5): it said keep-alive, and the connection closes
        REQUIRE(contains(head, "Connection: Keep-Alive\r\n"));
    }

    // A response pipelined ahead of it kept the connection
    if (path == "/fail" || path == "/abort") {
        auto ahead = fixture.probe.of("/hold");
        REQUIRE(ahead);
        REQUIRE_FALSE(ahead->reason);
        REQUIRE(result.data.starts_with("HTTP/1.1 200"));
        REQUIRE(contains(result.data.substr(0, result.data.find("\r\n\r\n") + 2), "Connection: Keep-Alive\r\n"));
    }
}

TEST_CASE("Frames that do not close the connection do not say so", "[server][disposition][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    table_server fixture(kind);
    http::test::raw_connection connection(fixture.endpoint());
    std::string data;

    SECTION("Kept alive: no Connection: Close, and a second request is served") {
        REQUIRE(connection.write(get("/ok")));
        REQUIRE(connection.read_until(data, "\r\n\r\nok", 3s));
        auto head = last_head(data);
        REQUIRE(contains(head, "Connection: Keep-Alive\r\n"));
        REQUIRE_FALSE(contains(head, "Close"));

        REQUIRE(connection.write(get("/ok")));
        REQUIRE(eventually([&] {
            connection.read_some(data, 100ms);
            return count(data, "HTTP/1.1 200") == 2 && data.ends_with("ok");
        }, 3s));
        REQUIRE_FALSE(contains(data, "Connection: Close"));
        // and the connection stays open
        connection.read_some(data, 200ms);
        REQUIRE_FALSE(connection.eof());

        auto seen = fixture.probe.of("/ok");
        REQUIRE(seen);
        REQUIRE_FALSE(seen->reason);
        REQUIRE(seen->disposition == after_response::keep_alive);
    }

    SECTION("A 100 Continue, with a close pending, does not carry it; the final head does") {
        REQUIRE(connection.write(post("/echo", "Connection: close\r\nExpect: 100-continue\r\nContent-Length: 5\r\n")));
        const std::string continue_line = "HTTP/1.1 100 Continue\r\n\r\n";
        REQUIRE(connection.read_until(data, continue_line, 3s));
        REQUIRE(data == continue_line);

        REQUIRE(connection.write("hello"));
        auto rest = connection.read_until_closed(5s);
        INFO("received: " << rest.data);
        REQUIRE(rest.closed);
        REQUIRE(rest.data.starts_with("HTTP/1.1 200"));
        REQUIRE(contains(rest.data, "Connection: Close\r\n"));
        REQUIRE(count(rest.data, "Connection:") == 1);
        REQUIRE(rest.data.ends_with("size=5"));

        auto seen = fixture.probe.of("/echo");
        REQUIRE(seen);
        REQUIRE(seen->reason == close_reason::client_asked);
    }

    SECTION("Body data, with a close pending, does not carry it; the head does") {
        REQUIRE(connection.write(get("/chunks", "Connection: close\r\n")));
        auto result = connection.read_until_closed(5s);
        INFO("received: " << result.data);
        REQUIRE(result.closed);
        auto head = last_head(result.data);
        REQUIRE(contains(head, "Connection: Close\r\n"));
        REQUIRE(count(result.data, "Connection:") == 1);
        REQUIRE(result.data.substr(head.size()) == "3\r\none\r\n3\r\ntwo\r\n0\r\n\r\n");
    }

    SECTION("A protocol switch keeps its Connection: Upgrade, with a close pending") {
        REQUIRE(connection.write(get("/ws", "Upgrade: websocket\r\nConnection: Upgrade, close\r\n"
                                            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                            "Sec-WebSocket-Version: 13\r\n")));
        REQUIRE(connection.read_until(data, "\r\n\r\n", 3s));
        auto head = last_head(data);
        INFO("head: " << head);
        REQUIRE(head.starts_with("HTTP/1.1 101"));
        REQUIRE(contains(head, "Connection: Upgrade\r\n"));
        REQUIRE(count(head, "Connection:") == 1);
        REQUIRE_FALSE(contains(head, "Close"));
        REQUIRE_FALSE(contains(head, "Keep-Alive"));

        auto seen = fixture.probe.of("/ws");
        REQUIRE(seen);
        REQUIRE(seen->disposition == after_response::take_over);
        REQUIRE(seen->reason == close_reason::client_asked);
        connection.close();
    }
}

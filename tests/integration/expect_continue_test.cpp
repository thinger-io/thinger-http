#include "server_test_helpers.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/reverse_proxy.hpp>
#include <thinger/util/types.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

using namespace thinger;
using namespace std::chrono_literals;
using namespace server_test;

// Expect: 100-continue (RFC 9110 section 10.1.1): the server asks for the body with a
// 100 Continue once it reads it, never when it answers without reading it, and answers
// 417 to any other expectation

namespace {

    const std::string continue_line = "HTTP/1.1 100 Continue\r\n\r\n";

    // A client connection reading with timeouts
    struct raw_connection {
        boost::asio::io_context ioc;
        boost::asio::ip::tcp::socket socket{ioc};
        std::string pending;
        bool closed = false;

        explicit raw_connection(uint16_t port) {
            socket.connect({boost::asio::ip::make_address("127.0.0.1"), port});
        }

        void send(std::string_view data) {
            boost::asio::write(socket, boost::asio::buffer(data.data(), data.size()));
        }

        // Read once more into `pending`; false on timeout or once the connection is closed
        bool read_more(std::chrono::milliseconds timeout) {
            if (closed) return false;
            char data[16384];
            boost::system::error_code result;
            size_t bytes = 0;
            bool done = false;
            socket.async_read_some(boost::asio::buffer(data), [&](boost::system::error_code ec, size_t n) {
                result = ec;
                bytes = n;
                done = true;
            });
            ioc.restart();
            ioc.run_for(timeout);
            if (!done) {
                socket.cancel();
                ioc.restart();
                ioc.run();
                return false;
            }
            pending.append(data, bytes);
            if (result) closed = true;
            return !result;
        }

        bool read_until(const std::string& needle, std::chrono::milliseconds timeout = 5s) {
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (pending.find(needle) == std::string::npos) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (left.count() <= 0 || !read_more(left)) return pending.find(needle) != std::string::npos;
            }
            return true;
        }

        // Whether the peer closes the connection within the timeout
        bool read_until_closed(std::chrono::milliseconds timeout = 5s) {
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!closed) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (left.count() <= 0) return false;
                read_more(left);
            }
            return true;
        }

        // Remove and return what `pending` holds up to the end of `needle`
        std::string take_until(const std::string& needle) {
            auto position = pending.find(needle);
            REQUIRE(position != std::string::npos);
            auto taken = pending.substr(0, position + needle.size());
            pending.erase(0, position + needle.size());
            return taken;
        }
    };

    std::string post_head(const std::string& path, const std::string& headers, const std::string& version = "1.1") {
        return "POST " + path + " HTTP/" + version + "\r\nHost: localhost\r\n" + headers + "\r\n";
    }

    std::string expect_head(const std::string& path, size_t length, const std::string& expect = "100-continue") {
        return post_head(path, "Expect: " + expect + "\r\nContent-Length: " + std::to_string(length) + "\r\n");
    }

    // Server with routes reading the body in every way, and some answering without reading it
    struct continue_server {
        http::server server;
        std::unique_ptr<running_server> running;
        uint16_t port = 0;
        std::atomic<int> handled{0};

        explicit continue_server(size_t max_body_size = http::default_max_body_size) {
            server.set_max_body_size(max_body_size);
            // The server reads the body before the handler
            server.post("/echo", [this](http::request& req, http::response& res) {
                handled++;
                res.send("body=[" + req.body() + "]");
            });
            // The handler reads it, as it arrives
            server.post("/stream", [this](http::request& req, http::response& res) -> awaitable<void> {
                handled++;
                std::string body;
                uint8_t buffer[7];
                while (size_t bytes = co_await req.read_some(buffer, sizeof(buffer))) {
                    body.append(reinterpret_cast<char*>(buffer), bytes);
                }
                // and streams its response
                http::http_response head;
                res.start_stream(head);
                co_await res.write_stream("streamed=[" + body + "]");
                co_await res.end_stream();
            });
            // Decoded as JSON before the handler
            server.post("/json", [this](nlohmann::json& body, http::response& res) {
                handled++;
                res.json({{"value", body["value"]}});
            });
            // Answers without reading the body
            server.post("/denied", [this](http::request&, http::response& res) -> awaitable<void> {
                handled++;
                res.error(http::http_response::status::unauthorized, "Unauthorized");
                co_return;
            });
            // Small limit
            server.post("/small", [this](http::request& req, http::response& res) {
                handled++;
                res.send("body=[" + req.body() + "]");
            }).max_body_size(16);
            server.get("/health", [](http::response& res) {
                res.send("healthy");
            });
            // A middleware rejecting requests without credentials, before reading anything
            server.use([](http::request& req, http::response& res, std::function<void()> next) {
                if (req.get_http_request()->get_path() == "/private" && req.header("Authorization").empty()) {
                    res.error(http::http_response::status::unauthorized, "Unauthorized");
                    return;
                }
                next();
            });
            server.post("/private", [this](http::request& req, http::response& res) {
                handled++;
                res.send("body=[" + req.body() + "]");
            });
        }

        void start() {
            running = std::make_unique<running_server>(server);
            port = running->port;
        }
    };

    // The server asks for the body: 100 Continue arrives while the client still holds it back
    void require_continue(raw_connection& connection) {
        REQUIRE(connection.read_until(continue_line, 3s));
        REQUIRE(connection.pending.starts_with(continue_line));
        connection.take_until(continue_line);
    }

    // Nothing arrives within `wait`
    void require_silence(raw_connection& connection, std::chrono::milliseconds wait = 300ms) {
        connection.read_more(wait);
        REQUIRE(connection.pending.empty());
        REQUIRE_FALSE(connection.closed);
    }

    // The response ends the connection: final status `status`, no 100 Continue before it, and
    // Connection: close
    void require_answered_without_continue(raw_connection& connection, const std::string& status) {
        REQUIRE(connection.read_until_closed(3s));
        INFO("response: " << connection.pending);
        REQUIRE(connection.pending.starts_with("HTTP/1.1 " + status));
        REQUIRE(connection.pending.find("100 Continue") == std::string::npos);
        REQUIRE(count(connection.pending, "HTTP/1.1 ") == 1);
        REQUIRE(connection.pending.find("Connection: Close") != std::string::npos);
    }

}

TEST_CASE("100 Continue is sent once the body is read", "[server][expect][integration]") {
    continue_server fixture;
    fixture.start();
    raw_connection connection(fixture.port);

    SECTION("Read by the server before the handler") {
        connection.send(expect_head("/echo", 5));
        require_continue(connection);
        connection.send("hello");
        REQUIRE(connection.read_until("body=[hello]"));
    }

    SECTION("The expectation is case-insensitive, also in a list") {
        connection.send(expect_head("/echo", 5, "100-Continue"));
        require_continue(connection);
        connection.send("hello");
        REQUIRE(connection.read_until("body=[hello]"));
        connection.pending.clear();

        connection.send(expect_head("/echo", 3, " 100-CONTINUE , x-other"));
        require_continue(connection);
        connection.send("bye");
        REQUIRE(connection.read_until("body=[bye]"));
    }

    SECTION("Read by the handler as it arrives, with a streamed response") {
        connection.send(expect_head("/stream", 11));
        require_continue(connection);
        connection.send("hello world");
        REQUIRE(connection.read_until("streamed=[hello world]"));
        REQUIRE(connection.read_until("0\r\n\r\n"));
    }

    SECTION("Chunked body read by the handler") {
        connection.send(post_head("/stream", "Expect: 100-continue\r\nTransfer-Encoding: chunked\r\n"));
        require_continue(connection);
        connection.send("5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
        REQUIRE(connection.read_until("streamed=[hello world]"));
        REQUIRE(connection.read_until("0\r\n\r\n"));
    }

    SECTION("Decoded as JSON before the handler") {
        std::string body = R"({"value":42})";
        connection.send(post_head("/json", "Expect: 100-continue\r\nContent-Type: application/json\r\nContent-Length: " +
                                               std::to_string(body.size()) + "\r\n"));
        require_continue(connection);
        connection.send(body);
        REQUIRE(connection.read_until(R"({"value":42})"));
    }

    SECTION("The client sent the body without waiting: no 100 Continue") {
        connection.send(expect_head("/echo", 5) + "hello");
        REQUIRE(connection.read_until("body=[hello]"));
        REQUIRE(connection.pending.find("100 Continue") == std::string::npos);
    }

    // The connection is kept alive, and the next requests served
    REQUIRE(connection.read_until("\r\n\r\n"));
    connection.pending.clear();
    connection.send("GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
    REQUIRE(connection.read_until("healthy"));
    REQUIRE(connection.pending.starts_with("HTTP/1.1 200"));
    REQUIRE(connection.pending.find("100 Continue") == std::string::npos);
}

TEST_CASE("A response sent without reading the body sends no 100 Continue and closes", "[server][expect][integration]") {
    continue_server fixture;
    fixture.start();
    raw_connection connection(fixture.port);

    SECTION("Handler answering 401") {
        connection.send(expect_head("/denied", 1000));
        require_answered_without_continue(connection, "401");
        REQUIRE(fixture.handled == 1);
    }

    SECTION("Middleware answering 401") {
        connection.send(expect_head("/private", 1000));
        require_answered_without_continue(connection, "401");
        REQUIRE(fixture.handled == 0);
    }

    SECTION("No route: 404") {
        connection.send(expect_head("/missing", 1000));
        require_answered_without_continue(connection, "404");
    }

    SECTION("Unsupported media type: 415") {
        connection.send(post_head("/json", "Expect: 100-continue\r\nContent-Type: text/csv\r\nContent-Length: 10\r\n"));
        require_answered_without_continue(connection, "415");
    }

    SECTION("Content-Length over the limit of the route: 413") {
        connection.send(expect_head("/small", 1000));
        require_answered_without_continue(connection, "413");
        REQUIRE(fixture.handled == 0);
    }

}

TEST_CASE("A Content-Length over the server limit is answered 413 without 100 Continue", "[server][expect][integration]") {
    continue_server fixture(100);
    fixture.start();
    raw_connection connection(fixture.port);
    connection.send(expect_head("/echo", 1000));
    require_answered_without_continue(connection, "413");
    REQUIRE(fixture.handled == 0);
}

TEST_CASE("Other expectations are answered with 417", "[server][expect][integration]") {
    continue_server fixture;
    fixture.start();
    raw_connection connection(fixture.port);

    SECTION("With a body: the connection is closed") {
        connection.send(expect_head("/echo", 5, "foo"));
        require_answered_without_continue(connection, "417");
        REQUIRE(fixture.handled == 0);
    }

    SECTION("Without a body: the connection is kept") {
        connection.send("GET /health HTTP/1.1\r\nHost: localhost\r\nExpect: something\r\n\r\n");
        REQUIRE(connection.read_until("\r\n\r\n"));
        REQUIRE(connection.pending.starts_with("HTTP/1.1 417"));
        REQUIRE(connection.read_until("Expectation Failed"));
        connection.pending.clear();
        connection.send("GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
        REQUIRE(connection.read_until("healthy"));
    }

    SECTION("An empty expectation is ignored") {
        connection.send(post_head("/echo", "Expect: \r\nContent-Length: 5\r\n") + "hello");
        REQUIRE(connection.read_until("body=[hello]"));
        REQUIRE(connection.pending.starts_with("HTTP/1.1 200"));
    }
}

TEST_CASE("Expect is ignored in HTTP/1.0 requests", "[server][expect][integration]") {
    continue_server fixture;
    fixture.start();
    raw_connection connection(fixture.port);

    SECTION("100-continue") {
        connection.send(post_head("/echo", "Expect: 100-continue\r\nContent-Length: 5\r\n", "1.0"));
        require_silence(connection);
        connection.send("hello");
    }

    SECTION("Another expectation") {
        connection.send(post_head("/echo", "Expect: foo\r\nContent-Length: 5\r\n", "1.0"));
        require_silence(connection);
        connection.send("hello");
    }

    REQUIRE(connection.read_until_closed(3s));
    REQUIRE(connection.pending.starts_with("HTTP/1.1 200"));
    REQUIRE(connection.pending.find("body=[hello]") != std::string::npos);
    REQUIRE(connection.pending.find("100 Continue") == std::string::npos);
}

TEST_CASE("100 Continue is written in order with pipelined responses", "[server][expect][pipelining][integration]") {
    continue_server fixture;
    fixture.start();
    raw_connection connection(fixture.port);

    SECTION("After the response of a previous request") {
        connection.send("GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n" + expect_head("/echo", 5));
        REQUIRE(connection.read_until(continue_line));
        auto health = connection.pending.find("healthy");
        REQUIRE(health != std::string::npos);
        REQUIRE(health < connection.pending.find(continue_line));
        connection.take_until(continue_line);
        connection.send("hello");
        REQUIRE(connection.read_until("body=[hello]"));
    }

    SECTION("Followed by a pipelined request, sent with the body") {
        connection.send(expect_head("/echo", 5));
        require_continue(connection);
        connection.send("hello" + expect_head("/echo", 3) + "bye" + "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
        REQUIRE(connection.read_until("healthy"));
        auto first = connection.pending.find("body=[hello]");
        auto second = connection.pending.find("body=[bye]");
        REQUIRE(first != std::string::npos);
        REQUIRE(second != std::string::npos);
        REQUIRE(first < second);
        // the second request carried its body: no 100 Continue for it
        REQUIRE(connection.pending.find("100 Continue") == std::string::npos);
    }

    SECTION("Several requests waiting for it, one after the other") {
        for (int i = 0; i < 3; ++i) {
            connection.send(expect_head("/echo", 1));
            require_continue(connection);
            connection.send(std::to_string(i));
            REQUIRE(connection.read_until("body=[" + std::to_string(i) + "]"));
            REQUIRE(count(connection.pending, "HTTP/1.1 ") == 1);
            connection.pending.clear();
        }
    }
}

TEST_CASE("Expect in requests dispatched in memory", "[server][expect][memory][integration]") {
    continue_server fixture;

    SECTION("100-continue: the body is there") {
        auto request = make_request(http::method::POST, "/echo", "hello");
        request->process_header("Expect", "100-continue");
        auto response = run_dispatch(fixture.server, request);
        REQUIRE(response->get_status() == http::http_response::status::ok);
        REQUIRE(response->get_content() == "body=[hello]");
    }

    SECTION("Another expectation: 417") {
        auto request = make_request(http::method::POST, "/echo", "hello");
        request->process_header("Expect", "foo");
        auto response = run_dispatch(fixture.server, request);
        REQUIRE(response->get_status() == http::http_response::status::expectation_failed);
    }
}

TEST_CASE("Reverse proxy asks for the body once it streams it to the target", "[server][expect][proxy][integration]") {
    // Target: echoes the body, and whether it got Expect
    http::server target;
    target.post("/upload", [](http::request& req, http::response& res) {
        res.send("expect=[" + req.header("Expect") + "] body=[" + req.body() + "]");
    });
    running_server running_target(target);

    http::reverse_proxy reverse_proxy;
    http::server proxy;
    auto destination = http::proxy_target::from_url("http://127.0.0.1:" + std::to_string(running_target.port));
    REQUIRE(destination);
    proxy.set_not_found_handler([&](http::request& req, http::response& res) -> awaitable<void> {
        co_await reverse_proxy.forward(req, res, *destination);
    });
    // A target nobody listens on
    auto unreachable = http::proxy_target::from_url("http://127.0.0.1:1");
    REQUIRE(unreachable);
    proxy.post("/down", [&](http::request& req, http::response& res) -> awaitable<void> {
        co_await reverse_proxy.forward(req, res, *unreachable);
    });
    running_server running_proxy(proxy);
    raw_connection connection(running_proxy.port);

    SECTION("A single 100 Continue, from the proxy; Expect is not forwarded") {
        connection.send(expect_head("/upload", 5));
        require_continue(connection);
        connection.send("hello");
        REQUIRE(connection.read_until("body=[hello]"));
        REQUIRE(connection.pending.starts_with("HTTP/1.1 200"));
        REQUIRE(connection.pending.find("expect=[]") != std::string::npos);
        REQUIRE(connection.pending.find("100 Continue") == std::string::npos);

        // kept alive
        connection.pending.clear();
        connection.send(expect_head("/upload", 3));
        require_continue(connection);
        connection.send("bye");
        REQUIRE(connection.read_until("body=[bye]"));
    }

    SECTION("Target down: 502 without asking for the body") {
        connection.send(expect_head("/down", 1000));
        require_answered_without_continue(connection, "502");
    }
}

TEST_CASE("curl uploads do not wait for 100 Continue", "[server][expect][curl][integration]") {
    if (std::system("curl --version > /dev/null 2>&1") != 0) {
        SKIP("curl is not available");
    }

    http::server server;
    std::atomic<size_t> received{0};
    server.put("/upload", [&](http::request& req, http::response& res) -> awaitable<void> {
        uint8_t buffer[16384];
        size_t total = 0;
        while (size_t bytes = co_await req.read_some(buffer, sizeof(buffer))) total += bytes;
        received = total;
        res.send(std::to_string(total));
    });
    running_server running(server);

    auto path = std::filesystem::temp_directory_path() /
                ("thinger_expect_" + std::to_string(running.port) + ".bin");
    {
        std::ofstream file(path, std::ios::binary);
        file << std::string(2 * 1024 * 1024, 'x');
    }

    // Asked explicitly, as the size from which curl sends Expect changed between versions
    auto command = "curl -s -o /dev/null -w '%{http_code}' -H 'Expect: 100-continue' --expect100-timeout 5 -T " +
                   path.string() + " http://127.0.0.1:" + std::to_string(running.port) + "/upload";
    auto start = std::chrono::steady_clock::now();
    std::string output;
    if (FILE* pipe = popen(command.c_str(), "r")) {
        char buffer[64];
        while (size_t bytes = fread(buffer, 1, sizeof(buffer), pipe)) output.append(buffer, bytes);
        pclose(pipe);
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    std::filesystem::remove(path);

    INFO("curl output: " << output);
    REQUIRE(output == "200");
    REQUIRE(received == 2 * 1024 * 1024);
    // Without a 100 Continue, curl would wait its 5 s timeout before sending the body
    REQUIRE(elapsed < 2s);
}

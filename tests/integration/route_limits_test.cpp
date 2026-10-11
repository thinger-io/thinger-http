#include "server_test_helpers.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/reverse_proxy.hpp>
#include <thinger/http/server/sse_connection.hpp>
#include <thinger/util/types.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

using namespace thinger;
using namespace std::chrono_literals;
using namespace server_test;

// Per-route (and per-group) body size limits and request timeouts

namespace {

    using clock = std::chrono::steady_clock;

    // Wait on a timer from a coroutine handler
    thinger::awaitable<void> sleep_for(std::chrono::milliseconds duration) {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, duration);
        co_await timer.async_wait(thinger::use_nothrow_awaitable);
    }

    std::string post(const std::string& path, const std::string& body, const std::string& extra_headers = "") {
        return "POST " + path + " HTTP/1.1\r\nHost: localhost\r\n" + extra_headers +
               "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    }

    std::string post_chunked(const std::string& path, const std::string& body, size_t chunk_size = 1000) {
        std::string request = "POST " + path + " HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n";
        for (size_t offset = 0; offset < body.size(); offset += chunk_size) {
            auto chunk = body.substr(offset, chunk_size);
            char size[20];
            std::snprintf(size, sizeof(size), "%zx", chunk.size());
            request += std::string(size) + "\r\n" + chunk + "\r\n";
        }
        return request + "0\r\n\r\n";
    }

    // A JSON string of about `size` bytes
    std::string json_body(size_t size) {
        return "\"" + std::string(size - 2, 'x') + "\"";
    }

    // A connection to the server sending requests and reading whole responses, to check
    // what happens on the same connection (keep-alive)
    struct connection {
        boost::asio::io_context ioc;
        boost::asio::ip::tcp::socket sock{ioc};
        std::string buffer;
        bool closed = false;

        explicit connection(uint16_t port) {
            sock.connect({boost::asio::ip::make_address("127.0.0.1"), port});
        }

        void send(const std::string& data) {
            boost::system::error_code ec;
            boost::asio::write(sock, boost::asio::buffer(data), ec);
        }

        // Size of the first complete response in the buffer (Content-Length or chunked), 0 if
        // it is not complete yet
        size_t complete_response() const {
            auto head_end = buffer.find("\r\n\r\n");
            if (head_end == std::string::npos) return 0;
            auto head = buffer.substr(0, head_end);
            for (auto& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            size_t body_start = head_end + 4;
            if (auto pos = head.find("content-length:"); pos != std::string::npos) {
                size_t length = std::stoul(head.substr(pos + 15));
                return buffer.size() >= body_start + length ? body_start + length : 0;
            }
            if (head.find("transfer-encoding: chunked") != std::string::npos) {
                auto end = buffer.find("\r\n0\r\n\r\n", body_start - 2);
                return end == std::string::npos ? 0 : end + 7;
            }
            return body_start;
        }

        // Read one whole response (empty if none arrives within the timeout)
        std::string read_response(std::chrono::milliseconds timeout = 3s) {
            std::array<char, 4096> data{};
            std::function<void()> read_more = [&]() {
                if (complete_response() > 0 || closed) return;
                sock.async_read_some(boost::asio::buffer(data), [&](boost::system::error_code ec, size_t bytes) {
                    buffer.append(data.data(), bytes);
                    if (ec) {
                        closed = true;
                        return;
                    }
                    read_more();
                });
            };
            read_more();
            ioc.restart();
            ioc.run_for(timeout);
            size_t size = complete_response();
            if (size == 0) {
                // stop a pending read
                sock.cancel();
                ioc.restart();
                ioc.run_for(100ms);
                return {};
            }
            auto response = buffer.substr(0, size);
            buffer.erase(0, size);
            return response;
        }

        // Whether the server closes the connection within the timeout
        bool wait_closed(std::chrono::milliseconds timeout = 3s) {
            std::array<char, 4096> data{};
            std::function<void()> read_more = [&]() {
                sock.async_read_some(boost::asio::buffer(data), [&](boost::system::error_code ec, size_t bytes) {
                    buffer.append(data.data(), bytes);
                    if (ec) {
                        closed = true;
                        return;
                    }
                    read_more();
                });
            };
            if (!closed) {
                read_more();
                ioc.restart();
                ioc.run_for(timeout);
            }
            return closed;
        }
    };

    bool has_header(const std::string& response, const std::string& header) {
        auto lower = response.substr(0, response.find("\r\n\r\n"));
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto wanted = header;
        for (auto& c : wanted) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return lower.find("\r\n" + wanted) != std::string::npos;
    }

}

// --- Body size ---

TEST_CASE("Routes can accept bodies larger or smaller than the application limit", "[server][body][limits][413][integration]") {
    http::server server;
    server.set_max_body_size(1024);

    auto size_handler = [](http::request& req, http::response& res) {
        res.send("size=" + std::to_string(req.body().size()));
    };
    server.post("/default", size_handler);
    server.post("/large", size_handler).max_body_size(64 * 1024);
    server.post("/tiny", size_handler).max_body_size(100);
    server.post("/unlimited", size_handler).max_body_size(std::numeric_limits<size_t>::max());
    server.post("/json", [](nlohmann::json& body, http::response& res) {
        res.send("size=" + std::to_string(body.get<std::string>().size()));
    }).max_body_size(64 * 1024);
    running_server running(server);

    auto body = std::string(4000, 'x');

    SECTION("Content-Length over the application limit, within the route one") {
        auto rejected = raw_exchange(running.port, post("/default", body));
        CHECK(rejected.data.starts_with("HTTP/1.1 413"));
        CHECK(rejected.closed);

        auto accepted = raw_exchange(running.port, post("/large", body, "Connection: close\r\n"));
        CHECK(accepted.data.starts_with("HTTP/1.1 200"));
        CHECK(accepted.data.find("size=4000") != std::string::npos);

        accepted = raw_exchange(running.port, post("/unlimited", body, "Connection: close\r\n"));
        CHECK(accepted.data.find("size=4000") != std::string::npos);
    }

    SECTION("Chunked body over the application limit, within the route one") {
        auto rejected = raw_exchange(running.port, post_chunked("/default", body));
        CHECK(rejected.data.starts_with("HTTP/1.1 413"));
        CHECK(rejected.closed);

        connection conn(running.port);
        conn.send(post_chunked("/large", body));
        auto response = conn.read_response();
        CHECK(response.starts_with("HTTP/1.1 200"));
        CHECK(response.find("size=4000") != std::string::npos);
    }

    SECTION("Decoded JSON bodies") {
        auto json = json_body(4000);
        auto accepted = raw_exchange(running.port, post("/json", json, "Content-Type: application/json\r\nConnection: close\r\n"));
        CHECK(accepted.data.find("size=3998") != std::string::npos);

        connection conn(running.port);
        conn.send(post_chunked("/json", json));
        CHECK(conn.read_response().find("size=3998") != std::string::npos);
        CHECK(raw_exchange(running.port, post("/json", json_body(70000))).data.starts_with("HTTP/1.1 413"));
    }

    SECTION("A route limit smaller than the application one") {
        auto rejected = raw_exchange(running.port, post("/tiny", std::string(200, 'x')));
        CHECK(rejected.data.starts_with("HTTP/1.1 413"));
        rejected = raw_exchange(running.port, post_chunked("/tiny", std::string(200, 'x'), 50));
        CHECK(rejected.data.starts_with("HTTP/1.1 413"));
        auto accepted = raw_exchange(running.port, post("/tiny", std::string(100, 'x'), "Connection: close\r\n"));
        CHECK(accepted.data.find("size=100") != std::string::npos);
    }

    SECTION("A Content-Length over the limit is rejected before the body is sent") {
        // Only the headers: the answer cannot wait for the body
        auto rejected = raw_exchange(running.port,
            "POST /large HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100000\r\n\r\n", 2s);
        CHECK(rejected.data.starts_with("HTTP/1.1 413"));
        CHECK(rejected.closed);
    }
}

TEST_CASE("Groups set the body limit of their routes", "[server][body][limits][group][integration]") {
    http::server server;
    server.set_max_body_size(1024);
    auto size_handler = [](http::request& req, http::response& res) {
        res.send("size=" + std::to_string(req.body().size()));
    };
    auto imports = server.group("/imports").max_body_size(64 * 1024);
    imports.post("/data", size_handler);
    imports.post("/small", size_handler).max_body_size(512);
    imports.group("/nested").post("/data", size_handler);
    running_server running(server);

    auto body = std::string(4000, 'x');
    CHECK(raw_exchange(running.port, post("/imports/data", body, "Connection: close\r\n")).data.find("size=4000") != std::string::npos);
    CHECK(raw_exchange(running.port, post("/imports/nested/data", body, "Connection: close\r\n")).data.find("size=4000") != std::string::npos);
    CHECK(raw_exchange(running.port, post_chunked("/imports/data", body)).data.find("size=4000") != std::string::npos);
    CHECK(raw_exchange(running.port, post("/imports/small", body)).data.starts_with("HTTP/1.1 413"));
    CHECK(raw_exchange(running.port, post_chunked("/imports/small", body)).data.starts_with("HTTP/1.1 413"));
}

TEST_CASE("Routes reading the body themselves are bounded by the route limit", "[server][body][limits][deferred][integration]") {
    http::server server;
    server.set_max_body_size(1024);
    std::atomic<size_t> read_total{0};
    std::atomic<int> errors{0};
    auto stream_handler = [&](http::request& req, http::response& res) -> thinger::awaitable<void> {
        uint8_t buffer[512];
        size_t total = 0;
        while (size_t bytes = co_await req.read_some(buffer, sizeof(buffer))) total += bytes;
        read_total = total;
        if (req.get_body_error() == http::body_error::too_large) {
            errors++;
            co_return;   // the server answers 413
        }
        res.send("read=" + std::to_string(total));
    };
    server.post("/stream", stream_handler);
    server.post("/stream-large", stream_handler).max_body_size(16 * 1024);
    server.post("/read-body", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
        if (co_await req.read_body()) res.send("size=" + std::to_string(req.body().size()));
    }).max_body_size(16 * 1024);
    running_server running(server);

    auto body = std::string(4000, 'x');

    SECTION("Content-Length: the first read fails, nothing is read") {
        auto result = raw_exchange(running.port, post("/stream", body));
        CHECK(result.data.starts_with("HTTP/1.1 413"));
        CHECK(result.closed);
        CHECK(read_total == 0);
        CHECK(errors == 1);
    }

    SECTION("Chunked: reading fails once past the limit") {
        auto result = raw_exchange(running.port, post_chunked("/stream", body, 256));
        CHECK(result.data.starts_with("HTTP/1.1 413"));
        CHECK(result.closed);
        CHECK(read_total <= 1024);
        CHECK(errors == 1);
    }

    SECTION("Within a larger route limit") {
        CHECK(raw_exchange(running.port, post("/stream-large", body, "Connection: close\r\n")).data.find("read=4000") != std::string::npos);
        CHECK(raw_exchange(running.port, post_chunked("/stream-large", body, 256)).data.find("read=4000") != std::string::npos);
        CHECK(raw_exchange(running.port, post("/read-body", body, "Connection: close\r\n")).data.find("size=4000") != std::string::npos);
        CHECK(raw_exchange(running.port, post("/stream-large", std::string(20000, 'x'))).data.starts_with("HTTP/1.1 413"));
        CHECK(raw_exchange(running.port, post("/read-body", std::string(20000, 'x'))).data.starts_with("HTTP/1.1 413"));
    }
}

TEST_CASE("The reverse proxy honors the route limit and its own, the smaller one", "[server][body][limits][proxy][integration]") {
    http::server backend;
    std::atomic<int> backend_calls{0};
    backend.post("/echo", [&](http::request& req, http::response& res) {
        backend_calls++;
        res.send("size=" + std::to_string(req.body().size()));
    });
    backend.set_max_body_size(1024 * 1024);
    running_server running_backend(backend);
    auto target = *http::proxy_target::from_url("http://127.0.0.1:" + std::to_string(running_backend.port));

    http::server server;
    server.set_max_body_size(1024);
    http::reverse_proxy proxy;
    server.post("/route-limit/echo", [&](http::request& req, http::response& res) -> thinger::awaitable<void> {
        http::proxy_options options;
        options.path = "/echo";
        co_await proxy.forward(req, res, target, options);
    }).max_body_size(1000);
    server.post("/proxy-limit/echo", [&](http::request& req, http::response& res) -> thinger::awaitable<void> {
        http::proxy_options options;
        options.path = "/echo";
        options.max_body_size = 1000;
        co_await proxy.forward(req, res, target, options);
    }).max_body_size(64 * 1024);
    server.post("/large/echo", [&](http::request& req, http::response& res) -> thinger::awaitable<void> {
        http::proxy_options options;
        options.path = "/echo";
        co_await proxy.forward(req, res, target, options);
    }).max_body_size(64 * 1024);
    running_server running(server);

    for (const auto* path : {"/route-limit/echo", "/proxy-limit/echo"}) {
        INFO(path);
        CHECK(raw_exchange(running.port, post(path, std::string(500, 'x'), "Connection: close\r\n")).data.find("size=500") != std::string::npos);
        CHECK(raw_exchange(running.port, post(path, std::string(2000, 'x'))).data.starts_with("HTTP/1.1 413"));
        CHECK(raw_exchange(running.port, post_chunked(path, std::string(2000, 'x'), 256)).data.starts_with("HTTP/1.1 413"));
    }
    // Over the application limit, within the route one
    CHECK(raw_exchange(running.port, post_chunked("/large/echo", std::string(32000, 'x'), 4000)).data.find("size=32000") != std::string::npos);
    CHECK(backend_calls == 3);
}

TEST_CASE("Virtual hosts set the limits of their routes and not-found handler", "[server][body][limits][timeout][vhost][integration]") {
    http::server backend;
    backend.set_max_body_size(1024 * 1024);
    backend.post("/echo", [](http::request& req, http::response& res) {
        res.send("size=" + std::to_string(req.body().size()));
    });
    backend.get("/slow", [](http::response& res) -> thinger::awaitable<void> {
        co_await sleep_for(1500ms);
        res.send("late");
    });
    running_server running_backend(backend);
    auto target = *http::proxy_target::from_url("http://127.0.0.1:" + std::to_string(running_backend.port));

    http::server server;
    server.set_max_body_size(1024);
    http::reverse_proxy proxy;
    auto forward = [&](http::request& req, http::response& res) -> thinger::awaitable<void> {
        co_await proxy.forward(req, res, target);
    };

    // A whole host forwarded to the backend, with larger bodies and a timeout
    server.host("proxy.example.com").max_body_size(64 * 1024).timeout(300ms).set_not_found_handler(forward);
    // The default host forwards too, with the application limits
    server.set_not_found_handler(forward);

    // Host defaults for its routes, which groups and routes override
    auto size_handler = [](http::request& req, http::response& res) {
        res.send("size=" + std::to_string(req.body().size()));
    };
    auto slow = [](http::response& res) -> thinger::awaitable<void> {
        co_await sleep_for(800ms);
        res.send("done");
    };
    auto& api = server.host("api.example.com").max_body_size(64 * 1024).timeout(150ms).decode_limits({.max_depth = 1});
    api.post("/size", size_handler);
    api.post("/small", size_handler).max_body_size(100);
    api.group("/group").max_body_size(512).post("/size", size_handler);
    api.get("/slow", slow);
    api.get("/no-timeout", slow).timeout(0ms);
    api.post("/json", [](nlohmann::json& body, http::response& res) { res.json(body); });
    running_server running(server);

    auto request = [](const std::string& host, const std::string& method, const std::string& path,
                      const std::string& body = "") {
        return method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\nContent-Length: " +
               std::to_string(body.size()) + "\r\n\r\n" + body;
    };
    auto body = std::string(4000, 'x');

    SECTION("Whole-host proxy: a body over the application limit, within the host one") {
        CHECK(raw_exchange(running.port, request("proxy.example.com", "POST", "/echo", body)).data.find("size=4000") != std::string::npos);
        auto chunked = post_chunked("/echo", body);
        chunked.replace(chunked.find("Host: localhost"), 15, "Host: proxy.example.com");
        connection conn(running.port);
        conn.send(chunked);
        CHECK(conn.read_response().find("size=4000") != std::string::npos);
        // Over the host limit
        CHECK(raw_exchange(running.port, request("proxy.example.com", "POST", "/echo", std::string(70000, 'x'))).data.starts_with("HTTP/1.1 413"));
        // The default host keeps the application limit
        CHECK(raw_exchange(running.port, request("other.example.com", "POST", "/echo", body)).data.starts_with("HTTP/1.1 413"));
    }

    SECTION("Whole-host proxy: the host timeout") {
        auto start = clock::now();
        auto result = raw_exchange(running.port, request("proxy.example.com", "GET", "/slow"));
        CHECK(result.data.starts_with("HTTP/1.1 504"));
        CHECK(clock::now() - start < 1200ms);
        // the default host has no timeout
        result = raw_exchange(running.port, request("other.example.com", "GET", "/slow"), 5s);
        CHECK(result.data.starts_with("HTTP/1.1 200"));
    }

    SECTION("Host defaults, overridden by groups and routes") {
        CHECK(raw_exchange(running.port, request("api.example.com", "POST", "/size", body)).data.find("size=4000") != std::string::npos);
        CHECK(raw_exchange(running.port, request("api.example.com", "POST", "/small", body)).data.starts_with("HTTP/1.1 413"));
        CHECK(raw_exchange(running.port, request("api.example.com", "POST", "/group/size", body)).data.starts_with("HTTP/1.1 413"));
        CHECK(raw_exchange(running.port, request("api.example.com", "GET", "/slow")).data.starts_with("HTTP/1.1 504"));
        CHECK(raw_exchange(running.port, request("api.example.com", "GET", "/no-timeout")).data.starts_with("HTTP/1.1 200"));
        CHECK(raw_exchange(running.port, request("api.example.com", "POST", "/json", "[1]")).data.starts_with("HTTP/1.1 200"));
        CHECK(raw_exchange(running.port, request("api.example.com", "POST", "/json", "[[1]]")).data.starts_with("HTTP/1.1 400"));
    }
}

TEST_CASE("In-memory dispatch honors the route limits", "[server][body][limits][dispatch][integration]") {
    http::server server;
    server.set_max_body_size(1024);
    server.post("/large", [](nlohmann::json& body, http::response& res) {
        res.send("size=" + std::to_string(body.get<std::string>().size()));
    }).max_body_size(64 * 1024);
    server.post("/tiny", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
        if (co_await req.read_body()) res.send("ok");
    }).max_body_size(100);

    auto response = run_dispatch(server, make_request(http::method::POST, "/large", json_body(4000)));
    CHECK(response->get_status_code() == 200);
    CHECK(response->get_content() == "size=3998");

    response = run_dispatch(server, make_request(http::method::POST, "/tiny", json_body(200)));
    CHECK(response->get_status_code() == 413);
}

TEST_CASE("Routes can set their own decode limits", "[server][body][limits][decode][integration]") {
    http::server server;
    auto echo = [](nlohmann::json& body, http::response& res) { res.json(body); };
    server.post("/default", echo);
    server.post("/shallow", echo).decode_limits({.max_depth = 2});
    server.group("/group").decode_limits({.max_depth = 1}).post("/shallow", echo);

    auto nested = std::string("[[[1]]]");
    CHECK(run_dispatch(server, make_request(http::method::POST, "/default", nested))->get_status_code() == 200);
    CHECK(run_dispatch(server, make_request(http::method::POST, "/shallow", nested))->get_status_code() == 400);
    CHECK(run_dispatch(server, make_request(http::method::POST, "/shallow", "[[1]]"))->get_status_code() == 200);
    CHECK(run_dispatch(server, make_request(http::method::POST, "/group/shallow", "[[1]]"))->get_status_code() == 400);
}

TEST_CASE("OpenAPI documents the route limits", "[server][limits][openapi]") {
    http::server server;
    server.post("/imports", [](http::response& res) { res.send("ok"); }).max_body_size(1000).timeout(2500ms);
    server.post("/plain", [](http::response& res) { res.send("ok"); });
    auto document = server.openapi().generate();
    auto imports = document["paths"]["/imports"]["post"];
    CHECK(imports["x-max-body-size"] == 1000);
    CHECK(imports["x-timeout-ms"] == 2500);
    auto plain = document["paths"]["/plain"]["post"];
    CHECK_FALSE(plain.contains("x-max-body-size"));
    CHECK_FALSE(plain.contains("x-timeout-ms"));
}

// --- Request timeout ---

TEST_CASE("A route timeout answers only that route", "[server][timeout][integration]") {
    http::server server;
    std::atomic<int> finished{0};
    std::atomic<bool> late_responded{false};
    auto slow = [&](http::request&, http::response& res) -> thinger::awaitable<void> {
        co_await sleep_for(1500ms);
        res.send("late");
        late_responded = res.has_responded();
        finished++;
    };
    server.get("/slow", slow).timeout(200ms);
    server.get("/slow-no-timeout", slow);
    server.get("/fast", [](http::response& res) { res.send("fast"); }).timeout(200ms);
    running_server running(server);

    SECTION("Timed out: 504, the handler goes on, its late answer is dropped") {
        connection conn(running.port);
        auto start = clock::now();
        conn.send("GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n");
        auto response = conn.read_response();
        auto elapsed = clock::now() - start;
        CHECK(response.starts_with("HTTP/1.1 504"));
        CHECK(elapsed < 1200ms);
        CHECK_FALSE(has_header(response, "Connection: close"));

        // The connection is still usable, even while the handler goes on
        conn.send("GET /fast HTTP/1.1\r\nHost: localhost\r\n\r\n");
        response = conn.read_response();
        CHECK(response.starts_with("HTTP/1.1 200"));
        CHECK(response.find("fast") != std::string::npos);

        // The handler ends later, and nothing else is sent
        for (int i = 0; i < 50 && finished == 0; ++i) std::this_thread::sleep_for(100ms);
        CHECK(finished == 1);
        CHECK(late_responded);
        CHECK(conn.read_response(300ms).empty());
        CHECK(conn.buffer.find("late") == std::string::npos);
    }

    SECTION("Without a timeout, a slow route is not affected") {
        auto start = clock::now();
        auto result = raw_exchange(running.port, "GET /slow-no-timeout HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 5s);
        CHECK(result.data.starts_with("HTTP/1.1 200"));
        CHECK(result.data.find("late") != std::string::npos);
        CHECK(clock::now() - start >= 1400ms);
    }

    SECTION("A response within the timeout") {
        auto result = raw_exchange(running.port, "GET /fast HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        CHECK(result.data.starts_with("HTTP/1.1 200"));
        // the timer was cancelled: nothing more arrives on the connection
        CHECK(count(result.data, "HTTP/1.1") == 1);
    }
}

TEST_CASE("A late answer from another thread is ignored after the timeout", "[server][timeout][deferred][integration]") {
    http::server server;
    std::mutex mutex;
    std::vector<std::thread> threads;
    std::atomic<int> late_answers{0};
    server.get("/device", [&](http::response& res) {
        // Answered later, from another thread (e.g. when a device answers)
        std::lock_guard lock(mutex);
        threads.emplace_back([res, &late_answers]() mutable {
            std::this_thread::sleep_for(600ms);
            res.json({{"value", 1}});
            if (res.has_responded()) late_answers++;
        });
    }).timeout(150ms);
    // The response moved away from the handler (the server must not use it afterwards)
    auto moved = [&](http::response& res) {
        std::lock_guard lock(mutex);
        threads.emplace_back([res = std::move(res)]() mutable {
            std::this_thread::sleep_for(300ms);
            res.send("moved");
        });
    };
    server.get("/moved", moved);
    server.get("/moved-timeout", moved).timeout(100ms);
    server.get("/fast", [](http::response& res) { res.send("fast"); });
    running_server running(server);

    auto close = "Host: localhost\r\nConnection: close\r\n\r\n";
    CHECK(raw_exchange(running.port, std::string("GET /moved HTTP/1.1\r\n") + close).data.find("moved") != std::string::npos);
    CHECK(raw_exchange(running.port, std::string("GET /moved-timeout HTTP/1.1\r\n") + close).data.starts_with("HTTP/1.1 504"));

    connection conn(running.port);
    conn.send("GET /device HTTP/1.1\r\nHost: localhost\r\n\r\n");
    auto response = conn.read_response();
    CHECK(response.starts_with("HTTP/1.1 504"));

    std::this_thread::sleep_for(800ms);
    CHECK(late_answers == 1);
    conn.send("GET /fast HTTP/1.1\r\nHost: localhost\r\n\r\n");
    response = conn.read_response();
    CHECK(response.starts_with("HTTP/1.1 200"));
    CHECK(conn.buffer.empty());

    std::lock_guard lock(mutex);
    for (auto& thread : threads) thread.join();
}

TEST_CASE("Timeout status and error format", "[server][timeout][error-format][integration]") {
    http::server server;
    server.set_error_formatter([](const http::http_error& error, http::http_response& response) {
        response.set_content(nlohmann::json{{"code", static_cast<int>(error.status)}, {"message", error.message}}.dump(),
                             "application/json");
    });
    server.get("/busy", [](http::response& res) -> thinger::awaitable<void> {
        co_await sleep_for(1000ms);
        res.send("late");
    }).timeout(100ms, http::http_response::status::service_unavailable);
    running_server running(server);

    auto result = raw_exchange(running.port, "GET /busy HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    CHECK(result.data.starts_with("HTTP/1.1 503"));
    CHECK(result.data.find(R"("code":503)") != std::string::npos);
    CHECK(result.data.find("No response within the timeout") != std::string::npos);
}

TEST_CASE("Groups set the timeout of their routes", "[server][timeout][group][integration]") {
    http::server server;
    auto slow = [](http::response& res) -> thinger::awaitable<void> {
        co_await sleep_for(800ms);
        res.send("done");
    };
    auto api = server.group("/api").timeout(150ms);
    api.get("/slow", slow);
    api.get("/long", slow).timeout(0ms);    // no timeout for this one
    api.get("/longer", slow).timeout(3s);
    running_server running(server);

    auto close = "Host: localhost\r\nConnection: close\r\n\r\n";
    CHECK(raw_exchange(running.port, std::string("GET /api/slow HTTP/1.1\r\n") + close).data.starts_with("HTTP/1.1 504"));
    CHECK(raw_exchange(running.port, std::string("GET /api/long HTTP/1.1\r\n") + close).data.starts_with("HTTP/1.1 200"));
    CHECK(raw_exchange(running.port, std::string("GET /api/longer HTTP/1.1\r\n") + close).data.starts_with("HTTP/1.1 200"));
}

TEST_CASE("Responses started before the timeout are not cut", "[server][timeout][stream][integration]") {
    http::server server;
    server.get("/stream", [](http::request&, http::response& res) -> thinger::awaitable<void> {
        http::http_response head;
        head.set_status(http::http_response::status::ok);
        head.set_content_type("text/plain");
        res.start_stream(head);
        co_await res.write_stream("first,");
        co_await sleep_for(500ms);
        co_await res.write_stream("second");
        co_await res.end_stream();
    }).timeout(100ms);
    server.get("/chunked", [](http::response& res) -> thinger::awaitable<void> {
        res.start_chunked("text/plain");
        res.write_chunk("first,");
        co_await sleep_for(500ms);
        res.write_chunk("second");
        res.end_chunked();
    }).timeout(100ms);
    std::promise<std::shared_ptr<http::sse_connection>> sse_promise;
    server.get("/events", [&](http::response& res) {
        res.start_sse([&](std::shared_ptr<http::sse_connection> sse) { sse_promise.set_value(sse); });
    }).timeout(100ms);
    running_server running(server);

    SECTION("Streamed response") {
        auto result = raw_exchange(running.port, "GET /stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        CHECK(result.data.starts_with("HTTP/1.1 200"));
        CHECK(result.data.find("second") != std::string::npos);
        CHECK(count(result.data, "HTTP/1.1") == 1);
    }

    SECTION("Chunked response") {
        auto result = raw_exchange(running.port, "GET /chunked HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        CHECK(result.data.starts_with("HTTP/1.1 200"));
        CHECK(result.data.find("second") != std::string::npos);
        CHECK(count(result.data, "HTTP/1.1") == 1);
    }

    SECTION("Server-Sent Events") {
        connection conn(running.port);
        conn.send("GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n");
        auto future = sse_promise.get_future();
        REQUIRE(future.wait_for(3s) == std::future_status::ready);
        auto sse = future.get();
        std::this_thread::sleep_for(400ms);
        sse->send_data("after the timeout");
        // read what arrives (the event stream does not end)
        conn.wait_closed(500ms);
        CHECK(conn.buffer.starts_with("HTTP/1.1 200"));
        CHECK(conn.buffer.find("after the timeout") != std::string::npos);
        CHECK(conn.buffer.find("504") == std::string::npos);
        sse->stop();
    }
}

TEST_CASE("A timeout with the body read keeps the connection; with the body unread, closes it", "[server][timeout][body][integration]") {
    http::server server;
    server.post("/json", [](nlohmann::json&, http::response& res) -> thinger::awaitable<void> {
        co_await sleep_for(1000ms);
        res.send("late");
    }).timeout(150ms);
    std::atomic<int> body_errors{0};
    server.post("/upload", [&](http::request& req, http::response& res) -> thinger::awaitable<void> {
        uint8_t buffer[1024];
        size_t total = 0;
        while (size_t bytes = co_await req.read_some(buffer, sizeof(buffer))) total += bytes;
        if (req.get_body_error() != http::body_error::none) body_errors++;
        res.send("read=" + std::to_string(total));
    }).timeout(150ms);
    server.get("/fast", [](http::response& res) { res.send("fast"); });
    running_server running(server);

    SECTION("Body read whole: the connection is kept alive") {
        connection conn(running.port);
        conn.send(post("/json", R"({"a":1})", "Content-Type: application/json\r\n"));
        auto response = conn.read_response();
        CHECK(response.starts_with("HTTP/1.1 504"));
        CHECK_FALSE(has_header(response, "Connection: close"));
        conn.send("GET /fast HTTP/1.1\r\nHost: localhost\r\n\r\n");
        CHECK(conn.read_response().starts_with("HTTP/1.1 200"));
    }

    SECTION("Body being read: the connection is closed after the response") {
        connection conn(running.port);
        // Half of the body: the handler waits for the rest
        conn.send("POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10000\r\n\r\n" + std::string(5000, 'x'));
        auto response = conn.read_response();
        CHECK(response.starts_with("HTTP/1.1 504"));
        CHECK(has_header(response, "Connection: close"));
        CHECK(conn.wait_closed());
        // the handler sees the body incomplete
        for (int i = 0; i < 30 && body_errors == 0; ++i) std::this_thread::sleep_for(100ms);
        CHECK(body_errors == 1);
    }
}

TEST_CASE("A route timeout longer than the connection timeout", "[server][timeout][connection][integration]") {
    http::server server;
    server.set_connection_timeout(1s);
    server.get("/device", [](http::response& res) -> thinger::awaitable<void> {
        co_await sleep_for(2000ms);
        res.send("answer");
    }).timeout(5s);
    running_server running(server);

    auto result = raw_exchange(running.port, "GET /device HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 5s);
    CHECK(result.data.starts_with("HTTP/1.1 200"));
    CHECK(result.data.find("answer") != std::string::npos);
}

TEST_CASE("In-memory dispatch honors the route timeout", "[server][timeout][dispatch][integration]") {
    http::server server;
    std::atomic<int> finished{0};
    server.get("/slow", [&](http::response& res) -> thinger::awaitable<void> {
        co_await sleep_for(800ms);
        res.send("late");
        finished++;
    }).timeout(100ms);
    server.get("/slower", [&](http::response& res) -> thinger::awaitable<void> {
        co_await sleep_for(5000ms);
        res.send("late");
    }).timeout(4s);

    auto start = clock::now();
    auto response = run_dispatch(server, make_request(http::method::GET, "/slow"));
    CHECK(response->get_status_code() == 504);
    CHECK(response->get_content() == "No response within the timeout");
    // run_dispatch runs its io_context until the handler ends too
    CHECK(finished == 1);

    // The dispatch timeout, if shorter, cancels the handler
    start = clock::now();
    response = run_dispatch(server, make_request(http::method::GET, "/slower"), {.timeout = 200ms});
    CHECK(response->get_status_code() == 504);
    CHECK(clock::now() - start < 3s);
}

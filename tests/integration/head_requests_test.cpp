#include "server_test_helpers.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/sse_connection.hpp>
#include <thinger/http/server/websocket_connection.hpp>
#include <thinger/util/ascii.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace thinger;
using namespace std::chrono_literals;

// HEAD requests over raw TCP, as Node.js/Express answer them: routed to the GET route of the
// path unless a HEAD route matches it, with the status and headers of the GET answer
// (Content-Length included) and no body, so the next response on the connection is read
// where it starts

namespace {

    namespace fs = std::filesystem;
    using namespace server_test;

    struct raw_response {
        int status = 0;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;

        std::string header(std::string_view name) const {
            for (const auto& [key, value] : headers) {
                if (util::ascii::iequals(key, name)) return value;
            }
            return {};
        }
        bool has_header(std::string_view name) const {
            return std::any_of(headers.begin(), headers.end(),
                               [&](const auto& header) { return util::ascii::iequals(header.first, name); });
        }
        // Headers but Connection (keep-alive or close, depending on the position in the pipeline)
        std::vector<std::pair<std::string, std::string>> representation_headers() const {
            std::vector<std::pair<std::string, std::string>> result;
            for (const auto& header : headers) {
                if (!util::ascii::iequals(header.first, "Connection")) result.push_back(header);
            }
            return result;
        }
    };

    struct pipelined_request {
        std::string method;
        std::string target;
        std::string headers = "";
    };

    // Send the requests on one connection, the last one closing it, and split the bytes
    // received into their responses: each head, then a body of its Content-Length, except
    // for the answers to HEAD, which have none. Every byte must belong to a response.
    std::vector<raw_response> exchange(uint16_t port, const std::vector<pipelined_request>& requests) {
        std::string data;
        for (size_t i = 0; i < requests.size(); ++i) {
            const auto& request = requests[i];
            data += request.method + " " + request.target + " HTTP/1.1\r\nHost: localhost\r\n" + request.headers;
            if (i + 1 == requests.size()) data += "Connection: close\r\n";
            data += "\r\n";
        }
        auto result = raw_exchange(port, data);
        INFO(result.data);
        REQUIRE(result.closed);

        std::vector<raw_response> responses;
        std::string_view rest = result.data;
        for (const auto& request : requests) {
            REQUIRE(rest.starts_with("HTTP/1.1 "));
            auto end_headers = rest.find("\r\n\r\n");
            REQUIRE(end_headers != std::string_view::npos);

            raw_response response;
            response.status = std::stoi(std::string(rest.substr(9, 3)));
            auto head = rest.substr(0, end_headers);
            size_t line_start = head.find("\r\n");
            while (line_start != std::string_view::npos) {
                line_start += 2;
                auto line_end = head.find("\r\n", line_start);
                auto line = head.substr(line_start, line_end == std::string_view::npos ? std::string_view::npos : line_end - line_start);
                auto colon = line.find(": ");
                if (colon != std::string_view::npos) {
                    response.headers.emplace_back(std::string(line.substr(0, colon)), std::string(line.substr(colon + 2)));
                }
                line_start = line_end;
            }
            rest.remove_prefix(end_headers + 4);

            if (request.method != "HEAD") {
                size_t length = response.has_header("Content-Length") ? std::stoul(response.header("Content-Length")) : 0;
                REQUIRE(rest.size() >= length);
                response.body = std::string(rest.substr(0, length));
                rest.remove_prefix(length);
            }
            responses.push_back(std::move(response));
        }
        REQUIRE(rest.empty());
        return responses;
    }

    // HEAD of `target`, then GET of it on the same connection: the HEAD answer is the GET one
    // without body, with the Content-Length of that body
    void require_head_like_get(uint16_t port, const std::string& target, const std::string& headers = "") {
        auto responses = exchange(port, {{"HEAD", target, headers}, {"GET", target, headers}});
        const auto& head = responses[0];
        const auto& get = responses[1];
        INFO(target);
        REQUIRE(head.status == get.status);
        REQUIRE(head.body.empty());
        REQUIRE(head.representation_headers() == get.representation_headers());
        REQUIRE(head.has_header("Content-Length"));
        REQUIRE(head.header("Content-Length") == std::to_string(get.body.size()));
    }

}

TEST_CASE("HEAD is answered by the GET route without body", "[server][head][integration]") {
    http::server server;
    server.get("/text", [](http::response& res) {
        res.header("X-Route", "text");
        res.send("Hello World!");
    });
    server.get("/json", [](http::response& res) {
        res.json({{"message", "hello"}, {"count", 42}});
    });
    server.get("/error", [](http::response& res) {
        res.error(http::http_response::status::forbidden, "not yours");
    });
    server.get("/redirect", [](http::response& res) {
        res.redirect("/text");
    });
    server.get("/items/:id", [](http::request& req, http::response& res) {
        res.send("item " + req["id"]);
    });
    server.get("/coroutine", [](http::response& res) -> thinger::awaitable<void> {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 5ms);
        co_await timer.async_wait(thinger::use_awaitable);
        res.send("later");
    });

    SECTION("send(), json(), error(), redirect(), with parameters and coroutines") {
        running_server running(server);
        for (const auto* target : {"/text", "/json", "/error", "/redirect", "/items/7", "/coroutine"}) {
            require_head_like_get(running.port, target);
        }
    }

    SECTION("The exact bytes on a keep-alive connection") {
        running_server running(server);
        auto result = raw_exchange(running.port,
            "HEAD /text HTTP/1.1\r\nHost: localhost\r\n\r\n"
            "GET /text HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(result.closed);
        auto end_head = result.data.find("\r\n\r\n");
        REQUIRE(end_head != std::string::npos);
        auto head = result.data.substr(0, end_head + 4);
        auto next = result.data.substr(end_head + 4);
        REQUIRE(head.starts_with("HTTP/1.1 200 OK\r\n"));
        REQUIRE(head.find("Content-Length: 12\r\n") != std::string::npos);
        REQUIRE(head.find("Hello World!") == std::string::npos);
        // the next response starts right after the head
        REQUIRE(next.starts_with("HTTP/1.1 200 OK\r\n"));
        REQUIRE(next.ends_with("\r\n\r\nHello World!"));
        REQUIRE(count(result.data, "Hello World!") == 1);
    }

    SECTION("Custom error formatter") {
        server.set_error_formatter([](const http::http_error& error, http::http_response& response) {
            response.set_content(nlohmann::json{{"error", error.message}}.dump(), "application/json");
        });
        running_server running(server);
        require_head_like_get(running.port, "/error");
        auto head = exchange(running.port, {{"HEAD", "/error"}})[0];
        REQUIRE(head.status == 403);
        REQUIRE(head.header("Content-Type") == "application/json");
    }

    SECTION("A HEAD request matching no route: 404, without body") {
        running_server running(server);
        auto responses = exchange(running.port, {{"HEAD", "/missing"}, {"GET", "/missing"}});
        REQUIRE(responses[0].status == 404);
        REQUIRE(responses[0].representation_headers() == responses[1].representation_headers());
    }

    SECTION("In-memory dispatch: no body, with the Content-Length of the GET one") {
        auto head = run_dispatch(server, make_request(http::method::HEAD, "/text"));
        REQUIRE(head->get_status_code() == 200);
        REQUIRE(head->get_header("X-Route") == "text");
        REQUIRE(head->get_header("Content-Length") == "12");
        REQUIRE(head->get_content().empty());
        REQUIRE(run_dispatch(server, make_request(http::method::GET, "/text"))->get_content() == "Hello World!");
    }
}

TEST_CASE("HEAD: an explicit HEAD route wins over the GET one", "[server][head][integration]") {
    http::server server;
    server.get("/both", [](http::response& res) { res.send("from get"); });
    server.head("/both", [](http::response& res) {
        res.header("X-Handler", "head");
        res.send("from head");
    });
    server.get("/only-get/:id", [](http::request& req, http::response& res) {
        res.header("X-Handler", "get");
        res.send("get " + req["id"]);
    });
    // a HEAD wildcard of another prefix does not hide the GET routes
    server.head("/other/:path(.*)", [](http::response& res) { res.send(""); });
    running_server running(server);

    auto responses = exchange(running.port, {{"HEAD", "/both"}, {"HEAD", "/only-get/5"}, {"GET", "/both"}});
    REQUIRE(responses[0].status == 200);
    REQUIRE(responses[0].header("X-Handler") == "head");
    REQUIRE(responses[0].header("Content-Length") == "9");
    REQUIRE(responses[1].header("X-Handler") == "get");
    REQUIRE(responses[1].header("Content-Length") == "5");
    REQUIRE(responses[2].body == "from get");
}

TEST_CASE("HEAD: 405 lists HEAD wherever GET is allowed", "[server][head][integration]") {
    http::server server;
    server.get("/items/:id", [](http::request& req, http::response& res) { res.send("item " + req["id"]); });
    server.post("/items", [](http::response& res) { res.send("created"); });
    running_server running(server);

    auto responses = exchange(running.port, {{"DELETE", "/items/1"}, {"DELETE", "/items"}, {"PATCH", "/missing"},
                                             {"HEAD", "/items/1"}, {"HEAD", "/missing"}, {"POST", "/items/1", "Content-Length: 0\r\n"}});
    REQUIRE(responses[0].status == 405);
    REQUIRE(responses[0].header("Allow") == "GET, HEAD");
    REQUIRE(responses[1].status == 405);
    REQUIRE(responses[1].header("Allow") == "POST");
    // no route matches the path: an empty Allow
    REQUIRE(responses[2].status == 405);
    REQUIRE(responses[2].has_header("Allow"));
    REQUIRE(responses[2].header("Allow").empty());
    // HEAD has the routes of GET: served, or 404
    REQUIRE(responses[3].status == 200);
    REQUIRE(responses[3].header("Content-Length") == "6");
    REQUIRE(responses[4].status == 404);
    // a method with routes, none matching: 404 as before
    REQUIRE(responses[5].status == 404);
}

TEST_CASE("HEAD: serve_static does not take the HEAD requests of GET routes", "[server][head][static][integration]") {
    auto root = fs::temp_directory_path() /
                ("thinger_head_static_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    std::ofstream(root / "index.html", std::ios::binary) << "<html>index</html>";

    {
        http::server server;
        server.serve_static("/", root.string());
        server.get("/api/x", [](http::response& res) {
            res.header("X-Api", "x");
            res.json({{"route", "/api/x"}, {"padding", std::string(10, 'p')}});
        });
        running_server running(server);

        require_head_like_get(running.port, "/api/x");
        auto head = exchange(running.port, {{"HEAD", "/api/x"}})[0];
        REQUIRE(head.header("X-Api") == "x");
        REQUIRE(head.header("Content-Type") == "application/json");

        // the files are still answered to HEAD, by the GET route of serve_static
        auto responses = exchange(running.port, {{"HEAD", "/index.html"}, {"HEAD", "/"}, {"HEAD", "/spa/path"}});
        for (const auto& response : responses) {
            REQUIRE(response.status == 200);
            REQUIRE(response.header("Content-Length") == "18");
            REQUIRE(response.has_header("ETag"));
            REQUIRE_FALSE(response.has_header("X-Api"));
        }
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("HEAD: not compressed, with the length of the identity body", "[server][head][compression][integration]") {
    http::server server;
    const std::string text(4096, 'a');
    server.get("/big", [&text](http::response& res) { res.send(text); });
    running_server running(server);

    const std::string accept = "Accept-Encoding: gzip\r\n";
    auto responses = exchange(running.port, {{"HEAD", "/big", accept}, {"GET", "/big", accept}, {"GET", "/big"}});
    const auto& head = responses[0];
    const auto& compressed = responses[1];
    const auto& identity = responses[2];

    REQUIRE(compressed.header("Content-Encoding") == "gzip");
    REQUIRE(compressed.body.size() < text.size());
    REQUIRE(identity.body == text);

    REQUIRE(head.status == 200);
    REQUIRE(head.body.empty());
    REQUIRE_FALSE(head.has_header("Content-Encoding"));
    REQUIRE(head.header("Content-Length") == std::to_string(text.size()));
    REQUIRE(head.representation_headers() == identity.representation_headers());
}

TEST_CASE("HEAD: chunked, streamed, SSE and WebSocket answers", "[server][head][integration]") {
    http::server server;
    server.get("/text", [](http::response& res) { res.send("Hello World!"); });
    server.get("/chunked", [](http::response& res) {
        if (!res.start_chunked("text/plain")) return;
        res.write_chunk("first,");
        res.write_chunk("second");
        res.end_chunked();
    });
    server.get("/stream", [](http::response& res) -> thinger::awaitable<void> {
        http::http_response head;
        head.set_content_type("text/plain");
        if (!res.start_stream(head, 10)) co_return;
        co_await res.write_stream("0123456789");
        co_await res.end_stream();
    });
    std::atomic<bool> sse_called{false};
    server.get("/events", [&sse_called](http::response& res) {
        res.start_sse([&sse_called](std::shared_ptr<http::sse_connection> connection) {
            sse_called = true;
            connection->send_data("event");
        });
    });
    server.get("/ws", [](http::response& res) {
        res.upgrade_websocket([](std::shared_ptr<http::websocket_connection>) {});
    });
    running_server running(server);

    SECTION("Chunked: the headers, without Transfer-Encoding nor chunks") {
        auto responses = exchange(running.port, {{"HEAD", "/chunked"}, {"GET", "/text"}});
        REQUIRE(responses[0].status == 200);
        REQUIRE(responses[0].header("Content-Type") == "text/plain");
        REQUIRE_FALSE(responses[0].has_header("Transfer-Encoding"));
        REQUIRE(responses[1].body == "Hello World!");
    }

    SECTION("Streamed: the length advertised, without the body") {
        auto responses = exchange(running.port, {{"HEAD", "/stream"}, {"GET", "/text"}});
        REQUIRE(responses[0].status == 200);
        REQUIRE(responses[0].header("Content-Length") == "10");
        REQUIRE(responses[1].body == "Hello World!");
    }

    SECTION("SSE: the headers of the event stream, without events nor handler") {
        auto responses = exchange(running.port, {{"HEAD", "/events"}, {"GET", "/text"}});
        REQUIRE(responses[0].status == 200);
        REQUIRE(responses[0].header("Content-Type") == "text/event-stream");
        REQUIRE(responses[1].body == "Hello World!");
        REQUIRE_FALSE(sse_called);
    }

    SECTION("WebSocket: a HEAD request cannot upgrade, answered without body") {
        const std::string upgrade = "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
        // (the Connection: Upgrade of the request does not close it)
        auto responses = exchange(running.port, {{"HEAD", "/ws", upgrade}, {"HEAD", "/ws"}, {"GET", "/text"}});
        REQUIRE(responses[0].status == 400);
        REQUIRE(responses[1].status == 426);
        REQUIRE(responses[2].body == "Hello World!");
    }
}

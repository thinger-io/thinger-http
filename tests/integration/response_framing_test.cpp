#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/sse_connection.hpp>

#include "server_test_helpers.hpp"

// Framing of the responses the server sends: every response tells where its body ends (a
// Content-Length, as Node), so the connection can carry the next one, or says the connection
// closes after it. Hand-built responses (send_response) and connections taken over included.

using namespace thinger;
using namespace std::chrono_literals;
using server_test::count;
using server_test::raw_exchange;
using server_test::running_server;

namespace {

    std::string get(const std::string& path, const std::string& method = "GET") {
        return method + " " + path + " HTTP/1.1\r\nHost: x\r\n\r\n";
    }

    // The head of the first response in `data`, lowercase
    std::string head_of(const std::string& data) {
        auto head = data.substr(0, data.find("\r\n\r\n") + 4);
        for (auto& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return head;
    }

    std::shared_ptr<http::http_response> hand_built(http::http_response::status status) {
        auto response = std::make_shared<http::http_response>();
        response->set_status(status);
        return response;
    }

}

TEST_CASE("Hand-built responses are framed by their content", "[server][framing][integration]") {
    http::server server;
    server.get("/no-length", [](http::response& res) {
        auto response = hand_built(http::http_response::status::ok);
        response->get_content() = "hello";      // no Content-Length
        response->set_content_type("text/plain");
        res.send_response(response);
    });
    server.get("/stale-length", [](http::response& res) {
        auto response = hand_built(http::http_response::status::ok);
        response->set_content("hello");
        response->get_content() += " world";    // Content-Length: 5 left behind
        res.send_response(response);
    });
    server.get("/copied", [](http::response& res) {
        // as copied from a response read chunked, whose body was decoded
        auto response = hand_built(http::http_response::status::ok);
        response->add_header("Transfer-Encoding", "chunked");
        response->get_content() = "decoded";
        res.send_response(response);
    });
    server.get("/empty", [](http::response& res) { res.send_response(hand_built(http::http_response::status::ok)); });
    server.get("/no-content", [](http::response& res) {
        auto response = hand_built(http::http_response::status::no_content);
        response->set_content("dropped");
        res.send_response(response);
    });
    server.get("/not-modified", [](http::response& res) {
        auto response = hand_built(http::http_response::status::not_modified);
        response->set_content_length(1234);     // the length of the representation
        res.send_response(response);
    });
    server.get("/status-no-content", [](http::response& res) {
        res.status(http::http_response::status::no_content);
        res.send("dropped");
    });
    server.get("/head", [](http::response& res) {
        auto response = hand_built(http::http_response::status::ok);
        response->get_content() = "hello";
        res.send_response(response);
    });
    running_server running(server);

    // Two requests on the same connection: both answered, the connection kept open
    auto twice = [&](const std::string& path, const std::string& method = "GET") {
        auto result = raw_exchange(running.port, get(path, method) + get(path, method), 500ms);
        INFO(result.data);
        REQUIRE_FALSE(result.closed);
        return result.data;
    };

    SECTION("Without Content-Length") {
        auto data = twice("/no-length");
        REQUIRE(count(data, "HTTP/1.1 200") == 2);
        REQUIRE(head_of(data).find("content-length: 5\r\n") != std::string::npos);
        REQUIRE(count(data, "\r\n\r\nhello") == 2);
    }
    SECTION("With a Content-Length that is not the one of the content") {
        auto data = twice("/stale-length");
        REQUIRE(count(data, "HTTP/1.1 200") == 2);
        REQUIRE(head_of(data).find("content-length: 11\r\n") != std::string::npos);
    }
    SECTION("With the Transfer-Encoding of another response") {
        auto data = twice("/copied");
        REQUIRE(count(data, "HTTP/1.1 200") == 2);
        auto head = head_of(data);
        REQUIRE(head.find("transfer-encoding") == std::string::npos);
        REQUIRE(head.find("content-length: 7\r\n") != std::string::npos);
    }
    SECTION("Without content") {
        auto data = twice("/empty");
        REQUIRE(count(data, "HTTP/1.1 200") == 2);
        REQUIRE(head_of(data).find("content-length: 0\r\n") != std::string::npos);
    }
    SECTION("204 and 304 have no body, and 204 no Content-Length") {
        auto data = twice("/no-content");
        REQUIRE(count(data, "HTTP/1.1 204") == 2);
        REQUIRE(head_of(data).find("content-length") == std::string::npos);
        REQUIRE(data.find("dropped") == std::string::npos);

        data = twice("/status-no-content");
        REQUIRE(count(data, "HTTP/1.1 204") == 2);
        REQUIRE(head_of(data).find("content-length") == std::string::npos);
        REQUIRE(data.find("dropped") == std::string::npos);

        data = twice("/not-modified");
        REQUIRE(count(data, "HTTP/1.1 304") == 2);
        REQUIRE(head_of(data).find("content-length: 1234\r\n") != std::string::npos);
    }
    SECTION("HEAD: the length of the content, without it") {
        auto data = twice("/head", "HEAD");
        REQUIRE(count(data, "HTTP/1.1 200") == 2);
        REQUIRE(head_of(data).find("content-length: 5\r\n") != std::string::npos);
        REQUIRE(data.find("hello") == std::string::npos);
    }
}

TEST_CASE("Responses of connections taken over say how the body ends", "[server][framing][takeover][integration]") {
    http::server server;
    server.get("/raw", [](http::response& res) {
        res.take_over([](std::shared_ptr<asio::socket> socket, std::string) {
            co_spawn(socket->get_io_context(), [socket]() -> awaitable<void> {
                co_await socket->write(std::string_view("raw body"));
                socket->close();
            }, detached);
        });
    });
    server.get("/framed", [](http::response& res) {
        res.header("Content-Length", "8");
        res.take_over([](std::shared_ptr<asio::socket> socket, std::string) {
            co_spawn(socket->get_io_context(), [socket]() -> awaitable<void> {
                co_await socket->write(std::string_view("raw body"));
            }, detached);
        });
    });
    server.get("/upgrade", [](http::response& res) {
        res.status(http::http_response::status::switching_protocols);
        res.header("Upgrade", "custom");
        res.header("Connection", "Upgrade");
        res.take_over([](std::shared_ptr<asio::socket> socket, std::string) { socket->close(); });
    });
    server.get("/events", [](http::response& res) {
        res.start_sse([](std::shared_ptr<http::sse_connection> sse) { sse->stop(); });
    });
    running_server running(server);

    SECTION("A body until the connection closes") {
        auto result = raw_exchange(running.port, get("/raw"));
        auto head = head_of(result.data);
        REQUIRE(head.starts_with("http/1.1 200"));
        REQUIRE(head.find("connection: close\r\n") != std::string::npos);
        REQUIRE(head.find("content-length") == std::string::npos);
        REQUIRE(result.data.ends_with("\r\n\r\nraw body"));
    }
    SECTION("A body framed by the handler") {
        auto result = raw_exchange(running.port, get("/framed"), 500ms);
        auto head = head_of(result.data);
        REQUIRE(head.find("content-length: 8\r\n") != std::string::npos);
        REQUIRE(head.find("connection: close") == std::string::npos);
    }
    SECTION("A protocol switch") {
        auto result = raw_exchange(running.port, get("/upgrade"));
        auto head = head_of(result.data);
        REQUIRE(head.starts_with("http/1.1 101"));
        REQUIRE(head.find("connection: upgrade\r\n") != std::string::npos);
        REQUIRE(head.find("connection: close") == std::string::npos);
    }
    SECTION("An event stream") {
        auto result = raw_exchange(running.port, get("/events"));
        auto head = head_of(result.data);
        REQUIRE(head.find("content-type: text/event-stream\r\n") != std::string::npos);
        REQUIRE(head.find("connection: close\r\n") != std::string::npos);
        REQUIRE(head.find("connection: keep-alive") == std::string::npos);
        REQUIRE(head.find("content-length") == std::string::npos);
    }
}

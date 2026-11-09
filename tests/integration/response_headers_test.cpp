#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>

#include "server_test_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>

// Headers set on a response before answering: set_header() replaces every field of a name,
// header() adds another one, and each is sent once whatever answer sends it

using namespace thinger;
using server_test::count;
using server_test::raw_exchange;
using server_test::running_server;

namespace {

    using status = http::http_response::status;

    // The head of the response to GET `target`, on a connection closed after it
    std::string head_of(const running_server& running, const std::string& target) {
        auto result = raw_exchange(running.port, "GET " + target + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        INFO(result.data);
        REQUIRE(result.closed);
        auto end = result.data.find("\r\n\r\n");
        REQUIRE(end != std::string::npos);
        return result.data.substr(0, end + 2);
    }

    std::string lowercase(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    std::shared_ptr<http::http_response> hand_built() {
        auto message = std::make_shared<http::http_response>();
        message->set_status(status::ok);
        message->set_content("hand built", "text/plain");
        return message;
    }

    // A file removed at the end of the test
    struct temporary_file {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("thinger_response_headers_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
        temporary_file() { std::ofstream(path, std::ios::binary) << "content"; }
        ~temporary_file() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    };

}

TEST_CASE("set_header() replaces every field of its name, header() adds another one", "[server][response][integration]") {
    http::server server;
    server.get("/replace", [](http::response& res) {
        res.header("X-A", "1");
        res.set_header("x-a", "2");
        res.send("ok");
    });
    server.get("/replace-all", [](http::response& res) {
        res.header("X-A", "1");
        res.header("X-B", "other");
        res.header("X-A", "2");
        res.set_header("X-A", "3");
        res.send("ok");
    });
    server.get("/set-new", [](http::response& res) {
        res.set_header("X-New", "value");
        res.send("ok");
    });
    server.get("/cookies", [](http::response& res) {
        res.header("Set-Cookie", "a=1");
        res.header("Set-Cookie", "b=2");
        res.send("ok");
    });
    server.get("/invalid", [](http::response& res) {
        res.header("X-A", "1");
        res.set_header("X-A", "bad\r\nvalue");
        res.set_header("Bad Name", "value");
        res.send("ok");
    });
    running_server running(server);

    SECTION("One field, compared without case") {
        auto head = head_of(running, "/replace");
        REQUIRE(count(head, "X-A: 2\r\n") == 1);
        REQUIRE(count(lowercase(head), "x-a:") == 1);
    }

    SECTION("All the fields of the name, and only those") {
        auto head = head_of(running, "/replace-all");
        REQUIRE(count(head, "X-A: 3\r\n") == 1);
        REQUIRE(count(lowercase(head), "x-a:") == 1);
        REQUIRE(count(head, "X-B: other\r\n") == 1);
    }

    SECTION("A name not set yet") {
        REQUIRE(count(head_of(running, "/set-new"), "X-New: value\r\n") == 1);
    }

    SECTION("header() still adds") {
        auto head = head_of(running, "/cookies");
        REQUIRE(count(head, "Set-Cookie: a=1\r\n") == 1);
        REQUIRE(count(head, "Set-Cookie: b=2\r\n") == 1);
    }

    SECTION("An invalid name or value is dropped, leaving the ones set before") {
        auto head = head_of(running, "/invalid");
        REQUIRE(count(head, "X-A: 1\r\n") == 1);
        REQUIRE(count(lowercase(head), "x-a:") == 1);
        REQUIRE(count(head, "Bad Name") == 0);
        REQUIRE(count(head, "bad") == 0);
    }
}

TEST_CASE("A header set before answering is sent once, whatever answer sends it", "[server][response][integration]") {
    temporary_file file;
    http::server server;
    auto nosniff = [](http::response& res) { res.set_header("X-Content-Type-Options", "nosniff"); };
    server.get("/file", [&](http::response& res) {
        nosniff(res);
        res.send_file(file.path);
    });
    server.get("/json", [&](http::response& res) {
        nosniff(res);
        res.json({{"a", 1}});
    });
    server.get("/send", [&](http::response& res) {
        nosniff(res);
        res.send("text");
    });
    server.get("/hand-built", [&](http::response& res) {
        nosniff(res);
        res.send_response(hand_built());
    });
    server.get("/chunked", [&](http::response& res) {
        nosniff(res);
        res.start_chunked("text/plain");
        res.write_chunk("data");
        res.end_chunked();
    });
    running_server running(server);

    for (const auto* target : {"/file", "/json", "/send", "/hand-built", "/chunked"}) {
        INFO(target);
        REQUIRE(count(head_of(running, target), "X-Content-Type-Options: nosniff\r\n") == 1);
    }
}

TEST_CASE("send_response() keeps the headers set before, its own win", "[server][response][integration]") {
    http::server server;
    server.get("/own-wins", [](http::response& res) {
        res.set_header("X-A", "draft");
        res.set_header("X-Only-Draft", "kept");
        auto message = hand_built();
        message->add_header("X-A", "own");
        res.send_response(message);
    });
    server.get("/cookies", [](http::response& res) {
        res.header("Set-Cookie", "a=1");
        res.header("Set-Cookie", "b=2");
        res.send_response(hand_built());
    });
    server.get("/type", [](http::response& res) {
        res.set_header("Content-Type", "text/csv");
        auto message = std::make_shared<http::http_response>();
        message->set_status(status::ok);
        message->set_content("a,b");
        res.send_response(message);
    });
    server.get("/framing", [](http::response& res) {
        res.header("Content-Length", "99999");
        res.send_response(hand_built());
    });
    running_server running(server);

    SECTION("The headers of the message replace the ones of that name") {
        auto head = head_of(running, "/own-wins");
        REQUIRE(count(head, "X-A: own\r\n") == 1);
        REQUIRE(count(head, "X-A: draft") == 0);
        REQUIRE(count(head, "X-Only-Draft: kept\r\n") == 1);
    }

    SECTION("Repeated fields are all sent") {
        auto head = head_of(running, "/cookies");
        REQUIRE(count(head, "Set-Cookie: a=1\r\n") == 1);
        REQUIRE(count(head, "Set-Cookie: b=2\r\n") == 1);
    }

    SECTION("A Content-Type set before is sent when the message has none") {
        REQUIRE(count(head_of(running, "/type"), "Content-Type: text/csv\r\n") == 1);
    }

    SECTION("Its framing is the message's own") {
        auto head = head_of(running, "/framing");
        REQUIRE(count(head, "Content-Length: 10\r\n") == 1);
        REQUIRE(count(lowercase(head), "content-length:") == 1);
    }
}

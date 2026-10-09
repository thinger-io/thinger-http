#include "server_test_helpers.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <atomic>
#include <memory>

using namespace thinger;
using namespace std::chrono_literals;

// Request body framing over raw TCP: Content-Length / Transfer-Encoding validation and
// strict chunked decoding, so that a request body can never be read as another request
// (request smuggling), nor the next request be lost

namespace {

    using namespace server_test;

    struct framing_server {
        http::server server;
        std::unique_ptr<running_server> running;
        uint16_t port = 0;
        std::atomic<int> smuggled{0};

        framing_server() {
            server.post("/echo", [](http::request& req, http::response& res) {
                res.send("body=[" + req.body() + "]");
            });
            server.get("/health", [](http::response& res) {
                res.send("healthy");
            });
            // Never requested by the tests on their own: only reached by a smuggled request
            server.get("/smuggled", [this](http::response& res) {
                smuggled++;
                res.send("smuggled");
            });
        }

        void start() {
            running = std::make_unique<running_server>(server);
            port = running->port;
        }
    };

    const std::string smuggled_request = "GET /smuggled HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const std::string health_request = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const std::string health_close_request = "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

    std::string post_headers(const std::string& framing_headers) {
        return "POST /echo HTTP/1.1\r\nHost: localhost\r\n" + framing_headers + "\r\n";
    }

    std::string chunked_post(const std::string& chunked_body) {
        return post_headers("Transfer-Encoding: chunked\r\n") + chunked_body;
    }

    // The request is answered with a single 400, the connection is closed, and nothing
    // sent after it is run as another request
    void require_rejected(framing_server& fixture, const std::string& request) {
        auto result = raw_exchange(fixture.port, request + smuggled_request);
        INFO("response: " << result.data);
        REQUIRE(result.data.starts_with("HTTP/1.1 400"));
        REQUIRE(count(result.data, "HTTP/1.1 ") == 1);
        REQUIRE(result.closed);
        REQUIRE(fixture.smuggled == 0);
    }
}

TEST_CASE("Transfer-Encoding other than chunked is rejected", "[server][framing][smuggling][integration]") {
    framing_server fixture;
    fixture.start();
    const std::string body = "5\r\nhello\r\n0\r\n\r\n";

    SECTION("Coding list ending in chunked") {
        require_rejected(fixture, post_headers("Transfer-Encoding: gzip, chunked\r\n") + body);
    }
    SECTION("Chunked twice in a list") {
        require_rejected(fixture, post_headers("Transfer-Encoding: chunked, chunked\r\n") + body);
    }
    SECTION("Chunked in two headers") {
        require_rejected(fixture, post_headers("Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n") + body);
    }
    SECTION("Unknown coding") {
        require_rejected(fixture, post_headers("Transfer-Encoding: foo\r\n") + body);
    }
    SECTION("Identity") {
        require_rejected(fixture, post_headers("Transfer-Encoding: identity\r\n") + body);
    }
}

TEST_CASE("Content-Length together with Transfer-Encoding is rejected", "[server][framing][smuggling][integration]") {
    framing_server fixture;
    fixture.start();

    SECTION("Content-Length first") {
        require_rejected(fixture, post_headers("Content-Length: 3\r\nTransfer-Encoding: chunked\r\n") + "5\r\nhello\r\n0\r\n\r\n");
    }
    SECTION("Transfer-Encoding first") {
        require_rejected(fixture, post_headers("Transfer-Encoding: chunked\r\nContent-Length: 3\r\n") + "5\r\nhello\r\n0\r\n\r\n");
    }
}

TEST_CASE("Invalid Content-Length is rejected", "[server][framing][smuggling][integration]") {
    framing_server fixture;
    fixture.start();

    SECTION("Repeated with different values") {
        require_rejected(fixture, post_headers("Content-Length: 5\r\nContent-Length: 6\r\n") + "hello!");
    }
    SECTION("Trailing garbage") {
        require_rejected(fixture, post_headers("Content-Length: 5abc\r\n") + "hello");
    }
    SECTION("Negative") {
        require_rejected(fixture, post_headers("Content-Length: -1\r\n") + "hello");
    }
    SECTION("Leading space") {
        require_rejected(fixture, post_headers("Content-Length:  5\r\n") + "hello");
    }
    SECTION("Plus sign") {
        require_rejected(fixture, post_headers("Content-Length: +5\r\n") + "hello");
    }
    SECTION("Empty") {
        require_rejected(fixture, post_headers("Content-Length: \r\n") + "hello");
    }
    SECTION("Overflow") {
        require_rejected(fixture, post_headers("Content-Length: 99999999999999999999999\r\n") + "hello");
    }
    SECTION("List of values") {
        require_rejected(fixture, post_headers("Content-Length: 5, 5\r\n") + "hello");
    }
}

TEST_CASE("Repeated Content-Length with the same value is accepted", "[server][framing][integration]") {
    framing_server fixture;
    fixture.start();

    auto result = raw_exchange(fixture.port, post_headers("Content-Length: 5\r\nContent-Length: 5\r\n") + "hello" + health_close_request);
    REQUIRE(result.data.starts_with("HTTP/1.1 200"));
    REQUIRE(result.data.find("body=[hello]") != std::string::npos);
    REQUIRE(result.data.find("healthy") != std::string::npos);
}

TEST_CASE("Chunk extensions are skipped, not read as the chunk size", "[server][framing][chunked-request][smuggling][integration]") {
    framing_server fixture;
    fixture.start();

    SECTION("Last chunk with an extension ends the body") {
        // "0;a" is the last chunk: the "a" of the extension is not a hex digit of the size
        auto result = raw_exchange(fixture.port, chunked_post("5\r\nhello\r\n0;a\r\n\r\n") + health_close_request);
        INFO("response: " << result.data);
        REQUIRE(count(result.data, "HTTP/1.1 200") == 2);
        REQUIRE(result.data.find("body=[hello]") != std::string::npos);
        REQUIRE(result.data.find("healthy") != std::string::npos);
    }

    SECTION("Extensions with values, on any chunk") {
        auto result = raw_exchange(fixture.port,
            chunked_post("5;name=value\r\nhello\r\n1;ext;other=\"quoted\"\r\n!\r\n0;last=1\r\n\r\n") + health_close_request);
        INFO("response: " << result.data);
        REQUIRE(count(result.data, "HTTP/1.1 200") == 2);
        REQUIRE(result.data.find("body=[hello!]") != std::string::npos);
    }
}

TEST_CASE("Malformed chunked bodies are rejected with 400 and close", "[server][framing][chunked-request][smuggling][integration]") {
    framing_server fixture;
    fixture.start();

    SECTION("Non-hex chunk size") {
        require_rejected(fixture, chunked_post("zz\r\nhello\r\n0\r\n\r\n"));
    }
    SECTION("Empty chunk size") {
        require_rejected(fixture, chunked_post("\r\nhello\r\n0\r\n\r\n"));
    }
    SECTION("Empty chunk size with an extension") {
        require_rejected(fixture, chunked_post(";ext\r\nhello\r\n0\r\n\r\n"));
    }
    SECTION("Chunk size overflow") {
        require_rejected(fixture, chunked_post("1ffffffffffffffff\r\nhello\r\n0\r\n\r\n"));
    }
    SECTION("Whitespace after the chunk size") {
        require_rejected(fixture, chunked_post("5 \r\nhello\r\n0\r\n\r\n"));
    }
    SECTION("Data longer than the chunk size") {
        require_rejected(fixture, chunked_post("5\r\nhelloXX\r\n0\r\n\r\n"));
    }
    SECTION("Missing CRLF after the data") {
        require_rejected(fixture, chunked_post("5\r\nhello0\r\n\r\n"));
    }
    SECTION("Bare LF after the chunk size") {
        require_rejected(fixture, chunked_post("5\nhello\r\n0\r\n\r\n"));
    }
    SECTION("Bare LF in the trailer section") {
        require_rejected(fixture, chunked_post("5\r\nhello\r\n0\r\nX-Trailer: a\n\r\n"));
    }
}

TEST_CASE("Trailer section is consumed completely", "[server][framing][chunked-request][pipelining][integration]") {
    framing_server fixture;
    fixture.start();

    auto result = raw_exchange(fixture.port,
        chunked_post("5\r\nhello\r\n0\r\nX-Trailer: a\r\nX-Other: b\r\n\r\n") + health_request + health_close_request);
    INFO("response: " << result.data);
    REQUIRE(count(result.data, "HTTP/1.1 ") == 3);
    REQUIRE(count(result.data, "HTTP/1.1 200") == 3);
    REQUIRE(result.data.find("body=[hello]") != std::string::npos);
    REQUIRE(count(result.data, "healthy") == 2);
}

TEST_CASE("Requests pipelined after a small chunked body are not lost", "[server][framing][chunked-request][pipelining][integration]") {
    framing_server fixture;
    fixture.start();

    // Well over the decoder batch size: all of it arrives with the chunked request headers
    std::string pipelined = chunked_post("5\r\nhello\r\n0\r\n\r\n");
    const int follow_ups = 40;
    for (int i = 0; i < follow_ups - 1; i++) pipelined += health_request;
    pipelined += health_close_request;
    REQUIRE(pipelined.size() > 1500);

    auto result = raw_exchange(fixture.port, pipelined);
    INFO("response: " << result.data);
    REQUIRE(count(result.data, "HTTP/1.1 200") == follow_ups + 1);
    REQUIRE(result.data.find("body=[hello]") != std::string::npos);
    REQUIRE(count(result.data, "healthy") == follow_ups);
    REQUIRE(result.closed);
}

TEST_CASE("Chunked body read in small pieces by the handler", "[server][framing][chunked-request][deferred][pipelining][integration]") {
    framing_server fixture;
    fixture.server.post("/pieces", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
        std::string received;
        uint8_t buffer[3];
        while (size_t bytes = co_await req.read(buffer, sizeof(buffer))) {
            received.append(reinterpret_cast<char*>(buffer), bytes);
        }
        res.send("pieces=[" + received + "]");
    });
    fixture.start();

    std::string body = "4\r\nThe \r\n6\r\nquick \r\n6;x=y\r\nbrown \r\n3\r\nfox\r\n0\r\nTrailer: t\r\n\r\n";
    std::string pipelined = "POST /pieces HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n" + body;
    for (int i = 0; i < 20; i++) pipelined += health_request;
    pipelined += health_close_request;

    auto result = raw_exchange(fixture.port, pipelined);
    INFO("response: " << result.data);
    REQUIRE(result.data.find("pieces=[The quick brown fox]") != std::string::npos);
    REQUIRE(count(result.data, "HTTP/1.1 200") == 22);
    REQUIRE(count(result.data, "healthy") == 21);
}

TEST_CASE("Chunked body split across many TCP writes", "[server][framing][chunked-request][integration]") {
    framing_server fixture;
    fixture.start();

    // Send one byte at a time: every framing state has to wait for more data
    std::string request = chunked_post("5;e=1\r\nhello\r\n6\r\n world\r\n0\r\nT: v\r\n\r\n") + health_close_request;

    boost::asio::io_context ioc;
    boost::asio::ip::tcp::socket sock(ioc);
    sock.connect({boost::asio::ip::make_address("127.0.0.1"), fixture.port});
    sock.set_option(boost::asio::ip::tcp::no_delay(true));
    for (char c : request) {
        boost::asio::write(sock, boost::asio::buffer(&c, 1));
        std::this_thread::sleep_for(1ms);
    }

    auto data = read_until_closed(ioc, sock, 3s).data;

    INFO("response: " << data);
    REQUIRE(data.find("body=[hello world]") != std::string::npos);
    REQUIRE(count(data, "HTTP/1.1 200") == 2);
}

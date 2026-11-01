#include "server_test_helpers.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <atomic>
#include <memory>

using namespace thinger;
using namespace std::chrono_literals;

// Request body framing over raw bytes (on every transport): Content-Length / Transfer-Encoding validation and
// strict chunked decoding, so that a request body can never be read as another request
// (request smuggling), nor the next request be lost

namespace {

    using namespace server_test;

    struct framing_server {
        http::server server;
        std::unique_ptr<running_server> running;
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

        void start(transport kind) {
            running = std::make_unique<running_server>(server, kind);
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

    // The request is answered with a single 400 (or `status`), the connection is closed,
    // and nothing sent after it is run as another request
    void require_rejected(framing_server& fixture, const std::string& request, const std::string& status = "400") {
        auto result = raw_exchange(*fixture.running, request + smuggled_request);
        INFO("response: " << result.data);
        REQUIRE(result.data.starts_with("HTTP/1.1 " + status));
        REQUIRE(count(result.data, "HTTP/1.1 ") == 1);
        REQUIRE(result.closed);
        REQUIRE(fixture.smuggled == 0);
    }
}

TEST_CASE("Transfer-Encoding other than chunked is rejected", "[server][framing][smuggling][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    framing_server fixture;
    fixture.start(kind);
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
    SECTION("Empty") {
        require_rejected(fixture, post_headers("Transfer-Encoding:\r\n") + body);
    }
    SECTION("Only whitespace") {
        require_rejected(fixture, post_headers("Transfer-Encoding: \t\r\n") + body);
    }
}

TEST_CASE("Transfer-Encoding in an HTTP/1.0 request is rejected", "[server][framing][smuggling][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    framing_server fixture;
    fixture.start(kind);

    // RFC 9112 section 6.1: an HTTP/1.0 message with Transfer-Encoding has faulty framing
    SECTION("Chunked") {
        require_rejected(fixture, "POST /echo HTTP/1.0\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
    }
    SECTION("Chunked on a keep-alive connection") {
        require_rejected(fixture, "POST /echo HTTP/1.0\r\nHost: localhost\r\nConnection: keep-alive\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
    }
}

TEST_CASE("Content-Length together with Transfer-Encoding is rejected", "[server][framing][smuggling][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    framing_server fixture;
    fixture.start(kind);

    SECTION("Content-Length first") {
        require_rejected(fixture, post_headers("Content-Length: 3\r\nTransfer-Encoding: chunked\r\n") + "5\r\nhello\r\n0\r\n\r\n");
    }
    SECTION("Transfer-Encoding first") {
        require_rejected(fixture, post_headers("Transfer-Encoding: chunked\r\nContent-Length: 3\r\n") + "5\r\nhello\r\n0\r\n\r\n");
    }
}

TEST_CASE("Invalid Content-Length is rejected", "[server][framing][smuggling][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    framing_server fixture;
    fixture.start(kind);

    SECTION("Repeated with different values") {
        require_rejected(fixture, post_headers("Content-Length: 5\r\nContent-Length: 6\r\n") + "hello!");
    }
    SECTION("Trailing garbage") {
        require_rejected(fixture, post_headers("Content-Length: 5abc\r\n") + "hello");
    }
    SECTION("Negative") {
        require_rejected(fixture, post_headers("Content-Length: -1\r\n") + "hello");
    }
    // (Optional whitespace around the value is accepted: see "Field values with optional
    // whitespace frame the body")
    SECTION("Space inside the value") {
        require_rejected(fixture, post_headers("Content-Length: 5 5\r\n") + "hello");
    }
    SECTION("Plus sign") {
        require_rejected(fixture, post_headers("Content-Length: +5\r\n") + "hello");
    }
    SECTION("Empty") {
        require_rejected(fixture, post_headers("Content-Length: \r\n") + "hello");
    }
    SECTION("Empty, without whitespace") {
        require_rejected(fixture, post_headers("Content-Length:\r\n") + "hello");
    }
    SECTION("Only whitespace") {
        require_rejected(fixture, post_headers("Content-Length: \t \r\n") + "hello");
    }
    SECTION("List of values without a space") {
        require_rejected(fixture, post_headers("Content-Length: 5,5\r\n") + "hello");
    }
    SECTION("Overflow") {
        require_rejected(fixture, post_headers("Content-Length: 99999999999999999999999\r\n") + "hello");
    }
    SECTION("List of values") {
        require_rejected(fixture, post_headers("Content-Length: 5, 5\r\n") + "hello");
    }
}

// RFC 9112 section 5.1 allows optional whitespace (SP or HTAB) before and after a field value,
// which is not part of it (RFC 9110 section 5.5): Node (llhttp) and Go accept these forms, which
// hand-written HTTP stacks of devices send, and frame the body with the trimmed value. The
// framing fields stay strict on that value
TEST_CASE("Field values with optional whitespace frame the body", "[server][framing][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    framing_server fixture;
    fixture.start(kind);

    auto require_echoed = [&](const std::string& request) {
        auto result = raw_exchange(*fixture.running, request + health_close_request);
        INFO("response: " << result.data);
        REQUIRE(count(result.data, "HTTP/1.1 200") == 2);
        REQUIRE(result.data.find("body=[hello]") != std::string::npos);
        REQUIRE(result.data.find("healthy") != std::string::npos);
        REQUIRE(fixture.smuggled == 0);
    };

    // Node and Go: served with the 5-byte body
    SECTION("Content-Length with trailing SP") {
        require_echoed(post_headers("Content-Length: 5 \r\n") + "hello");
    }
    SECTION("Content-Length between HTABs") {
        require_echoed(post_headers("Content-Length:\t5\t\r\n") + "hello");
    }
    SECTION("Content-Length without whitespace") {
        require_echoed(post_headers("Content-Length:5\r\n") + "hello");
    }
    // Rejected in 0.9.0 as "Leading space"
    SECTION("Content-Length with two leading spaces") {
        require_echoed(post_headers("Content-Length:  5\r\n") + "hello");
    }
    // Node and Go: read as chunked
    SECTION("Transfer-Encoding with trailing SP") {
        require_echoed(post_headers("Transfer-Encoding: chunked \r\n") + "5\r\nhello\r\n0\r\n\r\n");
    }
    SECTION("Transfer-Encoding after an HTAB") {
        require_echoed(post_headers("Transfer-Encoding:\tchunked\r\n") + "5\r\nhello\r\n0\r\n\r\n");
    }
    SECTION("Other fields with any OWS") {
        require_echoed(post_headers("X-A:value\r\nX-B:\r\nX-C:\t a\tb \t\r\nContent-Length: 5\r\n") + "hello");
    }
}

// RFC 9112 section 5.1: a server MUST reject whitespace between a field name and the colon
// with 400; llhttp and Go reject it too
TEST_CASE("Whitespace before the colon of a field is rejected", "[server][framing][smuggling][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    framing_server fixture;
    fixture.start(kind);

    SECTION("Content-Length") {
        require_rejected(fixture, post_headers("Content-Length : 5\r\n") + "hello");
    }
    SECTION("Transfer-Encoding") {
        require_rejected(fixture, post_headers("Transfer-Encoding\t: chunked\r\n") + "5\r\nhello\r\n0\r\n\r\n");
    }
    SECTION("Other field") {
        require_rejected(fixture, "GET /health HTTP/1.1\r\nHost: localhost\r\nX-A\t: b\r\n\r\n");
    }
}

TEST_CASE("Repeated Content-Length with the same value is accepted", "[server][framing][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    framing_server fixture;
    fixture.start(kind);

    auto result = raw_exchange(*fixture.running, post_headers("Content-Length: 5\r\nContent-Length: 5\r\n") + "hello" + health_close_request);
    REQUIRE(result.data.starts_with("HTTP/1.1 200"));
    REQUIRE(result.data.find("body=[hello]") != std::string::npos);
    REQUIRE(result.data.find("healthy") != std::string::npos);
}

TEST_CASE("Chunk extensions are skipped, not read as the chunk size", "[server][framing][chunked-request][smuggling][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    framing_server fixture;
    fixture.start(kind);

    SECTION("Last chunk with an extension ends the body") {
        // "0;a" is the last chunk: the "a" of the extension is not a hex digit of the size
        auto result = raw_exchange(*fixture.running, chunked_post("5\r\nhello\r\n0;a\r\n\r\n") + health_close_request);
        INFO("response: " << result.data);
        REQUIRE(count(result.data, "HTTP/1.1 200") == 2);
        REQUIRE(result.data.find("body=[hello]") != std::string::npos);
        REQUIRE(result.data.find("healthy") != std::string::npos);
    }

    SECTION("Extensions with values, on any chunk") {
        auto result = raw_exchange(*fixture.running,
            chunked_post("5;name=value\r\nhello\r\n1;ext;other=\"quoted\"\r\n!\r\n0;last=1\r\n\r\n") + health_close_request);
        INFO("response: " << result.data);
        REQUIRE(count(result.data, "HTTP/1.1 200") == 2);
        REQUIRE(result.data.find("body=[hello!]") != std::string::npos);
    }
}

TEST_CASE("Malformed chunked bodies are rejected with 400 and close", "[server][framing][chunked-request][smuggling][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    framing_server fixture;
    fixture.start(kind);

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
    // Found by differential fuzzing against Boost.Beast: without the final CRLF, the next
    // request was read as the trailer section (its request line taken as a field line), so
    // the server and a front end that ends the body at "0\r\n" disagreed on the requests
    SECTION("Request following a body without its final CRLF") {
        require_rejected(fixture, chunked_post("5\r\nhello\r\n0\r\n"));
    }
    SECTION("Trailer line that is not a field line") {
        require_rejected(fixture, chunked_post("5\r\nhello\r\n0\r\nX-Trailer\r\n\r\n"));
    }
    SECTION("Chunk extension not following the grammar") {
        require_rejected(fixture, chunked_post("5;a=b c\r\nhello\r\n0\r\n\r\n"));
    }
}

TEST_CASE("Trailer section is consumed completely", "[server][framing][chunked-request][pipelining][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    framing_server fixture;
    fixture.start(kind);

    auto result = raw_exchange(*fixture.running,
        chunked_post("5\r\nhello\r\n0\r\nX-Trailer: a\r\nX-Other: b\r\n\r\n") + health_request + health_close_request);
    INFO("response: " << result.data);
    REQUIRE(count(result.data, "HTTP/1.1 ") == 3);
    REQUIRE(count(result.data, "HTTP/1.1 200") == 3);
    REQUIRE(result.data.find("body=[hello]") != std::string::npos);
    REQUIRE(count(result.data, "healthy") == 2);
}

TEST_CASE("Requests pipelined after a small chunked body are not lost", "[server][framing][chunked-request][pipelining][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    framing_server fixture;
    fixture.start(kind);

    // Well over the decoder batch size: all of it arrives with the chunked request headers
    std::string pipelined = chunked_post("5\r\nhello\r\n0\r\n\r\n");
    const int follow_ups = 40;
    for (int i = 0; i < follow_ups - 1; i++) pipelined += health_request;
    pipelined += health_close_request;
    REQUIRE(pipelined.size() > 1500);

    auto result = raw_exchange(*fixture.running, pipelined);
    INFO("response: " << result.data);
    REQUIRE(count(result.data, "HTTP/1.1 200") == follow_ups + 1);
    REQUIRE(result.data.find("body=[hello]") != std::string::npos);
    REQUIRE(count(result.data, "healthy") == follow_ups);
    REQUIRE(result.closed);
}

TEST_CASE("Chunked body read in small pieces by the handler", "[server][framing][chunked-request][deferred][pipelining][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    framing_server fixture;
    fixture.server.post("/pieces", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
        std::string received;
        uint8_t buffer[3];
        while (size_t bytes = co_await req.read(buffer, sizeof(buffer))) {
            received.append(reinterpret_cast<char*>(buffer), bytes);
        }
        res.send("pieces=[" + received + "]");
    });
    fixture.start(kind);

    std::string body = "4\r\nThe \r\n6\r\nquick \r\n6;x=y\r\nbrown \r\n3\r\nfox\r\n0\r\nTrailer: t\r\n\r\n";
    std::string pipelined = "POST /pieces HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n" + body;
    for (int i = 0; i < 20; i++) pipelined += health_request;
    pipelined += health_close_request;

    auto result = raw_exchange(*fixture.running, pipelined);
    INFO("response: " << result.data);
    REQUIRE(result.data.find("pieces=[The quick brown fox]") != std::string::npos);
    REQUIRE(count(result.data, "HTTP/1.1 200") == 22);
    REQUIRE(count(result.data, "healthy") == 21);
}

TEST_CASE("Chunked body split across many writes", "[server][framing][chunked-request][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    framing_server fixture;
    fixture.start(kind);

    // Send one byte at a time (a TLS record each over TLS): every framing state has to wait
    // for more data
    std::string request = chunked_post("5;e=1\r\nhello\r\n6\r\n world\r\n0\r\nT: v\r\n\r\n") + health_close_request;

    http::test::raw_connection connection(fixture.running->endpoint(), true);
    for (char c : request) {
        REQUIRE(connection.write(std::string_view(&c, 1)));
        std::this_thread::sleep_for(1ms);
    }

    auto data = connection.read_until_closed(3s).data;

    INFO("response: " << data);
    REQUIRE(data.find("body=[hello world]") != std::string::npos);
    REQUIRE(count(data, "HTTP/1.1 200") == 2);
}

TEST_CASE("Folded header lines (obs-fold) are rejected", "[server][framing][smuggling][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    framing_server fixture;
    fixture.start(kind);

    // A line starting with whitespace continues the previous one (obsolete line folding):
    // it must never be merged into the previous header name, which turned
    // "Transfer-Encodin: chunked\r\n g" into "Transfer-Encoding: chunked"
    SECTION("Folding completes Transfer-Encoding") {
        require_rejected(fixture, post_headers("Transfer-Encodin: chunked\r\n g\r\n") + "5\r\nhello\r\n0\r\n\r\n");
    }
    SECTION("Folding completes Content-Length") {
        require_rejected(fixture, post_headers("Content-Lengt: 5\r\n h\r\n") + "hello");
    }
    SECTION("Folding completes Host") {
        require_rejected(fixture, "GET /health HTTP/1.1\r\nHos: evil.example\r\n t\r\n\r\n");
    }
    SECTION("Folding with a tab") {
        require_rejected(fixture, "GET /health HTTP/1.1\r\nHost: localhost\r\nX-Custom: a\r\n\tb\r\n\r\n");
    }
    SECTION("Folding a value") {
        require_rejected(fixture, "GET /health HTTP/1.1\r\nHost: localhost\r\nX-Custom: a\r\n b\r\n\r\n");
    }
    SECTION("Whitespace before the first header") {
        require_rejected(fixture, "GET /health HTTP/1.1\r\n Host: localhost\r\n\r\n");
    }
}

// Found by differential fuzzing against llhttp (tests/fuzz/fuzz_http_differential3.cpp):
// CONNECT is not served, and what follows its header section is tunnel data, not content nor
// another request (RFC 9110 section 9.3.6). Reading a body or a next request after it framed
// the stream unlike a proxy that implements CONNECT
TEST_CASE("CONNECT requests are rejected", "[server][framing][smuggling][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    framing_server fixture;
    fixture.start(kind);

    SECTION("Authority form") {
        require_rejected(fixture, "CONNECT localhost:443 HTTP/1.1\r\nHost: localhost:443\r\n\r\n");
    }
    SECTION("Origin form, with a body") {
        require_rejected(fixture, "CONNECT /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello");
    }
}

// Found by differential fuzzing against llhttp: bytes above 0x7F were accepted in the request
// target. They must be percent-encoded (RFC 9112 section 3.2), and an invalid request line is
// rejected rather than autocorrected (RFC 9112 section 3)
TEST_CASE("Request target with bytes other than ASCII is rejected", "[server][framing][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    framing_server fixture;
    fixture.start(kind);

    SECTION("UTF-8 in the path") {
        require_rejected(fixture, "GET /health\xc3\xa9 HTTP/1.1\r\nHost: localhost\r\n\r\n");
    }
    SECTION("Byte above 0x7F in the query") {
        require_rejected(fixture, "GET /health?a=\xff HTTP/1.1\r\nHost: localhost\r\n\r\n");
    }
    SECTION("Percent-encoded is accepted") {
        auto result = raw_exchange(*fixture.running, "GET /health?a=%C3%A9 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        INFO("response: " << result.data);
        REQUIRE(result.data.starts_with("HTTP/1.1 200"));
    }
}

// Found by differential fuzzing against llhttp: the target was checked to start with "/" once
// percent-decoded, so "%2Fhealth" was served as "/health", though it is not in origin form
// (RFC 9112 section 3.2.1)
TEST_CASE("Request target starting with an encoded slash is rejected", "[server][framing][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    framing_server fixture;
    fixture.start(kind);
    require_rejected(fixture, "GET %2Fhealth HTTP/1.1\r\nHost: localhost\r\n\r\n");
}

namespace {

    // Routes that echo what they see of the target
    struct target_server {
        http::server server;
        std::unique_ptr<running_server> running;

        target_server() {
            server.get("/api/items", [](http::request& req, http::response& res) {
                res.send("range=[" + req.query("range") + "]");
            });
            server.get("/files/:file", [](http::request& req, http::response& res) {
                res.send("file=[" + req["file"] + "]");
            });
            server.get("/p/:name", [](http::request& req, http::response& res) {
                res.send("path=[" + req.get_http_request()->get_path() + "] name=[" + req["name"] + "] resource=[" +
                         req.get_http_request()->get_resource() + "] q=[" + req.query("q") + "]");
            });
        }

        void start(transport kind) {
            running = std::make_unique<running_server>(server, kind);
        }

        std::string get(const std::string& target) {
            auto result = raw_exchange(*running, "GET " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
            return result.data;
        }
    };
}

// Only ".." path segments are refused; Node and Go accept "1..5" in a query or "v1..v2.txt" in a
// path, which are no traversal
TEST_CASE("Request targets with \"..\" outside path segments are served", "[server][framing][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    target_server fixture;
    fixture.start(kind);

    auto range = fixture.get("/api/items?range=1..5");
    INFO("response: " << range);
    REQUIRE(range.starts_with("HTTP/1.1 200"));
    REQUIRE(range.find("range=[1..5]") != std::string::npos);

    auto file = fixture.get("/files/v1..v2.txt");
    INFO("response: " << file);
    REQUIRE(file.starts_with("HTTP/1.1 200"));
    REQUIRE(file.find("file=[v1..v2.txt]") != std::string::npos);

    for (const char* target : {"/files/...", "/files/..a", "/files/a..", "/api/items?q=a%2E%2Eb", "/api/items?../.."}) {
        INFO("target: " << target);
        REQUIRE(fixture.get(target).starts_with("HTTP/1.1 200"));
    }
}

// A ".." segment of the percent-decoded path is refused with 400 before any route, as in 0.9.0
TEST_CASE("Request targets with \"..\" path segments are rejected", "[server][framing][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    framing_server fixture;
    fixture.start(kind);
    for (const char* target : {"/..", "/../health", "/a/../health", "/health/..", "/health/..?x=1", "/%2E%2E/health",
                               "/.%2e/health", "/a%2F..%2Fhealth"}) {
        INFO("target: " << target);
        require_rejected(fixture, "GET " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
    }
}

// "+" is literal in a path (Node's URL.pathname, Go's URL.Path) and a space in the query (Node's
// URLSearchParams, Go's URL.Query())
TEST_CASE("Plus is literal in the path and a space in the query", "[server][framing][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    target_server fixture;
    fixture.start(kind);

    auto plus = fixture.get("/p/a+b?q=c+d");
    INFO("response: " << plus);
    REQUIRE(plus.starts_with("HTTP/1.1 200"));
    REQUIRE(plus.find("path=[/p/a+b] name=[a+b] resource=[/p/a+b] q=[c d]") != std::string::npos);

    auto encoded = fixture.get("/p/a%2Bb");
    INFO("response: " << encoded);
    REQUIRE(encoded.starts_with("HTTP/1.1 200"));
    REQUIRE(encoded.find("resource=[/p/a+b]") != std::string::npos);
}

namespace {

    // A route and a middleware that count the requests reaching them
    struct host_server {
        http::server server;
        std::unique_ptr<running_server> running;
        std::atomic<int> middleware_calls{0};
        std::atomic<int> handler_calls{0};

        host_server() {
            server.use([this](http::request&, http::response&, std::function<void()> next) {
                middleware_calls++;
                next();
            });
            server.get("/host", [this](http::request& req, http::response& res) {
                handler_calls++;
                res.send("host=[" + req.get_http_request()->get_host() + "]");
            });
        }

        void start(transport kind) {
            running = std::make_unique<running_server>(server, kind);
        }

        // Answered 400 and closed, before any middleware or handler, and nothing sent after it
        // runs as another request
        void require_rejected(const std::string& head) {
            auto result = raw_exchange(*running, head + "\r\n" + "GET /host HTTP/1.1\r\nHost: localhost\r\n\r\n");
            INFO("response: " << result.data);
            REQUIRE(result.data.starts_with("HTTP/1.1 400"));
            REQUIRE(count(result.data, "HTTP/1.1 ") == 1);
            REQUIRE(result.closed);
            REQUIRE(middleware_calls == 0);
            REQUIRE(handler_calls == 0);
        }

        std::string served(const std::string& head) {
            auto result = raw_exchange(*running, head + "Connection: close\r\n\r\n");
            INFO("response: " << result.data);
            REQUIRE(result.data.starts_with("HTTP/1."));
            REQUIRE(result.data.find(" 200 ") != std::string::npos);
            return result.data;
        }
    };
}

// RFC 9110 section 7.2: an HTTP/1.1 request without Host is answered 400, as Node
// (requireHostHeader) and Go ("missing required Host header") do; HTTP/1.0 may omit it, and an
// empty Host is present (Node and Go check presence only)
TEST_CASE("HTTP/1.1 requests without Host are rejected", "[server][framing][host][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    host_server fixture;
    fixture.start(kind);

    SECTION("HTTP/1.1 without Host: 400 and close") {
        fixture.require_rejected("GET /host HTTP/1.1\r\n");
        fixture.require_rejected("GET /host HTTP/1.1\r\nX-Host: localhost\r\n");
    }
    SECTION("HTTP/1.0 without Host is served") {
        auto response = fixture.served("GET /host HTTP/1.0\r\n");
        REQUIRE(response.find("host=[]") != std::string::npos);
        REQUIRE(fixture.handler_calls == 1);
    }
    SECTION("HTTP/1.1 with an empty Host is served by the default host") {
        auto response = fixture.served("GET /host HTTP/1.1\r\nHost:\r\n");
        REQUIRE(response.find("host=[]") != std::string::npos);
        fixture.served("GET /host HTTP/1.1\r\nHost: \r\n");
        REQUIRE(fixture.handler_calls == 2);
    }
}

// RFC 9110 section 7.2: more than one Host field is answered 400. Go answers 400 ("too many Host
// headers"), Node keeps the first one; they differ, so the RFC decides. Two Hosts may route
// differently along a proxy chain
TEST_CASE("Requests with more than one Host are rejected", "[server][framing][host][smuggling][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    host_server fixture;
    fixture.start(kind);

    SECTION("Different values") {
        fixture.require_rejected("GET /host HTTP/1.1\r\nHost: localhost\r\nHost: evil.example\r\n");
    }
    SECTION("Equal values") {
        fixture.require_rejected("GET /host HTTP/1.1\r\nHost: localhost\r\nHost: localhost\r\n");
    }
    SECTION("Any letter case") {
        fixture.require_rejected("GET /host HTTP/1.1\r\nHost: localhost\r\nhost: localhost\r\n");
        fixture.require_rejected("GET /host HTTP/1.1\r\nHOST: a\r\nX-A: b\r\nhOsT: b\r\n");
    }
    SECTION("In HTTP/1.0") {
        fixture.require_rejected("GET /host HTTP/1.0\r\nHost: localhost\r\nHost: localhost\r\n");
    }
}

namespace {
    // GET /health with `count` headers in all (Host included)
    std::string request_with_headers(int count) {
        std::string request = "GET /health HTTP/1.1\r\nHost: localhost\r\n";
        for (int i = 1; i < count; i++) request += "X-Header-" + std::to_string(i) + ": value\r\n";
        return request + "\r\n";
    }

    // GET /health whose request line and headers take `size` bytes
    std::string request_of_size(size_t size) {
        std::string head = "GET /health HTTP/1.1\r\nHost: localhost\r\nX-Big: ";
        std::string tail = "\r\n\r\n";
        return head + std::string(size - head.size() - tail.size(), 'a') + tail;
    }
}

TEST_CASE("Request header section limits", "[server][framing][limits][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    framing_server fixture;
    fixture.start(kind);

    SECTION("100 header lines are accepted") {
        auto result = raw_exchange(*fixture.running, request_with_headers(100) + health_close_request);
        INFO("response: " << result.data);
        REQUIRE(count(result.data, "HTTP/1.1 200") == 2);
    }
    SECTION("More than 100 header lines are rejected with 431") {
        require_rejected(fixture, request_with_headers(101), "431");
    }
    SECTION("Many repeated Content-Length headers are rejected with 431") {
        std::string request = post_headers("");
        request.resize(request.size() - 2);
        for (int i = 0; i < 150; i++) request += "Content-Length: 5\r\n";
        require_rejected(fixture, request + "\r\nhello", "431");
    }
    SECTION("A 16 KB header section is accepted") {
        auto result = raw_exchange(*fixture.running, request_of_size(16 * 1024) + health_close_request);
        INFO("response: " << result.data.substr(0, 200));
        REQUIRE(count(result.data, "HTTP/1.1 200") == 2);
    }
    SECTION("A header section over 16 KB is rejected with 431") {
        require_rejected(fixture, request_of_size(16 * 1024 + 1), "431");
    }
    // A target alone over the 16 KB section: 431, as Node (HPE_HEADER_OVERFLOW) and Go
    // (MaxHeaderBytes) answer, since the request line and the headers share one budget
    SECTION("A request line over 16 KB is rejected with 431") {
        require_rejected(fixture, "GET /health/" + std::string(16 * 1024, 'a') + " HTTP/1.1\r\nHost: localhost\r\n\r\n", "431");
        require_rejected(fixture, "GET /health?q=" + std::string(16 * 1024, 'a') + " HTTP/1.1\r\nHost: localhost\r\n\r\n", "431");
    }
}

#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/response_factory.hpp>
#include <thinger/http/common/http_response.hpp>
#include <string>
#include <vector>

using namespace thinger::http;
using namespace std::string_literals;
using body_framing = response_factory::body_framing;

namespace {

    struct parse_result {
        boost::tribool result = boost::indeterminate;
        client_error error = client_error::none;
        size_t consumed = 0;                    // bytes of the input the response took
        bool keep_alive = false;
        body_framing framing = body_framing::none;
        std::shared_ptr<http_response> response;
    };

    // Parse `raw` as the response to a `request_method` request, in pieces of `piece` bytes
    // (all at once by default), then end the stream if `close` and it is still incomplete
    parse_result parse_response(const std::string& raw, method request_method = method::GET, bool close = false,
                                size_t piece = std::string::npos, response_factory* configured = nullptr) {
        response_factory own;
        auto& parser = configured ? *configured : own;
        parser.set_request_method(request_method);
        parse_result r;
        size_t offset = 0;
        while (offset < raw.size() && boost::indeterminate(r.result)) {
            std::string part = raw.substr(offset, piece);
            const char* begin = part.data();
            r.result = parser.parse_some(begin, part.data() + part.size());
            offset += static_cast<size_t>(begin - part.data());
        }
        if (close && boost::indeterminate(r.result)) r.result = parser.end_of_stream();
        r.error = parser.error();
        r.consumed = offset;
        r.keep_alive = parser.keep_alive();
        r.framing = parser.get_body_framing();
        if (r.result) r.response = parser.consume_response();
        return r;
    }

    void require_error(const std::string& raw, client_error error, method request_method = method::GET) {
        INFO(raw);
        auto r = parse_response(raw, request_method, true);
        REQUIRE(bool(!r.result));
        REQUIRE(r.error == error);
        // however it is split
        auto bytewise = parse_response(raw, request_method, true, 1);
        REQUIRE(bool(!bytewise.result));
        REQUIRE(bytewise.error == error);
    }

    parse_result require_complete(const std::string& raw, method request_method = method::GET, bool close = false) {
        INFO(raw);
        auto r = parse_response(raw, request_method, close);
        REQUIRE(bool(r.result));
        auto bytewise = parse_response(raw, request_method, close, 1);
        REQUIRE(bool(bytewise.result));
        REQUIRE(bytewise.consumed == r.consumed);
        REQUIRE(bytewise.response->get_content() == r.response->get_content());
        REQUIRE(bytewise.response->get_status_code() == r.response->get_status_code());
        return r;
    }

    std::string response_with(const std::string& headers, const std::string& body = "") {
        return "HTTP/1.1 200 OK\r\n" + headers + "\r\n" + body;
    }
}

TEST_CASE("Response factory parses a response with headers", "[response_factory][unit]") {
    response_factory parser;
    std::string raw = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-A: 1\r\n\r\nhello";
    auto result = parser.parse(raw.begin(), raw.end());
    REQUIRE(bool(result));
    auto response = parser.consume_response();
    REQUIRE(response->get_content_length() == 5);
    REQUIRE(response->get_header("X-A") == "1");
    REQUIRE(response->get_content() == "hello");
}

TEST_CASE("Response factory rejects folded header lines (obs-fold)", "[response_factory][unit]") {
    // A line starting with whitespace must never be merged into the previous header name,
    // which turned "Content-Lengt: 5\r\n h" into "Content-Length: 5"
    SECTION("Folding completes Content-Length") {
        require_error("HTTP/1.1 200 OK\r\nContent-Lengt: 5\r\n h\r\n\r\nhello", client_error::invalid_header);
    }
    SECTION("Folding a value with a tab") {
        require_error("HTTP/1.1 200 OK\r\nX-A: 1\r\n\t2\r\nContent-Length: 0\r\n\r\n", client_error::invalid_header);
    }
}

TEST_CASE("Response factory status line", "[response_factory][unit]") {
    SECTION("Version, status and reason") {
        auto r = require_complete("HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n");
        REQUIRE(r.response->get_http_version_major() == 1);
        REQUIRE(r.response->get_http_version_minor() == 0);
        REQUIRE(r.response->get_status_code() == 404);
        REQUIRE(r.response->get_reason() == "Not Found");
    }
    SECTION("Empty reason, with or without the space before it") {
        REQUIRE(require_complete("HTTP/1.1 204 \r\n\r\n").response->get_reason().empty());
        REQUIRE(require_complete("HTTP/1.1 204\r\n\r\n").response->get_status_code() == 204);
    }
    SECTION("Reason with tabs and bytes over 0x7F") {
        REQUIRE(require_complete("HTTP/1.1 200 \tOK \xc3\xa9\r\nContent-Length: 0\r\n\r\n").response->get_reason() == "\tOK \xc3\xa9");
    }
    SECTION("A higher HTTP/1 minor version is HTTP/1.1") {
        auto r = require_complete("HTTP/1.2 200 OK\r\nContent-Length: 0\r\n\r\n");
        REQUIRE(r.response->get_http_version_minor() == 2);
        REQUIRE(r.keep_alive);
    }
    SECTION("Invalid versions") {
        for (std::string line : {"HTTP/1.10 200 OK", "HTTP/2.0 200 OK", "HTTP/11.1 200 OK", "HTTP/0.9 200 OK",
                                 "HTTP/1. 200 OK", "HTTP/1 200 OK", "http/1.1 200 OK", "HTTP/1.1  200 OK", "ICE/1.0 200 OK"}) {
            require_error(line + "\r\nContent-Length: 0\r\n\r\n", client_error::invalid_status_line);
        }
    }
    SECTION("Status codes other than three digits from 100 to 599") {
        for (std::string line : {"HTTP/1.1 20 OK", "HTTP/1.1 2000 OK", "HTTP/1.1 099 OK", "HTTP/1.1 600 OK",
                                 "HTTP/1.1 000 OK", "HTTP/1.1 2x0 OK", "HTTP/1.1 -20 OK", "HTTP/1.1 +200 OK", "HTTP/1.1 200OK"}) {
            require_error(line + "\r\nContent-Length: 0\r\n\r\n", client_error::invalid_status_line);
        }
    }
    SECTION("Control characters in the reason") {
        require_error("HTTP/1.1 200 O\0K\r\nContent-Length: 0\r\n\r\n"s, client_error::invalid_status_line);
        require_error("HTTP/1.1 200 O\rK\r\nContent-Length: 0\r\n\r\n", client_error::invalid_status_line);
    }
    SECTION("Line terminators other than CRLF") {
        require_error("HTTP/1.1 200 OK\nContent-Length: 0\r\n\r\n", client_error::invalid_status_line);
        require_error("HTTP/1.1 200 OK\rContent-Length: 0\r\n\r\n", client_error::invalid_status_line);
    }
    SECTION("Empty lines before the status line") {
        require_error("\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", client_error::invalid_status_line);
    }
    SECTION("Another protocol fails from its first bytes, without waiting for a line") {
        response_factory parser;
        std::string raw = "SSH-2.0-OpenSSH_9.6";
        REQUIRE(bool(!parser.parse(raw.begin(), raw.end())));
        REQUIRE(parser.error() == client_error::invalid_status_line);
    }
}

TEST_CASE("Response factory header lines", "[response_factory][unit]") {
    SECTION("Optional whitespace around values is not part of them") {
        auto r = require_complete(response_with("A:1\r\nB:  2 \t\r\nC:\t3\r\nD:\r\nContent-Length:0\r\n"));
        REQUIRE(r.response->get_header("A") == "1");
        REQUIRE(r.response->get_header("B") == "2");
        REQUIRE(r.response->get_header("C") == "3");
        REQUIRE(r.response->has_header("D"));
        REQUIRE(r.response->get_header("D").empty());
    }
    SECTION("Inner whitespace and bytes over 0x7F are kept") {
        auto r = require_complete(response_with("X-A: a \t b\xff\r\nContent-Length: 0\r\n"));
        REQUIRE(r.response->get_header("X-A") == "a \t b\xff");
    }
    SECTION("Invalid names") {
        require_error(response_with("Content-Length : 0\r\n"), client_error::invalid_header);
        require_error(response_with(": 0\r\nContent-Length: 0\r\n"), client_error::invalid_header);
        require_error(response_with("X(A): 1\r\nContent-Length: 0\r\n"), client_error::invalid_header);
        require_error(response_with("No colon\r\nContent-Length: 0\r\n"), client_error::invalid_header);
        require_error(response_with("X-\xc3\xa9: 1\r\nContent-Length: 0\r\n"), client_error::invalid_header);
    }
    SECTION("Control characters in values") {
        require_error(response_with("X-A: a\0b\r\nContent-Length: 0\r\n"s), client_error::invalid_header);
        require_error(response_with("X-A: a\x7f\r\nContent-Length: 0\r\n"), client_error::invalid_header);
        require_error(response_with("X-A: a\rb\r\nContent-Length: 0\r\n"), client_error::invalid_header);
    }
    SECTION("Bare LF") {
        require_error(response_with("X-A: 1\nContent-Length: 0\r\n"), client_error::invalid_header);
        require_error("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\n", client_error::invalid_header);
    }
    SECTION("Header line limit") {
        std::string lines;
        for (size_t i = 1; i < response_factory::max_header_lines; i++) lines += "X-" + std::to_string(i) + ": a\r\n";
        require_complete(response_with(lines + "Content-Length: 0\r\n"));
        require_error(response_with(lines + "X-Over: a\r\nContent-Length: 0\r\n"), client_error::header_section_too_large);
    }
    SECTION("Header section size limit") {
        std::string head = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nX-Big: ";
        std::string fits = head + std::string(response_factory::max_header_section_size - head.size() - 4, 'a') + "\r\n\r\n";
        REQUIRE(fits.size() == response_factory::max_header_section_size);
        require_complete(fits);
        std::string over = head + std::string(response_factory::max_header_section_size - head.size() - 3, 'a') + "\r\n\r\n";
        require_error(over, client_error::header_section_too_large);
        // a line that never ends
        require_error(head + std::string(response_factory::max_header_section_size, 'a'), client_error::header_section_too_large);
    }
}

TEST_CASE("Response factory framing headers", "[response_factory][unit]") {
    SECTION("Content-Length together with Transfer-Encoding") {
        require_error(response_with("Content-Length: 3\r\nTransfer-Encoding: chunked\r\n", "3\r\nabc\r\n0\r\n\r\n"), client_error::invalid_framing);
        require_error(response_with("Transfer-Encoding: chunked\r\nContent-Length: 3\r\n", "3\r\nabc\r\n0\r\n\r\n"), client_error::invalid_framing);
    }
    SECTION("Transfer-Encoding other than a single chunked") {
        for (std::string te : {"gzip, chunked", "chunked, chunked", "chunked, gzip", "gzip", "identity", "chunked;q=1", "", "chunk"}) {
            require_error(response_with("Transfer-Encoding: " + te + "\r\n", "0\r\n\r\n"), client_error::invalid_framing);
        }
        require_error(response_with("Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n", "0\r\n\r\n"), client_error::invalid_framing);
    }
    SECTION("Transfer-Encoding in an HTTP/1.0 response") {
        require_error("HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", client_error::invalid_framing);
    }
    SECTION("Invalid Content-Length") {
        for (std::string length : {"5abc", "-1", "+5", "", "5, 5", "5 5", "0x5", "99999999999999999999999"}) {
            require_error(response_with("Content-Length: " + length + "\r\n", "hello"), client_error::invalid_framing);
        }
        require_error(response_with("Content-Length: 5\r\nContent-Length: 6\r\n", "hello!"), client_error::invalid_framing);
    }
    SECTION("Framing is checked for responses without a body too") {
        require_error(response_with("Content-Length: 3\r\nTransfer-Encoding: chunked\r\n"), client_error::invalid_framing, method::HEAD);
        require_error("HTTP/1.1 304 Not Modified\r\nContent-Length: -1\r\n\r\n", client_error::invalid_framing);
        require_error("HTTP/1.1 100 Continue\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n", client_error::invalid_framing);
    }
    SECTION("Valid forms") {
        REQUIRE(require_complete(response_with("Transfer-Encoding: Chunked\r\n", "0\r\n\r\n")).framing == body_framing::chunked);
        REQUIRE(require_complete(response_with("Transfer-Encoding:  chunked \r\n", "0\r\n\r\n")).framing == body_framing::chunked);
        auto repeated = require_complete(response_with("Content-Length: 5\r\nContent-Length: 5\r\n", "hello"));
        REQUIRE(repeated.response->get_content() == "hello");
        REQUIRE(require_complete(response_with("Content-Length: 005\r\n", "hello")).response->get_content() == "hello");
    }
}

TEST_CASE("Response factory bodies", "[response_factory][unit]") {
    SECTION("Content-Length: what follows is left for the next response") {
        auto r = require_complete(response_with("Content-Length: 5\r\n", "helloHTTP/1.1 200 OK\r\n"));
        REQUIRE(r.response->get_content() == "hello");
        REQUIRE(r.framing == body_framing::content_length);
        REQUIRE(r.consumed == response_with("Content-Length: 5\r\n", "hello").size());
        REQUIRE(r.keep_alive);
    }
    SECTION("Chunked, with extensions and trailers") {
        std::string body = "5;name=value\r\nhello\r\n6\r\n world\r\n0\r\nTrailer: x\r\n\r\n";
        auto r = require_complete(response_with("Transfer-Encoding: chunked\r\n", body + "next"));
        REQUIRE(r.response->get_content() == "hello world");
        REQUIRE(r.consumed == response_with("Transfer-Encoding: chunked\r\n", body).size());
        REQUIRE_FALSE(r.response->has_header("Trailer"));
        REQUIRE(r.keep_alive);
    }
    SECTION("Malformed chunked bodies") {
        for (std::string body : {"5\r\nhello0\r\n\r\n", "0x5\r\nhello\r\n0\r\n\r\n", "fffffffffffffffff\r\n", "5 \r\nhello\r\n0\r\n\r\n",
                                 "-5\r\nhello\r\n0\r\n\r\n", "5\nhello\r\n0\r\n\r\n", "\r\n", "5\r\nhello\r\n0\r\n Folded: x\r\n\r\n",
                                 "5;\r\nhello\r\n0\r\n\r\n", "5\r\nhello\r\n0\r\n\r\r"}) {
            require_error(response_with("Transfer-Encoding: chunked\r\n", body), client_error::malformed_body);
        }
    }
    SECTION("Neither Content-Length nor Transfer-Encoding: the body ends with the connection") {
        std::string raw = response_with("", "all of this, Content-Length: 5\r\n\r\nHTTP/1.1 200 OK\r\n\r\n");
        response_factory parser;
        REQUIRE(boost::indeterminate(parser.parse(raw.begin(), raw.end())));
        REQUIRE(parser.get_body_framing() == body_framing::until_close);
        REQUIRE(parser.end_of_stream());
        REQUIRE_FALSE(parser.keep_alive());
        REQUIRE(parser.consume_response()->get_content() == "all of this, Content-Length: 5\r\n\r\nHTTP/1.1 200 OK\r\n\r\n");
    }
    SECTION("The connection ends before the response does") {
        require_error(response_with("Content-Length: 5\r\n", "hell"), client_error::incomplete_response);
        require_error(response_with("Transfer-Encoding: chunked\r\n", "5\r\nhello\r\n"), client_error::incomplete_response);
        require_error("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n", client_error::incomplete_response);
        require_error("", client_error::incomplete_response);
    }
    SECTION("No body for responses to HEAD, whatever their headers") {
        for (std::string headers : {"Content-Length: 5\r\n", "Transfer-Encoding: chunked\r\n", ""}) {
            auto r = require_complete(response_with(headers, "hello"), method::HEAD);
            REQUIRE(r.response->get_content().empty());
            REQUIRE(r.consumed == response_with(headers).size());
            REQUIRE(r.keep_alive);
        }
    }
    SECTION("No body for 204 and 304") {
        for (std::string status : {"204 No Content", "304 Not Modified"}) {
            auto r = require_complete("HTTP/1.1 " + status + "\r\nContent-Length: 5\r\n\r\nhello");
            REQUIRE(r.response->get_content().empty());
            REQUIRE(r.keep_alive);
        }
        REQUIRE(require_complete("HTTP/1.1 204 No Content\r\n\r\n").keep_alive);
    }
    SECTION("A 2xx to CONNECT starts a tunnel") {
        auto r = require_complete("HTTP/1.1 200 Connection Established\r\nContent-Length: 5\r\n\r\ntunnel", method::CONNECT);
        REQUIRE(r.response->get_content().empty());
        REQUIRE_FALSE(r.keep_alive);
    }
}

TEST_CASE("Response factory interim responses", "[response_factory][unit]") {
    SECTION("100 Continue and 103 Early Hints are skipped") {
        auto r = require_complete("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </a.css>; rel=preload\r\n\r\n" +
                                  response_with("Content-Length: 5\r\n", "hello"));
        REQUIRE(r.response->get_status_code() == 200);
        REQUIRE(r.response->get_content() == "hello");
        REQUIRE_FALSE(r.response->has_header("Link"));
    }
    SECTION("Interim responses have no body, whatever their headers") {
        auto r = require_complete("HTTP/1.1 102 Processing\r\nContent-Length: 5\r\n\r\n" + response_with("Content-Length: 2\r\n", "ok"));
        REQUIRE(r.response->get_content() == "ok");
    }
    SECTION("Each one has its own header section limits") {
        std::string interim;
        for (size_t i = 0; i < 3; i++) {
            interim += "HTTP/1.1 103 Early Hints\r\n";
            for (size_t line = 0; line < response_factory::max_header_lines; line++) interim += "Link: </a.css>\r\n";
            interim += "\r\n";
        }
        REQUIRE(require_complete(interim + response_with("Content-Length: 0\r\n")).response->get_status_code() == 200);
    }
    SECTION("Returned on their own in headers-only mode") {
        response_factory parser;
        parser.set_headers_only(true);
        std::string raw = "HTTP/1.1 100 Continue\r\n\r\n" + response_with("Content-Length: 2\r\n", "ok");
        const char* begin = raw.data();
        REQUIRE(bool(parser.parse_some(begin, raw.data() + raw.size())));
        REQUIRE(parser.consume_response()->get_status_code() == 100);
        REQUIRE(begin == raw.data() + 25);
    }
    SECTION("101 Switching Protocols is final: what follows belongs to the new protocol") {
        std::string head = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
        auto r = require_complete(head + "\x81\x02hi");
        REQUIRE(r.response->get_status_code() == 101);
        REQUIRE(r.consumed == head.size());
        REQUIRE_FALSE(r.keep_alive);
    }
}

TEST_CASE("Response factory keep-alive", "[response_factory][unit]") {
    REQUIRE(require_complete(response_with("Content-Length: 0\r\n")).keep_alive);
    REQUIRE_FALSE(require_complete(response_with("Connection: close\r\nContent-Length: 0\r\n")).keep_alive);
    REQUIRE_FALSE(require_complete("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n").keep_alive);
    REQUIRE(require_complete("HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n").keep_alive);
    REQUIRE_FALSE(require_complete(response_with(""), method::GET, true).keep_alive);
}

TEST_CASE("Response factory size limit of buffered bodies", "[response_factory][unit]") {
    response_factory parser;
    parser.set_max_content_size(4);

    SECTION("Content-Length over it fails before reading the body") {
        std::string raw = response_with("Content-Length: 5\r\n");
        REQUIRE(bool(!parser.parse(raw.begin(), raw.end())));
        REQUIRE(parser.error() == client_error::body_too_large);
    }
    SECTION("Chunked") {
        auto r = parse_response(response_with("Transfer-Encoding: chunked\r\n", "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n"), method::GET, false,
                                std::string::npos, &parser);
        REQUIRE(bool(!r.result));
        REQUIRE(r.error == client_error::body_too_large);
    }
    SECTION("Until the connection ends") {
        auto r = parse_response(response_with("", "abcde"), method::GET, true, std::string::npos, &parser);
        REQUIRE(bool(!r.result));
        REQUIRE(r.error == client_error::body_too_large);
    }
    SECTION("Within it") {
        auto r = parse_response(response_with("Transfer-Encoding: chunked\r\n", "2\r\nab\r\n2\r\ncd\r\n0\r\n\r\n"), method::GET, false,
                                std::string::npos, &parser);
        REQUIRE(bool(r.result));
        REQUIRE(r.response->get_content() == "abcd");
    }
}

TEST_CASE("Response factory streaming", "[response_factory][unit]") {
    response_factory parser;
    std::string streamed;
    std::vector<size_t> totals;
    bool proceed = true;
    parser.setOnStreaming([&](const std::string_view& data, size_t downloaded, size_t total) {
        streamed.append(data);
        REQUIRE(downloaded == streamed.size());
        totals.push_back(total);
        return proceed;
    });

    SECTION("Content-Length, with its total") {
        auto r = parse_response(response_with("Content-Length: 11\r\n", "hello world"), method::GET, false, 3, &parser);
        REQUIRE(bool(r.result));
        REQUIRE(streamed == "hello world");
        REQUIRE(r.response->get_content().empty());
        REQUIRE(totals.back() == 11);
    }
    SECTION("Chunked, without total") {
        auto r = parse_response(response_with("Transfer-Encoding: chunked\r\n", "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"), method::GET,
                                false, 4, &parser);
        REQUIRE(bool(r.result));
        REQUIRE(streamed == "hello world");
        REQUIRE(totals.back() == 0);
    }
    SECTION("Not limited by the maximum content size") {
        parser.set_max_content_size(2);
        auto r = parse_response(response_with("Content-Length: 11\r\n", "hello world"), method::GET, false, std::string::npos, &parser);
        REQUIRE(bool(r.result));
        REQUIRE(streamed == "hello world");
    }
    SECTION("Aborted by the callback") {
        proceed = false;
        auto r = parse_response(response_with("Content-Length: 11\r\n", "hello world"), method::GET, false, std::string::npos, &parser);
        REQUIRE(bool(!r.result));
        REQUIRE(r.error == client_error::aborted);
    }
}

TEST_CASE("Response factory chunk callback", "[response_factory][unit]") {
    response_factory parser;
    std::vector<std::pair<int, std::string>> calls;
    parser.setOnChunked([&](int event, const std::string& data) { calls.emplace_back(event, data); });
    auto r = parse_response(response_with("Transfer-Encoding: chunked\r\n", "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"), method::GET, false,
                            2, &parser);
    REQUIRE(bool(r.result));
    // each chunk whole, however the input was split
    std::vector<std::pair<int, std::string>> expected{{0, ""}, {1, "hello"}, {1, " world"}, {2, ""}};
    REQUIRE(calls == expected);
}

TEST_CASE("Response factory reset", "[response_factory][unit]") {
    response_factory parser;
    std::string bad = "HTTP/1.1 2000 OK\r\n\r\n";
    REQUIRE(bool(!parser.parse(bad.begin(), bad.end())));
    parser.reset();
    REQUIRE(parser.error() == client_error::none);
    std::string good = response_with("Content-Length: 2\r\n", "ok");
    REQUIRE(bool(parser.parse(good.begin(), good.end())));
    REQUIRE(parser.consume_response()->get_content() == "ok");
}

TEST_CASE("Client errors have a category and a message", "[response_factory][unit]") {
    boost::system::error_code ec = client_error::invalid_framing;
    REQUIRE(ec.category() == client_error_category());
    REQUIRE(ec == client_error::invalid_framing);
    REQUIRE(ec.message().find("framing") != std::string::npos);
    REQUIRE_FALSE(boost::system::error_code(client_error::none));
}

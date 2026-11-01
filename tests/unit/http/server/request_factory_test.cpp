#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/request_factory.hpp>
#include <thinger/http/common/http_request.hpp>
#include <cstring>
#include <chrono>
#include <optional>
#include <string>
#include <utility>

using namespace thinger::http;

// ============================================================================
// Request Factory - headers_only mode
// ============================================================================

TEST_CASE("Request factory headers_only mode returns true at end of headers with Content-Length", "[request_factory][unit]") {
    request_factory parser;
    parser.set_headers_only(true);

    std::string raw =
        "POST /test HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "Hello, World!";

    auto* begin = reinterpret_cast<const uint8_t*>(raw.data());
    auto* end = begin + raw.size();

    // Cast to non-const since parse takes InputIterator& (needs lvalue)
    auto* it = begin;
    boost::tribool result = parser.parse(it, end);

    REQUIRE(bool(result) == true);

    // The body should NOT have been consumed by the parser
    auto req = parser.consume_request();
    REQUIRE(req != nullptr);
    REQUIRE(req->get_content_length() == 13);
    REQUIRE(req->get_body().empty()); // Body not read in headers_only mode

    // Iterator should point to first byte after headers (start of body)
    size_t consumed = static_cast<size_t>(it - begin);
    size_t remaining = raw.size() - consumed;
    REQUIRE(remaining == 13); // "Hello, World!"
    REQUIRE(std::string(reinterpret_cast<const char*>(it), remaining) == "Hello, World!");
}

TEST_CASE("Request factory headers_only mode with no Content-Length", "[request_factory][unit]") {
    request_factory parser;
    parser.set_headers_only(true);

    std::string raw =
        "GET /test HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    auto* begin = reinterpret_cast<const uint8_t*>(raw.data());
    auto* end = begin + raw.size();
    auto* it = begin;

    boost::tribool result = parser.parse(it, end);

    REQUIRE(bool(result) == true);

    auto req = parser.consume_request();
    REQUIRE(req != nullptr);
    REQUIRE(req->get_content_length() == 0);
    REQUIRE(it == end); // No remaining data
}

// ============================================================================
// Request Factory - Iterator position tracking
// ============================================================================

TEST_CASE("Request factory iterator tracks consumed bytes correctly", "[request_factory][unit]") {
    request_factory parser;
    // Default mode (not headers_only) - with no body
    std::string raw =
        "GET /hello HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n"
        "NEXT";

    auto* begin = reinterpret_cast<const uint8_t*>(raw.data());
    auto* end = begin + raw.size();
    auto* it = begin;

    boost::tribool result = parser.parse(it, end);
    REQUIRE(bool(result) == true);

    size_t remaining = static_cast<size_t>(end - it);
    REQUIRE(remaining == 4); // "NEXT"
    REQUIRE(std::string(reinterpret_cast<const char*>(it), remaining) == "NEXT");
}

TEST_CASE("Request factory returns indeterminate on partial headers", "[request_factory][unit]") {
    request_factory parser;

    std::string partial = "GET /hello HTTP/1.1\r\n"
                          "Host: local";

    auto* begin = reinterpret_cast<const uint8_t*>(partial.data());
    auto* end = begin + partial.size();
    auto* it = begin;

    boost::tribool result = parser.parse(it, end);
    REQUIRE(boost::indeterminate(result));
    REQUIRE(it == end); // All data consumed

    // Feed the rest
    std::string rest = "host\r\n\r\n";
    auto* begin2 = reinterpret_cast<const uint8_t*>(rest.data());
    auto* end2 = begin2 + rest.size();
    auto* it2 = begin2;

    result = parser.parse(it2, end2);
    REQUIRE(bool(result) == true);

    auto req = parser.consume_request();
    REQUIRE(req != nullptr);
    REQUIRE(req->get_uri() == "/hello");
}

// ============================================================================
// Request Factory - headers_only getter/setter
// ============================================================================

TEST_CASE("Request factory headers_only defaults to false", "[request_factory][unit]") {
    request_factory parser;
    REQUIRE(parser.get_headers_only() == false);
}

TEST_CASE("Request factory headers_only can be toggled", "[request_factory][unit]") {
    request_factory parser;
    parser.set_headers_only(true);
    REQUIRE(parser.get_headers_only() == true);
    parser.set_headers_only(false);
    REQUIRE(parser.get_headers_only() == false);
}

TEST_CASE("Request factory rejects folded header lines (obs-fold)", "[request_factory][unit]") {
    auto parse = [](const std::string& raw) {
        request_factory parser;
        parser.set_headers_only(true);
        auto* it = reinterpret_cast<const uint8_t*>(raw.data());
        auto* end = it + raw.size();
        return parser.parse(it, end);
    };

    REQUIRE(bool(parse("GET / HTTP/1.1\r\nHost: localhost\r\nX-A: 1\r\n\r\n")));
    REQUIRE(bool(!parse("GET / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encodin: chunked\r\n g\r\n\r\n")));
    REQUIRE(bool(!parse("GET / HTTP/1.1\r\nHost: localhost\r\nX-A: 1\r\n\t2\r\n\r\n")));
    REQUIRE(bool(!parse("GET / HTTP/1.1\r\n Host: localhost\r\n\r\n")));
}

namespace {
    boost::tribool parse_headers(request_factory& parser, const std::string& raw) {
        parser.set_headers_only(true);
        auto* it = reinterpret_cast<const uint8_t*>(raw.data());
        auto* end = it + raw.size();
        return parser.parse(it, end);
    }

    std::string request_with_headers(int count) {
        std::string raw = "GET / HTTP/1.1\r\nHost: localhost\r\n";
        for (int i = 1; i < count; i++) raw += "X-Header-" + std::to_string(i) + ": value\r\n";
        return raw + "\r\n";
    }

    // Request line and headers taking exactly `size` bytes
    std::string request_of_size(size_t size) {
        std::string head = "GET / HTTP/1.1\r\nHost: localhost\r\nX-Big: ";
        std::string tail = "\r\n\r\n";
        return head + std::string(size - head.size() - tail.size(), 'a') + tail;
    }
}

TEST_CASE("Request factory limits the number of header lines", "[request_factory][unit]") {
    request_factory accepted;
    REQUIRE(bool(parse_headers(accepted, request_with_headers(100))));

    request_factory rejected;
    REQUIRE(bool(!parse_headers(rejected, request_with_headers(101))));
}

TEST_CASE("Request factory limits the size of the header section", "[request_factory][unit]") {
    request_factory accepted;
    REQUIRE(bool(parse_headers(accepted, request_of_size(16 * 1024))));

    request_factory rejected;
    REQUIRE(bool(!parse_headers(rejected, request_of_size(16 * 1024 + 1))));
}

TEST_CASE("Request factory rejects many repeated Content-Length headers quickly", "[request_factory][unit]") {
    std::string raw = "POST / HTTP/1.1\r\nHost: localhost\r\n";
    for (int i = 0; i < 20000; i++) raw += "Content-Length: 5\r\n";
    raw += "\r\nhello";

    request_factory parser;
    auto start = std::chrono::steady_clock::now();
    auto result = parse_headers(parser, raw);
    auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(bool(!result));
    REQUIRE(elapsed < std::chrono::seconds(2));
}

// Found by differential fuzzing against Boost.Beast (tests/fuzz/fuzz_http_differential.cpp):
// multi-digit versions were accepted and truncated to 8 bits ("HTTP/1.257" read as 1.1)
TEST_CASE("Request factory accepts only HTTP/1.x with single-digit versions", "[request_factory][unit]") {
    auto version_of = [](const std::string& version) -> std::optional<std::pair<int, int>> {
        request_factory parser;
        auto result = parse_headers(parser, "GET / HTTP/" + version + "\r\nHost: localhost\r\n\r\n");
        if (!result) return std::nullopt;
        REQUIRE(bool(result));
        auto request = parser.consume_request();
        return std::make_pair(request->get_http_version_major(), request->get_http_version_minor());
    };

    REQUIRE(version_of("1.1") == std::make_pair(1, 1));
    REQUIRE(version_of("1.0") == std::make_pair(1, 0));
    // A higher minor version is handled as the highest one supported (RFC 9110, section 2.5)
    REQUIRE(version_of("1.2") == std::make_pair(1, 2));

    for (const char* version : {"1.10", "1.01", "1.257", "01.1", "11.1", "257.1", "2.0", "0.9", "9.9", "1.", ".1", "1"}) {
        INFO("version: " << version);
        REQUIRE_FALSE(version_of(version).has_value());
    }
}

// Found by differential fuzzing against llhttp (tests/fuzz/fuzz_http_differential3.cpp): bytes
// above 0x7F were accepted in the request target, which is ASCII (RFC 9112 section 3.2)
TEST_CASE("Request factory accepts only visible ASCII in the request target", "[request_factory][unit]") {
    auto accepts = [](const std::string& target) {
        request_factory parser;
        return bool(parse_headers(parser, "GET " + target + " HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    };

    REQUIRE(accepts("/a"));
    REQUIRE(accepts("/%C3%A9?q=%FF#f"));
    for (const char* target : {"/\xc3\xa9", "/\xff", "/a?b=\x80", "/a#\xa0", "/\x7f", "/\x01"}) {
        INFO("target: " << target);
        REQUIRE_FALSE(accepts(target));
    }
}

// Found by differential fuzzing against llhttp: CONNECT (not served) was parsed as any other
// request, though what follows its header section is tunnel data (RFC 9110 section 9.3.6)
TEST_CASE("Request factory rejects CONNECT", "[request_factory][unit]") {
    for (const char* request : {"CONNECT localhost:443 HTTP/1.1\r\nHost: localhost:443\r\n\r\n",
                                "CONNECT /a HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello"}) {
        INFO("request: " << request);
        request_factory parser;
        REQUIRE(bool(!parse_headers(parser, request)));
    }
}

// Found by differential fuzzing against llhttp: the target was checked to start with "/" once
// percent-decoded, so "%2Fa" was accepted (and routed as "/a"), though it is not in origin
// form (RFC 9112 section 3.2.1)
TEST_CASE("Request factory requires a target starting with a slash as sent", "[request_factory][unit]") {
    auto accepts = [](const std::string& target) {
        request_factory parser;
        return bool(parse_headers(parser, "GET " + target + " HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    };

    REQUIRE(accepts("/a%2Fb"));
    for (const char* target : {"%2Fa", "%2fa/b", "%2F", "a/b", "?a=b"}) {
        INFO("target: " << target);
        REQUIRE_FALSE(accepts(target));
    }
}

// ============================================================================
// Request Factory - whitespace around field values
// ============================================================================

namespace {
    using namespace std::string_literals;

    enum class outcome { accepted, rejected, incomplete };

    struct parsed {
        outcome result;
        std::shared_ptr<http_request> request;
    };

    // Parse `raw` fed in two pieces, split at `split`: the bytes before and after it take the
    // fast path (runs of bytes) or consume() at different places
    parsed parse_split(const std::string& raw, size_t split) {
        request_factory parser;
        auto* it = reinterpret_cast<const uint8_t*>(raw.data());
        auto* middle = it + split;
        auto* end = it + raw.size();
        boost::tribool result = parser.parse(it, middle);
        if (boost::indeterminate(result)) {
            it = middle;
            result = parser.parse(it, end);
        }
        if (boost::indeterminate(result)) return {outcome::incomplete, nullptr};
        if (!result) return {outcome::rejected, nullptr};
        return {outcome::accepted, parser.consume_request()};
    }

    // Parse `raw` whole and split at every byte offset, requiring the same outcome and the same
    // headers and body every time. The request if it is accepted, null if rejected
    std::shared_ptr<http_request> parse_at_every_split(const std::string& raw) {
        auto whole = parse_split(raw, raw.size());
        REQUIRE(whole.result != outcome::incomplete);
        for (size_t split = 0; split < raw.size(); split++) {
            INFO("split at byte " << split);
            auto piece = parse_split(raw, split);
            REQUIRE(piece.result == whole.result);
            if (whole.request) {
                REQUIRE(piece.request->get_headers() == whole.request->get_headers());
                REQUIRE(piece.request->get_body() == whole.request->get_body());
            }
        }
        return whole.request;
    }

    // GET request with `field_line` after the Host header
    std::string request_with_field(const std::string& field_line) {
        return "GET / HTTP/1.1\r\nHost: localhost\r\n" + field_line + "\r\n\r\n";
    }

    // Value of X-A once `field_line` is parsed (failing the test if it is rejected)
    std::string value_of(const std::string& field_line) {
        INFO("field line: " << field_line);
        auto request = parse_at_every_split(request_with_field(field_line));
        REQUIRE(request);
        REQUIRE(request->has_header("X-A"));
        return request->get_header("X-A");
    }

    bool rejects(const std::string& request) {
        INFO("request: " << request);
        return parse_at_every_split(request) == nullptr;
    }
}

// RFC 9112 section 5.1: field-line = field-name ":" OWS field-value OWS. llhttp (Node) and Go's
// net/textproto (ReadMIMEHeader trims SP and HTAB on both sides) accept no whitespace, several
// SP or HTAB after the colon
TEST_CASE("Request factory skips any SP and HTAB after the colon", "[request_factory][unit]") {
    // Node: req.headers['x-a'] === 'value'; Go: r.Header.Get("X-A") == "value"
    REQUIRE(value_of("X-A:value") == "value");
    REQUIRE(value_of("X-A: value") == "value");
    REQUIRE(value_of("X-A:\tvalue") == "value");
    REQUIRE(value_of("X-A:  value") == "value");
    REQUIRE(value_of("X-A: \t value") == "value");
}

// Node and Go store an empty value (Node: '', Go: Header["X-A"] == [""]); RFC 9110 section 5.5
// allows an empty field value
TEST_CASE("Request factory stores an empty or whitespace-only value as empty", "[request_factory][unit]") {
    REQUIRE(value_of("X-A:").empty());
    REQUIRE(value_of("X-A: ").empty());
    REQUIRE(value_of("X-A:   ").empty());
    REQUIRE(value_of("X-A:\t").empty());
    REQUIRE(value_of("X-A: \t \t").empty());
}

// Trailing OWS is not part of the value (RFC 9110 section 5.5): Node and Go trim it
TEST_CASE("Request factory trims trailing SP and HTAB of a value", "[request_factory][unit]") {
    REQUIRE(value_of("X-A: value ") == "value");
    REQUIRE(value_of("X-A: value \t") == "value");
    REQUIRE(value_of("X-A: value\t") == "value");
    REQUIRE(value_of("X-A:\tvalue\t\t") == "value");
    REQUIRE(value_of("X-A: a b \t") == "a b");
}

// HTAB is allowed inside a field value (field-vchar [ 1*( SP / HTAB / field-vchar ) field-vchar],
// RFC 9110 section 5.5); Node and Go keep it
TEST_CASE("Request factory keeps HTAB inside a value", "[request_factory][unit]") {
    REQUIRE(value_of("X-A: a\tb") == "a\tb");
    REQUIRE(value_of("X-A:\ta\t \tb\t") == "a\t \tb");
    // obs-text (bytes over 0x7F) stays accepted, as in Node and Go
    REQUIRE(value_of("X-A: caf\xc3\xa9 ") == "caf\xc3\xa9");
}

// Host is trimmed like any other field before the host and port are taken from it
TEST_CASE("Request factory trims the whitespace around Host", "[request_factory][unit]") {
    auto request = parse_at_every_split("GET / HTTP/1.1\r\nHost:\t example.com:8080 \t\r\n\r\n");
    REQUIRE(request);
    REQUIRE(request->get_host() == "example.com");
    REQUIRE(request->get_port() == "8080");
}

// Content-Length and Transfer-Encoding are validated as strictly as before, on the value without
// its OWS: Node and Go accept "Content-Length: 5 " and "Transfer-Encoding:\tchunked"
TEST_CASE("Request factory validates framing fields without their whitespace", "[request_factory][unit]") {
    auto post = [](const std::string& field_line, const std::string& body) {
        return "POST / HTTP/1.1\r\nHost: localhost\r\n" + field_line + "\r\n\r\n" + body;
    };

    SECTION("Content-Length with OWS frames the body") {
        for (const char* field : {"Content-Length: 5 ", "Content-Length:\t5\t", "Content-Length:5", "Content-Length:  5"}) {
            INFO("field: " << field);
            auto request = parse_at_every_split(post(field, "hello"));
            REQUIRE(request);
            REQUIRE(request->has_valid_framing());
            REQUIRE(request->get_content_length() == 5);
            REQUIRE(request->get_body() == "hello");
        }
    }

    // Node and Go reject these too (Go: "bad Content-Length"; llhttp: HPE_INVALID_CONTENT_LENGTH
    // or HPE_UNEXPECTED_CONTENT_LENGTH)
    SECTION("Invalid Content-Length values stay invalid") {
        for (const char* field : {"Content-Length:", "Content-Length: \t", "Content-Length: 5 5", "Content-Length: 5,5",
                                  "Content-Length: 5, 5", "Content-Length: +5", "Content-Length:\t-5"}) {
            INFO("field: " << field);
            auto request = parse_at_every_split(request_with_field(field));
            REQUIRE(request);
            REQUIRE_FALSE(request->has_valid_framing());
        }
    }

    SECTION("Transfer-Encoding with OWS is chunked") {
        for (const char* field : {"Transfer-Encoding: chunked ", "Transfer-Encoding:\tchunked", "Transfer-Encoding:chunked\t"}) {
            INFO("field: " << field);
            auto request = parse_at_every_split(post(field, "5\r\nhello\r\n0\r\n\r\n"));
            REQUIRE(request);
            REQUIRE(request->has_valid_framing());
            REQUIRE(request->is_chunked_transfer());
        }
    }

    // Go answers 501 to "chunked, chunked" (unsupported list) and 400 to an empty one, Node 400
    SECTION("Invalid Transfer-Encoding values stay invalid") {
        for (const char* field : {"Transfer-Encoding:", "Transfer-Encoding: \t ", "Transfer-Encoding: chunked, chunked",
                                  "Transfer-Encoding: chunked\tchunked"}) {
            INFO("field: " << field);
            auto request = parse_at_every_split(request_with_field(field));
            REQUIRE(request);
            REQUIRE_FALSE(request->has_valid_framing());
        }
    }
}

// RFC 9112 section 5.1: no whitespace is allowed between the field name and the colon, and a
// server MUST reject it with 400 (it was used for request smuggling). llhttp rejects it
// (HPE_INVALID_HEADER_TOKEN), Go too ("malformed MIME header line")
TEST_CASE("Request factory rejects whitespace before the colon", "[request_factory][unit]") {
    for (const char* field : {"Content-Length : 5", "Content-Length\t: 5", "X-A : b", "X-A\t: b", "X-A \t: b"}) {
        REQUIRE(rejects(request_with_field(field)));
    }
}

// obs-fold (RFC 9112 section 5.2) and control characters other than HTAB in a value: llhttp
// rejects them (HPE_INVALID_HEADER_TOKEN, HPE_UNEXPECTED_SPACE, HPE_LF_EXPECTED), Go too
// (httpguts.ValidHeaderFieldValue, "malformed MIME header")
TEST_CASE("Request factory rejects folding and control characters in values", "[request_factory][unit]") {
    REQUIRE(rejects("GET / HTTP/1.1\r\nHost: localhost\r\nX-A: a\r\n b\r\n\r\n"));
    REQUIRE(rejects("GET / HTTP/1.1\r\nHost: localhost\r\nX-A: a\r\n\tb\r\n\r\n"));
    REQUIRE(rejects("GET / HTTP/1.1\r\nHost: localhost\r\nX-A:\r\n b\r\n\r\n"));
    REQUIRE(rejects("GET / HTTP/1.1\r\n Host: localhost\r\n\r\n"));
    REQUIRE(rejects("GET / HTTP/1.1\r\n\tHost: localhost\r\n\r\n"));
    for (const std::string& field : {"X-A: a\0b"s, "X-A: \0"s, "X-A:\0"s, "X-A: a\rb"s, "X-A: a\nb"s, "X-A: \nb"s,
                                     "X-A: a\x7f"s, "X-A:\x7f"s, "X-A: a\x01"s, "X-A: \x0b"s, "X-A: a\x1f b"s}) {
        REQUIRE(rejects(request_with_field(field)));
    }
}

// ============================================================================
// Request Factory - ".." segments and "+" in the target
// ============================================================================

namespace {
    std::shared_ptr<http_request> parse_target(const std::string& target) {
        INFO("target: " << target);
        return parse_at_every_split("GET " + target + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
    }
}

// Only a ".." segment of the path is a traversal (RFC 3986 section 3.3: dot-segments are whole
// segments). Node (llhttp) accepts every target below and leaves it to the application; Go's
// ServeMux cleans only "." and ".." segments (path.Clean), so "1..5" or "v1..v2.txt" are plain
// names for both. The query is never a path
TEST_CASE("Request factory accepts \"..\" that is not a path segment", "[request_factory][unit]") {
    for (const char* target : {"/api/items?range=1..5", "/files/v1..v2.txt", "/a/...", "/..a", "/a..", "/?q=a%2E%2Eb",
                               "/x?../..", "/a/./b", "/a%3F/..b", "/...", "/a/.../b"}) {
        auto request = parse_target(target);
        REQUIRE(request);
        REQUIRE(request->get_uri() == target);
    }
    auto request = parse_target("/api/items?range=1..5");
    REQUIRE(request->get_path() == "/api/items");
    REQUIRE(request->get_uri_parameters() == std::multimap<std::string, std::string>{{"range", "1..5"}});
    REQUIRE(parse_target("/files/v1..v2.txt")->get_resource() == "/files/v1..v2.txt");
}

// A ".." segment of the percent-decoded path, which ends at the first "?" as sent (an encoded
// "%3F" belongs to the path): rejected with 400, as in 0.9.0 (Go's ServeMux would redirect to
// the cleaned path; Node does not check it)
TEST_CASE("Request factory rejects \"..\" path segments", "[request_factory][unit]") {
    for (const char* target : {"/..", "/../x", "/a/../b", "/a/..", "/a/..?x=1", "/%2E%2E/x", "/.%2e/x", "/a%2F..%2Fb",
                               "/a/%2e%2E", "/a/..%3Fx=1/..", "/..?", "//.."}) {
        REQUIRE_FALSE(parse_target(target));
    }
    // "/a/..%3F" decodes to "/a/..?", whose last segment is "..?", not ".."
    REQUIRE(parse_target("/a/..%3F"));
}

// Invalid percent-encoding stays rejected, in the path and in the query, as in 0.9.0
TEST_CASE("Request factory rejects invalid percent-encoding in the target", "[request_factory][unit]") {
    for (const char* target : {"/a%2", "/a%", "/a%zz", "/a?k=%zz", "/a?k=%", "/a?k=%4", "/a?%%41", "/a%2Fb?c=%g1"}) {
        REQUIRE_FALSE(parse_target(target));
    }
    REQUIRE(parse_target("/a%2Fb?c=%41%62&d=%7e"));
}

// "+" is literal in a path (RFC 3986; Node's URL.pathname and Go's URL.Path keep it): it is a
// space only in the query, as application/x-www-form-urlencoded (Node's URLSearchParams and
// Go's URL.Query() decode it)
TEST_CASE("Request factory keeps \"+\" literal in the path", "[request_factory][unit]") {
    auto plus = parse_target("/a+b");
    REQUIRE(plus);
    REQUIRE(plus->get_path() == "/a+b");
    REQUIRE(plus->get_resource() == "/a+b");

    auto encoded = parse_target("/a%2Bb");
    REQUIRE(encoded);
    REQUIRE(encoded->get_resource() == "/a+b");

    auto query = parse_target("/x?q=a+b");
    REQUIRE(query);
    REQUIRE(query->get_resource() == "/x");
    REQUIRE(query->get_uri_parameters() == std::multimap<std::string, std::string>{{"q", "a b"}});
}

// ============================================================================
// Request Factory - Host fields
// ============================================================================

// RFC 9110 section 7.2: a client MUST send Host in HTTP/1.1 requests, and a server MUST answer
// 400 to an HTTP/1.1 request without it. Node (requireHostHeader, true by default) and Go
// ("missing required Host header") reject it; both accept an HTTP/1.0 request without Host
TEST_CASE("Request factory requires Host in HTTP/1.1 requests", "[request_factory][unit]") {
    REQUIRE(rejects("GET / HTTP/1.1\r\n\r\n"));
    REQUIRE(rejects("GET / HTTP/1.1\r\nX-A: b\r\n\r\n"));
    REQUIRE(rejects("POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello"));
    // a higher minor version is handled as 1.1 (RFC 9110 section 2.5)
    REQUIRE(rejects("GET / HTTP/1.2\r\n\r\n"));
    // a field whose name only starts or ends like Host is not one
    REQUIRE(rejects("GET / HTTP/1.1\r\nHosts: localhost\r\n\r\n"));
    REQUIRE(rejects("GET / HTTP/1.1\r\nX-Host: localhost\r\n\r\n"));

    auto http_1_0 = parse_at_every_split("GET / HTTP/1.0\r\n\r\n");
    REQUIRE(http_1_0);
    REQUIRE_FALSE(http_1_0->has_header("Host"));
    REQUIRE(parse_at_every_split("POST / HTTP/1.0\r\nContent-Length: 5\r\n\r\nhello"));
}

// Node and Go check that Host is present, not its value; RFC 9110 section 7.2 allows an empty
// one (a target URI without an authority)
TEST_CASE("Request factory accepts an empty Host", "[request_factory][unit]") {
    for (const char* field : {"Host:", "Host: ", "Host:\t \t"}) {
        INFO("field: " << field);
        auto request = parse_at_every_split("GET / HTTP/1.1\r\n" + std::string(field) + "\r\n\r\n");
        REQUIRE(request);
        REQUIRE(request->has_header("Host"));
        REQUIRE(request->get_host().empty());
    }
}

// Any letter case is the Host field (field names are case-insensitive, RFC 9110 section 5.1)
TEST_CASE("Request factory takes Host in any letter case", "[request_factory][unit]") {
    for (const char* name : {"host", "HOST", "hOsT"}) {
        INFO("name: " << name);
        auto request = parse_at_every_split("GET / HTTP/1.1\r\n" + std::string(name) + ": example.com\r\n\r\n");
        REQUIRE(request);
        REQUIRE(request->get_host() == "example.com");
    }
}

// RFC 9110 section 7.2 (a request with more than one Host field is invalid, 400). Go answers 400
// ("too many Host headers"); Node keeps the first, so the RFC decides. Rejected whatever the
// values and the letter case, also in HTTP/1.0
TEST_CASE("Request factory rejects more than one Host field", "[request_factory][unit]") {
    for (const char* fields : {"Host: a.example\r\nHost: b.example", "Host: a.example\r\nHost: a.example",
                               "Host: a.example\r\nhost: a.example", "HOST: a\r\nX-A: b\r\nhost: b", "Host:\r\nHost:",
                               "Host: a\r\nHost: a\r\nHost: a"}) {
        REQUIRE(rejects("GET / HTTP/1.1\r\n" + std::string(fields) + "\r\n\r\n"));
        REQUIRE(rejects("GET / HTTP/1.0\r\n" + std::string(fields) + "\r\n\r\n"));
    }
}

// The checks are per request: the next request on the same parser starts without a Host
TEST_CASE("Request factory checks Host again on the next request", "[request_factory][unit]") {
    request_factory parser;
    std::string first = "GET /a HTTP/1.1\r\nHost: localhost\r\n\r\n";
    REQUIRE(bool(parse_headers(parser, first)));
    parser.consume_request();
    // a second Host field across requests is not a repeated one
    REQUIRE(bool(parse_headers(parser, first)));
    parser.consume_request();
    REQUIRE(bool(!parse_headers(parser, "GET /b HTTP/1.1\r\n\r\n")));
}

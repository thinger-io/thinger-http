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
    REQUIRE(bool(!parse("GET / HTTP/1.1\r\nTransfer-Encodin: chunked\r\n g\r\n\r\n")));
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
        std::string head = "GET / HTTP/1.1\r\nX-Big: ";
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

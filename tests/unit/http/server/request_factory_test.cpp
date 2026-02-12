#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/request_factory.hpp>
#include <thinger/http/common/http_request.hpp>
#include <cstring>

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

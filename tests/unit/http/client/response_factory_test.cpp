#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/response_factory.hpp>
#include <thinger/http/common/http_response.hpp>
#include <string>

using namespace thinger::http;

namespace {
    boost::tribool parse_response(response_factory& parser, const std::string& raw) {
        return parser.parse(raw.begin(), raw.end());
    }
}

TEST_CASE("Response factory parses a response with headers", "[response_factory][unit]") {
    response_factory parser;
    auto result = parse_response(parser, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-A: 1\r\n\r\nhello");
    REQUIRE(bool(result));
    auto response = parser.consume_response();
    REQUIRE(response->get_content_length() == 5);
    REQUIRE(response->get_header("X-A") == "1");
}

TEST_CASE("Response factory rejects folded header lines (obs-fold)", "[response_factory][unit]") {
    // A line starting with whitespace must never be merged into the previous header name,
    // which turned "Content-Lengt: 5\r\n h" into "Content-Length: 5"
    SECTION("Folding completes Content-Length") {
        response_factory parser;
        REQUIRE(!parse_response(parser, "HTTP/1.1 200 OK\r\nContent-Lengt: 5\r\n h\r\n\r\nhello"));
    }
    SECTION("Folding a value with a tab") {
        response_factory parser;
        REQUIRE(!parse_response(parser, "HTTP/1.1 200 OK\r\nX-A: 1\r\n\t2\r\nContent-Length: 0\r\n\r\n"));
    }
}

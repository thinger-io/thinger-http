#include <catch2/catch_test_macros.hpp>
#include <thinger/http/util/http_date.hpp>

using namespace thinger::http::util;

// The example of RFC 9110, 5.6.7
static constexpr std::time_t example_time = 784111777;

TEST_CASE("format_http_date", "[http_date][unit]") {

    SECTION("IMF-fixdate in GMT") {
        REQUIRE(format_http_date(example_time) == "Sun, 06 Nov 1994 08:49:37 GMT");
        REQUIRE(format_http_date(0) == "Thu, 01 Jan 1970 00:00:00 GMT");
        REQUIRE(format_http_date(951782400) == "Tue, 29 Feb 2000 00:00:00 GMT");
        REQUIRE(format_http_date(1798761599) == "Thu, 31 Dec 2026 23:59:59 GMT");
    }

    SECTION("Parsed back to the same time") {
        for (std::time_t time : {std::time_t{0}, example_time, std::time_t{951782400}, std::time_t{1798761599}}) {
            REQUIRE(parse_http_date(format_http_date(time)) == time);
        }
    }
}

TEST_CASE("parse_http_date", "[http_date][unit]") {

    SECTION("IMF-fixdate") {
        REQUIRE(parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT") == example_time);
    }

    SECTION("Obsolete RFC 850 format") {
        REQUIRE(parse_http_date("Sunday, 06-Nov-94 08:49:37 GMT") == example_time);
    }

    SECTION("Obsolete asctime format") {
        REQUIRE(parse_http_date("Sun Nov  6 08:49:37 1994") == example_time);
        REQUIRE(parse_http_date("Wed Nov 16 08:49:37 1994") == example_time + 10 * 86400);
    }

    SECTION("Leap second") {
        REQUIRE(parse_http_date("Sat, 31 Dec 2016 23:59:60 GMT") == parse_http_date("Sun, 01 Jan 2017 00:00:00 GMT"));
    }

    SECTION("Invalid dates") {
        REQUIRE_FALSE(parse_http_date(""));
        REQUIRE_FALSE(parse_http_date("yesterday"));
        REQUIRE_FALSE(parse_http_date("784111777"));
        REQUIRE_FALSE(parse_http_date("Sun, 06 Nov 1994 08:49:37 UTC"));
        REQUIRE_FALSE(parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT "));
        REQUIRE_FALSE(parse_http_date(" Sun, 06 Nov 1994 08:49:37 GMT"));
        REQUIRE_FALSE(parse_http_date("Sun, 6 Nov 1994 08:49:37 GMT"));
        REQUIRE_FALSE(parse_http_date("sun, 06 nov 1994 08:49:37 GMT"));
        REQUIRE_FALSE(parse_http_date("Sun, 06 Nov 94 08:49:37 GMT"));
        REQUIRE_FALSE(parse_http_date("Sun, 31 Nov 1994 08:49:37 GMT"));
        REQUIRE_FALSE(parse_http_date("Sun, 29 Feb 1994 08:49:37 GMT"));
        REQUIRE_FALSE(parse_http_date("Sun, 06 Nov 1994 24:00:00 GMT"));
        REQUIRE_FALSE(parse_http_date("Sun, 06 Nov 1994 08:60:00 GMT"));
        REQUIRE_FALSE(parse_http_date("Sun, 06 Nov 1994 08:49:61 GMT"));
        REQUIRE_FALSE(parse_http_date("Sun, 06 Nov 1994 08:49 GMT"));
        REQUIRE_FALSE(parse_http_date("Sun, 06 Nov 1994 08:49:37"));
        REQUIRE_FALSE(parse_http_date("Sunday, 06-Nov-1994 08:49:37 GMT"));
        REQUIRE_FALSE(parse_http_date("Sun Nov 6 08:49:37 1994"));
    }
}

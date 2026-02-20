#include <catch2/catch_test_macros.hpp>
#include <thinger/http/util/url.hpp>

using namespace thinger::http::util::url;

// ============================================================================
// url_encode
// ============================================================================

TEST_CASE("url_encode", "[url][unit]") {

    SECTION("Alphanumeric characters are not encoded") {
        REQUIRE(url_encode("abc123") == "abc123");
        REQUIRE(url_encode("ABCxyz") == "ABCxyz");
    }

    SECTION("Unreserved characters are not encoded") {
        REQUIRE(url_encode("-_.~") == "-_.~");
    }

    SECTION("Spaces are percent-encoded") {
        REQUIRE(url_encode("hello world") == "hello%20world");
    }

    SECTION("Special characters are percent-encoded") {
        REQUIRE(url_encode("a=b&c=d") == "a%3Db%26c%3Dd");
        REQUIRE(url_encode("foo@bar") == "foo%40bar");
        REQUIRE(url_encode("100%") == "100%25");
    }

    SECTION("Slash is encoded") {
        REQUIRE(url_encode("/path/to") == "%2Fpath%2Fto");
    }

    SECTION("UTF-8 multi-byte characters are encoded") {
        // café: c a f 0xC3 0xA9
        REQUIRE(url_encode("caf\xC3\xA9") == "caf%C3%A9");
    }

    SECTION("Empty string") {
        REQUIRE(url_encode("").empty());
    }
}

// ============================================================================
// uri_path_encode
// ============================================================================

TEST_CASE("uri_path_encode", "[url][unit]") {

    SECTION("Preserves slashes in paths") {
        REQUIRE(uri_path_encode("/api/v1/users") == "/api/v1/users");
    }

    SECTION("Encodes spaces in path segments") {
        REQUIRE(uri_path_encode("/my files/doc") == "/my%20files/doc");
    }

    SECTION("Encodes query characters in path") {
        REQUIRE(uri_path_encode("/path?query") == "/path%3Fquery");
        REQUIRE(uri_path_encode("/path#frag") == "/path%23frag");
    }

    SECTION("Preserves unreserved characters") {
        REQUIRE(uri_path_encode("a-b_c.d~e") == "a-b_c.d~e");
    }

    SECTION("Empty string") {
        REQUIRE(uri_path_encode("").empty());
    }
}

// ============================================================================
// url_decode
// ============================================================================

TEST_CASE("url_decode (bool variant)", "[url][unit]") {

    SECTION("Plain text passes through") {
        std::string out;
        REQUIRE(url_decode("hello", out));
        REQUIRE(out == "hello");
    }

    SECTION("Percent-encoded characters are decoded") {
        std::string out;
        REQUIRE(url_decode("hello%20world", out));
        REQUIRE(out == "hello world");
    }

    SECTION("Plus is decoded as space") {
        std::string out;
        REQUIRE(url_decode("hello+world", out));
        REQUIRE(out == "hello world");
    }

    SECTION("Multiple percent-encoded characters") {
        std::string out;
        REQUIRE(url_decode("%48%65%6C%6C%6F", out));
        REQUIRE(out == "Hello");
    }

    SECTION("Mixed case hex digits") {
        std::string out;
        REQUIRE(url_decode("%2f%2F", out));
        REQUIRE(out == "//");
    }

    SECTION("UTF-8 encoded characters") {
        std::string out;
        REQUIRE(url_decode("caf%C3%A9", out));
        REQUIRE(out == "caf\xC3\xA9");
    }

    SECTION("Truncated percent sequence at end returns false") {
        std::string out;
        REQUIRE_FALSE(url_decode("hello%2", out));
        REQUIRE_FALSE(url_decode("hello%", out));
    }

    SECTION("Invalid hex digits return false") {
        std::string out;
        REQUIRE_FALSE(url_decode("hello%GG", out));
        REQUIRE_FALSE(url_decode("hello%XZ", out));
    }

    SECTION("Empty string") {
        std::string out;
        REQUIRE(url_decode("", out));
        REQUIRE(out.empty());
    }
}

TEST_CASE("url_decode (string variant)", "[url][unit]") {

    SECTION("Successful decode returns string") {
        REQUIRE(url_decode("hello%20world") == "hello world");
    }

    SECTION("Failed decode returns empty string") {
        REQUIRE(url_decode("hello%GG").empty());
    }
}

// ============================================================================
// url_encode / url_decode roundtrip
// ============================================================================

TEST_CASE("url_encode and url_decode roundtrip", "[url][unit]") {

    SECTION("ASCII roundtrip") {
        std::string original = "hello world! @#$%^&*()";
        REQUIRE(url_decode(url_encode(original)) == original);
    }

    SECTION("UTF-8 roundtrip") {
        std::string original = "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC";
        REQUIRE(url_decode(url_encode(original)) == original);
    }

    SECTION("Empty roundtrip") {
        REQUIRE(url_decode(url_encode("")).empty());
    }

    SECTION("Unreserved characters roundtrip unchanged") {
        std::string original = "abc-123_XYZ.test~value";
        std::string encoded = url_encode(original);
        REQUIRE(encoded == original); // not encoded at all
        REQUIRE(url_decode(encoded) == original);
    }
}

// ============================================================================
// parse_url_encoded_data
// ============================================================================

TEST_CASE("parse_url_encoded_data", "[url][unit]") {

    SECTION("Single key=value pair") {
        std::multimap<std::string, std::string> params;
        parse_url_encoded_data("key=value", params);
        REQUIRE(params.size() == 1);
        REQUIRE(params.find("key")->second == "value");
    }

    SECTION("Multiple pairs separated by &") {
        std::multimap<std::string, std::string> params;
        parse_url_encoded_data("a=1&b=2&c=3", params);
        REQUIRE(params.size() == 3);
        REQUIRE(params.find("a")->second == "1");
        REQUIRE(params.find("b")->second == "2");
        REQUIRE(params.find("c")->second == "3");
    }

    SECTION("Key with empty value") {
        std::multimap<std::string, std::string> params;
        parse_url_encoded_data("key=", params);
        REQUIRE(params.size() == 1);
        REQUIRE(params.find("key")->second.empty());
    }

    SECTION("Key without equals sign") {
        std::multimap<std::string, std::string> params;
        parse_url_encoded_data("key", params);
        REQUIRE(params.size() == 1);
        REQUIRE(params.find("key")->second.empty());
    }

    SECTION("Percent-encoded keys and values are decoded") {
        std::multimap<std::string, std::string> params;
        parse_url_encoded_data("hello%20world=foo%26bar", params);
        REQUIRE(params.size() == 1);
        REQUIRE(params.find("hello world")->second == "foo&bar");
    }

    SECTION("Plus in values decoded as space") {
        std::multimap<std::string, std::string> params;
        parse_url_encoded_data("q=hello+world", params);
        REQUIRE(params.size() == 1);
        REQUIRE(params.find("q")->second == "hello world");
    }

    SECTION("Duplicate keys create multiple entries") {
        std::multimap<std::string, std::string> params;
        parse_url_encoded_data("tag=a&tag=b", params);
        REQUIRE(params.count("tag") == 2);
    }

    SECTION("Empty string produces no entries") {
        std::multimap<std::string, std::string> params;
        parse_url_encoded_data("", params);
        REQUIRE(params.empty());
    }
}

// ============================================================================
// get_url_encoded_data
// ============================================================================

TEST_CASE("get_url_encoded_data", "[url][unit]") {

    SECTION("Single pair") {
        std::multimap<std::string, std::string> params;
        params.emplace("key", "value");
        REQUIRE(get_url_encoded_data(params) == "key=value");
    }

    SECTION("Multiple pairs joined with &") {
        std::multimap<std::string, std::string> params;
        params.emplace("a", "1");
        params.emplace("b", "2");
        auto result = get_url_encoded_data(params);
        REQUIRE(result == "a=1&b=2");
    }

    SECTION("Special characters in keys/values are encoded") {
        std::multimap<std::string, std::string> params;
        params.emplace("hello world", "foo&bar");
        auto result = get_url_encoded_data(params);
        REQUIRE(result == "hello%20world=foo%26bar");
    }

    SECTION("Empty map returns empty string") {
        std::multimap<std::string, std::string> params;
        REQUIRE(get_url_encoded_data(params).empty());
    }
}

// ============================================================================
// parse + get roundtrip
// ============================================================================

TEST_CASE("parse and get_url_encoded_data roundtrip", "[url][unit]") {

    SECTION("Roundtrip preserves data") {
        std::string original = "name=Alice&city=New%20York&lang=es";

        std::multimap<std::string, std::string> params;
        parse_url_encoded_data(original, params);

        auto result = get_url_encoded_data(params);

        // Re-parse the result to verify
        std::multimap<std::string, std::string> params2;
        parse_url_encoded_data(result, params2);

        REQUIRE(params == params2);
    }
}

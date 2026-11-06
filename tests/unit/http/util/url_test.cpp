#include <catch2/catch_test_macros.hpp>
#include <thinger/http/util/url.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

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

// The public url_decode() keeps decoding "+" as a space (query and form data); only the path
// decoding of a request keeps it literal
TEST_CASE("url_decode keeps decoding plus as a space", "[url][unit]") {
    REQUIRE(url_decode("a+b") == "a b");
    std::string out;
    REQUIRE(url_decode("a+b%2B", out));
    REQUIRE(out == "a b+");
}

// "+" is literal in a path: Node's new URL("http://h/a+b").pathname is "/a+b", Go's
// url.Parse("/a+b").Path is "/a+b"
TEST_CASE("path_decode keeps plus literal", "[url][unit]") {
    std::string out;
    REQUIRE(detail::path_decode("/a+b", out));
    REQUIRE(out == "/a+b");
    REQUIRE(detail::path_decode("/a%2Bb%20c", out));
    REQUIRE(out == "/a+b c");
    REQUIRE(detail::path_decode("", out));
    REQUIRE(out.empty());
    REQUIRE_FALSE(detail::path_decode("/a%2", out));
    REQUIRE_FALSE(detail::path_decode("/a%", out));
    REQUIRE_FALSE(detail::path_decode("/a%zz", out));
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

// ============================================================================
// loggable_uri
// ============================================================================

TEST_CASE("loggable_uri redacts credentials in query parameters and user info", "[url][unit][logging]") {

    SECTION("A target without query or user info is unchanged") {
        REQUIRE(loggable_uri("/v1/users/alice/devices") == "/v1/users/alice/devices");
        REQUIRE(loggable_uri("https://example.com/a/b") == "https://example.com/a/b");
        REQUIRE(loggable_uri("") == "");
    }

    SECTION("Credential parameters are redacted, the others kept, in order") {
        REQUIRE(loggable_uri("/v1/data?authorization=eyJhbGciOi.x.y&count=10") ==
                "/v1/data?authorization=[redacted]&count=10");
        REQUIRE(loggable_uri("/cb?code=abc&access_token=t0k3n&state=s") == "/cb?code=[redacted]&access_token=[redacted]&state=s");
        REQUIRE(loggable_uri("/x?a=1&TOKEN=secret") == "/x?a=1&TOKEN=[redacted]");
        REQUIRE(loggable_uri("/x?api%5Fkey=secret") == "/x?api%5Fkey=[redacted]");
        REQUIRE(loggable_uri("/x?password=p&client_secret=c&apikey=k&key=k2") ==
                "/x?password=[redacted]&client_secret=[redacted]&apikey=[redacted]&key=[redacted]");
    }

    SECTION("Names that only contain a credential name are kept") {
        REQUIRE(loggable_uri("/x?tokens=3&keyboard=1&mytoken=2") == "/x?tokens=3&keyboard=1&mytoken=2");
        REQUIRE(loggable_uri("/x?codec=1&zipcode=2&tokens=3&mytoken=4") == "/x?codec=1&zipcode=2&tokens=3&mytoken=4");
    }

    SECTION("Parameters without a value, empty pairs and the fragment are kept") {
        REQUIRE(loggable_uri("/x?token&a=1") == "/x?token&a=1");
        REQUIRE(loggable_uri("/x?a=1&&token=s#frag") == "/x?a=1&&token=[redacted]#frag");
        REQUIRE(loggable_uri("/x?") == "/x?");
        REQUIRE(loggable_uri("/x#token=s") == "/x#token=s");
        REQUIRE(loggable_uri("/x?code&a=1") == "/x?code&a=1");
        REQUIRE(loggable_uri("/x?a=1&&code=s#frag") == "/x?a=1&&code=[redacted]#frag");
        REQUIRE(loggable_uri("/x#code=s") == "/x#code=s");
    }

    SECTION("An empty value is redacted, and every occurrence of a repeated name") {
        REQUIRE(loggable_uri("/x?code=&a=1") == "/x?code=[redacted]&a=1");
        REQUIRE(loggable_uri("/x?code=a&b=1&code=b") == "/x?code=[redacted]&b=1&code=[redacted]");
    }

    SECTION("The name is compared decoded, and logged as it came") {
        REQUIRE(loggable_uri("/x?co%64e=abc") == "/x?co%64e=[redacted]");
        REQUIRE(loggable_uri("/x?CO%64E=abc") == "/x?CO%64E=[redacted]");
    }

    SECTION("The password of the user info is redacted, the user kept") {
        REQUIRE(loggable_uri("http://user:pa55@proxy:3128/path?token=t") ==
                "http://user:[redacted]@proxy:3128/path?token=[redacted]");
        REQUIRE(loggable_uri("wss://user@host/ws") == "wss://user@host/ws");
        REQUIRE(loggable_uri("http://host/p?q=a@b:c") == "http://host/p?q=a@b:c");
    }
}

TEST_CASE("loggable_uri redacts each default name, in any case, and keeps the rest", "[url][unit][logging]") {
    const std::vector<std::string> names{
        "access_token", "api_key", "apikey", "authorization", "client_secret", "code",
        "id_token", "key", "password", "refresh_token", "secret", "sig", "signature", "token",
        "x-amz-security-token", "x-amz-signature", "x-goog-signature"};
    REQUIRE(redacted_parameters::defaults().size() == names.size());

    for (const auto& name : names) {
        INFO(name);
        std::string upper = name;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
        REQUIRE(redacted_parameters::defaults().contains(name));
        REQUIRE(loggable_uri("/x?a=1&&" + name + "=s3cret&b=2#frag") == "/x?a=1&&" + name + "=[redacted]&b=2#frag");
        REQUIRE(loggable_uri("/x?a=1&&" + upper + "=s3cret&b=2#frag") == "/x?a=1&&" + upper + "=[redacted]&b=2#frag");
    }
}

TEST_CASE("loggable_uri redacts with the set it is given", "[url][unit][logging]") {

    SECTION("Added to the defaults") {
        auto names = redacted_parameters::defaults();
        names.add("invite");
        REQUIRE(loggable_uri("/join?invite=xyz&code=abc&page=2", names) == "/join?invite=[redacted]&code=[redacted]&page=2");
    }

    SECTION("Removed from the defaults, whatever its case") {
        auto names = redacted_parameters::defaults();
        names.remove("KEY");
        REQUIRE(loggable_uri("/x?key=visible&token=t", names) == "/x?key=visible&token=[redacted]");
    }

    SECTION("A copy changed leaves the defaults and the other copies as they were") {
        auto size = redacted_parameters::defaults().size();
        auto first = redacted_parameters::defaults();
        auto second = first;
        second.add("invite").remove("code");
        REQUIRE(first.contains("code"));
        REQUIRE_FALSE(first.contains("invite"));
        REQUIRE(second.contains("invite"));
        REQUIRE_FALSE(second.contains("code"));
        REQUIRE(redacted_parameters::defaults().size() == size);
        REQUIRE(redacted_parameters::defaults().contains("code"));
        REQUIRE_FALSE(redacted_parameters::defaults().contains("invite"));
    }

    SECTION("A set of its own replaces the defaults") {
        redacted_parameters names{"invite"};
        REQUIRE(loggable_uri("/x?invite=a&code=b", names) == "/x?invite=[redacted]&code=b");
    }

    SECTION("The case does not matter in the set or in the URI") {
        redacted_parameters names{"Invite"};
        REQUIRE(names.contains("INVITE"));
        REQUIRE(loggable_uri("/x?INVITE=a&invite=b&InViTe=c", names) == "/x?INVITE=[redacted]&invite=[redacted]&InViTe=[redacted]");
    }

    SECTION("A name is added once") {
        redacted_parameters names{"invite", "INVITE"};
        names.add("Invite");
        REQUIRE(names.size() == 1);
    }

    SECTION("An empty set redacts no parameter, but the password of the user info") {
        const redacted_parameters names{};
        REQUIRE(names.size() == 0);
        REQUIRE(loggable_uri("http://user:pa55@host/p?token=t&code=c", names) == "http://user:[redacted]@host/p?token=t&code=c");
        REQUIRE(loggable_uri("/x?token=t", {}) == "/x?token=t");
    }

    SECTION("Removing every name leaves the set empty, and a missing name changes nothing") {
        redacted_parameters names{"invite"};
        names.remove("other");
        REQUIRE(names.size() == 1);
        names.remove("invite");
        REQUIRE(names.size() == 0);
        REQUIRE(loggable_uri("/x?invite=a", names) == "/x?invite=a");
    }

    SECTION("An empty name is ignored: parameters without a name stay as they are") {
        redacted_parameters names{"", "invite"};
        names.add("");
        REQUIRE(names.size() == 1);
        REQUIRE(loggable_uri("/x?=a&invite=b", names) == "/x?=a&invite=[redacted]");
    }

    SECTION("A name decoded is compared with the set") {
        redacted_parameters names{"my invite"};
        REQUIRE(loggable_uri("/x?my%20invite=a&my+invite=b", names) == "/x?my%20invite=[redacted]&my+invite=[redacted]");
    }
}

#include <catch2/catch_test_macros.hpp>
#include <thinger/http.hpp>
#include <sstream>

TEST_CASE("HTTP Headers operations", "[http][headers]") {
    // Use http_request as a concrete implementation of http_headers
    thinger::http::http_request headers;
    
    SECTION("Add and get header") {
        headers.add_header("Content-Type", "application/json");
        
        REQUIRE(headers.has_header("Content-Type"));
        REQUIRE(headers.get_header("Content-Type") == "application/json");
        REQUIRE(headers.get_header("content-type") == "application/json"); // Case insensitive
    }
    
    SECTION("Multiple values for same header") {
        headers.add_header("Accept", "text/html");
        headers.add_header("Accept", "application/json");
        
        auto values = headers.get_headers_with_key("Accept");
        REQUIRE(values.size() == 2);
        REQUIRE(values[0] == "text/html");
        REQUIRE(values[1] == "application/json");
    }
    
    SECTION("Remove header") {
        headers.add_header("Authorization", "Bearer token123");
        REQUIRE(headers.has_header("Authorization"));
        
        headers.remove_header("Authorization");
        REQUIRE_FALSE(headers.has_header("Authorization"));
    }
    
    SECTION("Get all headers") {
        headers.add_header("Header1", "Value1");
        headers.add_header("Header2", "Value2");
        
        const auto& all_headers = headers.get_headers();
        REQUIRE(all_headers.size() >= 2);
        
        bool found1 = false, found2 = false;
        for (const auto& [name, value] : all_headers) {
            if (name == "Header1" && value == "Value1") found1 = true;
            if (name == "Header2" && value == "Value2") found2 = true;
        }
        REQUIRE(found1);
        REQUIRE(found2);
    }
    
    SECTION("Iterate headers") {
        headers.add_header("Host", "example.com");
        headers.add_header("User-Agent", "TestAgent/1.0");

        int count = 0;
        for (const auto& [name, value] : headers.get_headers()) {
            count++;
            REQUIRE(!name.empty());
            REQUIRE(!value.empty());
        }
        REQUIRE(count >= 2);
    }

    SECTION("set is alias for set_header") {
        headers.set("X-Custom", "value1");
        REQUIRE(headers.get_header("X-Custom") == "value1");
        headers.set("X-Custom", "value2");
        REQUIRE(headers.get_header("X-Custom") == "value2");
    }

    SECTION("set_header replaces existing case-insensitive") {
        headers.add_header("Content-Type", "text/html");
        headers.set_header("content-type", "application/json");
        REQUIRE(headers.get_header("Content-Type") == "application/json");
    }

    SECTION("remove_header returns false for non-existent") {
        REQUIRE_FALSE(headers.remove_header("NonExistent"));
    }

    SECTION("get_header returns empty for non-existent") {
        REQUIRE(headers.get_header("NonExistent").empty());
    }

    SECTION("empty_headers") {
        REQUIRE(headers.empty_headers());
        headers.add_header("Key", "Value");
        REQUIRE_FALSE(headers.empty_headers());
    }

    SECTION("upgrade and stream flags default false") {
        REQUIRE_FALSE(headers.upgrade());
        REQUIRE_FALSE(headers.stream());
    }
}

// ============================================================================
// process_header — Connection, Accept, Content-Length
// ============================================================================

TEST_CASE("Headers process_header", "[http][headers]") {

    SECTION("Connection: keep-alive sets keep_alive") {
        thinger::http::http_request h;
        h.process_header("Connection", "keep-alive");
        REQUIRE(h.keep_alive() == true);
        REQUIRE(h.upgrade() == false);
    }

    SECTION("Connection: close sets keep_alive false") {
        thinger::http::http_request h;
        h.process_header("Connection", "close");
        REQUIRE(h.keep_alive() == false);
    }

    SECTION("Connection: keep-alive, upgrade (Firefox WebSocket)") {
        thinger::http::http_request h;
        h.process_header("Connection", "keep-alive, upgrade");
        REQUIRE(h.keep_alive() == true);
        REQUIRE(h.upgrade() == true);
    }

    SECTION("Connection: Upgrade alone sets upgrade flag") {
        thinger::http::http_request h;
        h.process_header("Connection", "Upgrade");
        REQUIRE(h.upgrade() == true);
    }

    SECTION("Accept: text/event-stream sets stream") {
        thinger::http::http_request h;
        h.process_header("Accept", "text/event-stream");
        REQUIRE(h.stream() == true);
    }

    SECTION("Accept: other does not set stream") {
        thinger::http::http_request h;
        h.process_header("Accept", "application/json");
        REQUIRE(h.stream() == false);
    }

    SECTION("Content-Length with valid value") {
        thinger::http::http_request h;
        h.process_header("Content-Length", "42");
        REQUIRE(h.get_content_length() == 42);
    }

    SECTION("Content-Length with invalid value falls back to 0") {
        thinger::http::http_request h;
        h.process_header("Content-Length", "not-a-number");
        REQUIRE(h.get_content_length() == 0);
    }

    SECTION("process_header adds to headers vector") {
        thinger::http::http_request h;
        h.process_header("X-Custom", "value");
        REQUIRE(h.has_header("X-Custom"));
        REQUIRE(h.get_header("X-Custom") == "value");
    }
}

// ============================================================================
// keep_alive and HTTP version
// ============================================================================

TEST_CASE("Headers keep_alive and HTTP version", "[http][headers]") {

    SECTION("Indeterminate keep_alive with HTTP/1.1 defaults true") {
        thinger::http::http_request h;
        REQUIRE(h.keep_alive() == true);
    }

    SECTION("Indeterminate keep_alive with HTTP/1.0 defaults false") {
        thinger::http::http_request h;
        h.set_http_version_major(1);
        h.set_http_version_minor(0);
        REQUIRE(h.keep_alive() == false);
    }

    SECTION("HTTP version getters and setters") {
        thinger::http::http_request h;
        REQUIRE(h.get_http_version_major() == 1);
        REQUIRE(h.get_http_version_minor() == 1);

        h.set_http_version_major(2);
        h.set_http_version_minor(0);
        REQUIRE(h.get_http_version_major() == 2);
        REQUIRE(h.get_http_version_minor() == 0);
    }

    SECTION("set_keep_alive true sets Connection header") {
        thinger::http::http_request h;
        h.set_keep_alive(true);
        REQUIRE(h.keep_alive() == true);
        REQUIRE(h.get_header("Connection") == "Keep-Alive");
    }

    SECTION("set_keep_alive false sets Connection: Close") {
        thinger::http::http_request h;
        h.set_keep_alive(false);
        REQUIRE(h.keep_alive() == false);
        REQUIRE(h.get_header("Connection") == "Close");
    }
}

// ============================================================================
// Proxy headers
// ============================================================================

TEST_CASE("Headers proxy operations", "[http][headers]") {
    thinger::http::http_request h;

    SECTION("add_proxy does not crash") {
        h.add_proxy("X-Forwarded-For", "192.168.1.1");
        h.add_proxy("X-Forwarded-Proto", "https");
    }

    SECTION("set_proxy replaces existing") {
        h.add_proxy("X-Forwarded-For", "192.168.1.1");
        h.set_proxy("X-Forwarded-For", "10.0.0.1");
    }

    SECTION("add_proxy with empty key is no-op") {
        h.add_proxy("", "value");
    }

    SECTION("set_proxy with new key adds it") {
        h.set_proxy("X-New-Proxy", "value");
    }
}

// ============================================================================
// Convenience getters
// ============================================================================

TEST_CASE("Headers convenience getters", "[http][headers]") {
    thinger::http::http_request h;

    SECTION("get_authorization") {
        h.add_header("Authorization", "Bearer token123");
        REQUIRE(h.get_authorization() == "Bearer token123");
    }

    SECTION("get_cookie") {
        h.add_header("Cookie", "session=abc; lang=en");
        REQUIRE(h.get_cookie() == "session=abc; lang=en");
    }

    SECTION("get_user_agent") {
        h.add_header("User-Agent", "TestAgent/1.0");
        REQUIRE(h.get_user_agent() == "TestAgent/1.0");
    }

    SECTION("get_content_type") {
        h.add_header("Content-Type", "application/json");
        REQUIRE(h.get_content_type() == "application/json");
    }

    SECTION("is_content_type case insensitive prefix match") {
        h.add_header("Content-Type", "Application/JSON; charset=utf-8");
        REQUIRE(h.is_content_type("application/json"));
        REQUIRE_FALSE(h.is_content_type("text/html"));
    }

    SECTION("is_content_type with no Content-Type header") {
        REQUIRE_FALSE(h.is_content_type("text/html"));
    }

    SECTION("Convenience getters return empty for missing headers") {
        REQUIRE(h.get_authorization().empty());
        REQUIRE(h.get_cookie().empty());
        REQUIRE(h.get_user_agent().empty());
        REQUIRE(h.get_content_type().empty());
    }
}

// ============================================================================
// get_parameter — cookie/header value parsing
// ============================================================================

TEST_CASE("Headers get_parameter", "[http][headers]") {

    SECTION("Parse simple parameter") {
        REQUIRE(thinger::http::headers::get_parameter("session=abc123", "session") == "abc123");
    }

    SECTION("Parse multiple parameters") {
        std::string cookie = "session=abc123; lang=en; theme=dark";
        REQUIRE(thinger::http::headers::get_parameter(cookie, "session") == "abc123");
        REQUIRE(thinger::http::headers::get_parameter(cookie, "lang") == "en");
        REQUIRE(thinger::http::headers::get_parameter(cookie, "theme") == "dark");
    }

    SECTION("Parse quoted values") {
        std::string cookie = R"(session="abc123"; data="some value")";
        REQUIRE(thinger::http::headers::get_parameter(cookie, "session") == "abc123");
        REQUIRE(thinger::http::headers::get_parameter(cookie, "data") == "some value");
    }

    SECTION("Non-existent parameter returns empty") {
        REQUIRE(thinger::http::headers::get_parameter("session=abc", "token").empty());
    }

    SECTION("Empty header returns empty") {
        REQUIRE(thinger::http::headers::get_parameter("", "key").empty());
    }
}

// ============================================================================
// debug_headers
// ============================================================================

TEST_CASE("Headers debug_headers", "[http][headers]") {
    thinger::http::http_request h;
    h.add_header("Host", "example.com");
    h.add_header("Accept", "text/html");

    std::ostringstream os;
    h.debug_headers(os);
    std::string output = os.str();
    REQUIRE(output.find("Host") != std::string::npos);
    REQUIRE(output.find("example.com") != std::string::npos);
    REQUIRE(output.find("Accept") != std::string::npos);
}

// ============================================================================
// log
// ============================================================================

TEST_CASE("Headers log does not crash", "[http][headers]") {
    thinger::http::http_request h;
    h.add_header("Host", "example.com");
    h.add_proxy("X-Forwarded-For", "10.0.0.1");
    REQUIRE_NOTHROW(h.log("test", 0));
}
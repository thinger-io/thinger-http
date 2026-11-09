#include <catch2/catch_test_macros.hpp>
#include <thinger/http.hpp>
#include <thinger/http/common/syntax.hpp>
#include <thinger/util/logger.hpp>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <sstream>
#include <chrono>
#include <memory>

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

    SECTION("Connection options: any case, surrounding whitespace, empty elements") {
        thinger::http::http_request h;
        h.process_header("Connection", ",\t UPGRADE \t,, Close ,");
        REQUIRE(h.keep_alive() == false);
        REQUIRE(h.upgrade() == true);

        thinger::http::http_request other;
        other.process_header("Connection", "keep-alive-ish, closed, upgrades");
        REQUIRE(other.keep_alive() == true);   // HTTP/1.1 default: no option matched
        REQUIRE(other.upgrade() == false);
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
    REQUIRE_NOTHROW(h.log("test"));
}
// ============================================================================
// Body framing headers (Content-Length / Transfer-Encoding)
// ============================================================================

TEST_CASE("Headers framing decisions on repeated headers", "[http][headers][framing]") {
    using thinger::http::http_request;

    SECTION("Content-Length repeated with the same value is valid") {
        http_request h;
        h.process_header("Content-Length", "5");
        h.process_header("Content-Length", "5");
        REQUIRE_FALSE(h.has_invalid_content_length());
        REQUIRE(h.has_valid_framing());
        REQUIRE(h.get_content_length() == 5);
    }

    SECTION("Content-Length repeated with a different value is invalid") {
        http_request h;
        h.process_header("Content-Length", "5");
        h.process_header("Content-Length", "6");
        REQUIRE(h.has_invalid_content_length());
        REQUIRE_FALSE(h.has_valid_framing());
    }

    SECTION("Invalid Content-Length followed by a valid one stays invalid") {
        http_request h;
        h.process_header("Content-Length", "abc");
        h.process_header("Content-Length", "5");
        REQUIRE(h.has_invalid_content_length());
    }

    SECTION("Content-Length added before a parsed one with another value is invalid") {
        http_request h;
        h.add_header("Content-Length", "5");
        h.process_header("Content-Length", "7");
        REQUIRE(h.has_invalid_content_length());
    }

    SECTION("Content-Length parsed again after removing the previous one") {
        http_request h;
        h.process_header("Content-Length", "5");
        REQUIRE(h.remove_header("Content-Length"));
        h.process_header("Content-Length", "7");
        REQUIRE_FALSE(h.has_invalid_content_length());
        REQUIRE(h.get_content_length() == 7);
    }

    SECTION("remove_headers removes every instance") {
        http_request h;
        h.add_header("Content-Length", "1");
        h.add_header("X-Other", "x");
        h.add_header("content-length", "2");
        REQUIRE(h.remove_headers("Content-Length") == 2);
        REQUIRE_FALSE(h.has_header("Content-Length"));
        REQUIRE_FALSE(h.has_content_length_header());
        REQUIRE(h.has_header("X-Other"));
        REQUIRE(h.remove_headers("Content-Length") == 0);
        h.process_header("Content-Length", "7");
        REQUIRE_FALSE(h.has_invalid_content_length());
    }

    SECTION("Single Transfer-Encoding: chunked is valid") {
        http_request h;
        h.process_header("Transfer-Encoding", "chunked");
        REQUIRE(h.is_chunked_transfer());
        REQUIRE(h.has_valid_framing());
    }

    SECTION("Repeated Transfer-Encoding is invalid") {
        http_request h;
        h.process_header("Transfer-Encoding", "chunked");
        h.process_header("transfer-encoding", "chunked");
        REQUIRE_FALSE(h.has_valid_framing());
    }

    SECTION("Transfer-Encoding set before a parsed one is invalid") {
        http_request h;
        h.set_header("Transfer-Encoding", "chunked");
        h.process_header("Transfer-Encoding", "chunked");
        REQUIRE_FALSE(h.has_valid_framing());
    }

    SECTION("Transfer-Encoding in HTTP/1.0 is invalid") {
        http_request h;
        h.set_http_version_major(1);
        h.set_http_version_minor(0);
        h.process_header("Transfer-Encoding", "chunked");
        REQUIRE_FALSE(h.has_valid_framing());

        http_request h11;
        h11.set_http_version_major(1);
        h11.set_http_version_minor(1);
        h11.process_header("Transfer-Encoding", "chunked");
        REQUIRE(h11.has_valid_framing());
    }

    SECTION("Content-Length together with Transfer-Encoding is invalid, in any order") {
        http_request a;
        a.process_header("Content-Length", "5");
        a.process_header("Transfer-Encoding", "chunked");
        REQUIRE_FALSE(a.has_valid_framing());

        http_request b;
        b.process_header("Transfer-Encoding", "chunked");
        b.process_header("Content-Length", "5");
        REQUIRE_FALSE(b.has_valid_framing());
    }
}

TEST_CASE("Repeated framing headers are processed in linear time", "[http][headers][framing]") {
    using thinger::http::http_request;
    // Other headers first: each repeated header used to scan all of them
    constexpr int repetitions = 20000;
    auto with_filler = []() {
        auto h = std::make_unique<http_request>();
        for (int i = 0; i < repetitions; i++) h->process_header("X-Filler", "value");
        return h;
    };

    SECTION("Content-Length") {
        auto h = with_filler();
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < repetitions; i++) h->process_header("Content-Length", "5");
        auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE(elapsed < std::chrono::seconds(2));
        REQUIRE(h->has_valid_framing());
        REQUIRE(h->get_content_length() == 5);
    }

    SECTION("Transfer-Encoding") {
        auto h = with_filler();
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < repetitions; i++) h->process_header("Transfer-Encoding", "chunked");
        auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE(elapsed < std::chrono::seconds(2));
        REQUIRE_FALSE(h->has_valid_framing());
    }
}

namespace {

    // The library log lines, as "[level] message", while alive
    struct header_log_capture {
        std::shared_ptr<spdlog::logger> previous = thinger::logging::get_logger();
        std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink =
            std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(64);

        header_log_capture() {
            auto logger = std::make_shared<spdlog::logger>("headers_test", sink);
            logger->set_pattern("[%l] %v");
            logger->set_level(spdlog::level::debug);
            thinger::logging::set_logger(logger);
        }
        ~header_log_capture() { thinger::logging::set_logger(previous); }

        std::vector<std::string> lines() const { return sink->last_formatted(); }
    };

    const std::vector<std::string> invalid_values = {
        std::string("v\r\nX-Injected: 1"), std::string("v\nX"), std::string("v\rX"),
        std::string("v\0X", 3), std::string("v\x7fX"), std::string("v\x01X"), std::string("v\x1fX"),
    };

    const std::vector<std::string> invalid_names = {
        std::string("X-A\r\nX-B"), std::string("X\nA"), std::string("X\rA"), std::string("X\0A", 3),
        std::string("X A"), std::string("X:A"), std::string("X\x7f"), std::string("Caf\xc3\xa9"),
        std::string("X\tA"), std::string("(X)"),
    };

}

TEST_CASE("Header setters refuse invalid names and values", "[http][headers][validation]") {
    using thinger::http::http_request;
    using thinger::http::http_response;

    SECTION("add_header stores nothing for an invalid value") {
        for (const auto& value : invalid_values) {
            http_response h;
            h.add_header("X-A", "ok");
            h.add_header("X-Bad", value);
            REQUIRE_FALSE(h.has_header("X-Bad"));
            REQUIRE(h.get_headers().size() == 1);
            REQUIRE(h.has_refused_header());
        }
    }

    SECTION("add_header stores nothing for an invalid name") {
        for (const auto& name : invalid_names) {
            http_request h;
            h.add_header(name, "value");
            REQUIRE(h.get_headers().empty());
            REQUIRE(h.has_refused_header());
        }
    }

    SECTION("A refused set_header leaves the message as it was") {
        http_response h;
        h.add_header("X-A", "first");
        h.set_header("X-A", "bad\nvalue");
        REQUIRE(h.get_header("X-A") == "first");
        h.set("X-A", std::string("bad\0value", 9));
        REQUIRE(h.get_header("X-A") == "first");
        h.set_header("X\r\nA", "value");
        REQUIRE(h.get_headers().size() == 1);
        REQUIRE(h.has_refused_header());
    }

    SECTION("replace_headers leaves a single field, unless refused") {
        http_response h;
        h.add_header("X-A", "first");
        h.add_header("X-B", "other");
        h.add_header("x-a", "second");
        h.replace_headers("X-A", "bad\nvalue");
        REQUIRE(h.get_headers().size() == 3);
        REQUIRE(h.get_headers_with_key("X-A") == std::vector<std::string>{"first", "second"});
        h.replace_headers("x-A", "third");
        REQUIRE(h.get_headers().size() == 2);
        REQUIRE(h.get_headers_with_key("X-A") == std::vector<std::string>{"third"});
        REQUIRE(h.get_headers()[0].first == "X-A");
        h.replace_headers("X-C", "new");
        REQUIRE(h.get_header("X-C") == "new");
    }

    SECTION("Framing setters built on them are checked too") {
        http_response h;
        h.set_content_type("text/plain\r\nX-Injected: 1");
        REQUIRE_FALSE(h.has_header("Content-Type"));
        REQUIRE(h.has_refused_header());
        h.set_content_type(std::string("text/plain"));
        REQUIRE(h.get_header("Content-Type") == "text/plain");
    }

    SECTION("Proxy headers are checked") {
        http_request h;
        h.add_proxy("Proxy-Authorization", "Basic x\r\nX: 1");
        h.set_proxy("Proxy\nAuthorization", "Basic x");
        h.set_proxy("X-Proxy", "a\x7f");
        std::vector<boost::asio::const_buffer> buffers;
        h.set_method(thinger::http::method::GET);
        h.set_uri("/");
        h.to_buffer(buffers);
        std::string serialized;
        for (const auto& b : buffers) serialized.append(static_cast<const char*>(b.data()), b.size());
        REQUIRE(serialized.find("Proxy") == std::string::npos);
        REQUIRE(h.has_refused_header());
        h.set_proxy("X-Proxy", "ok");
        h.set_proxy("X-Proxy", "ok\n");
        buffers.clear();
        h.to_buffer(buffers);
        serialized.clear();
        for (const auto& b : buffers) serialized.append(static_cast<const char*>(b.data()), b.size());
        REQUIRE(serialized.find("X-Proxy: ok\r\n") != std::string::npos);
    }

    SECTION("An application Host with CR/LF is refused") {
        http_request h;
        h.set_host("example.com\r\nX-Injected: 1");
        REQUIRE_FALSE(h.has_header("Host"));
        REQUIRE(h.has_refused_header());
    }

    SECTION("Valid headers pass byte for byte") {
        http_response h;
        h.add_header("X-Tab", "a\tb");
        h.add_header("X-Obs", "caf\xc3\xa9");
        h.add_header("X-Empty", "");
        h.add_header("X-Spaces", " padded ");
        h.add_header("!#$%&'*+-.^_`|~09azAZ", "symbols");
        REQUIRE_FALSE(h.has_refused_header());
        REQUIRE(h.get_header("X-Tab") == "a\tb");
        REQUIRE(h.get_header("X-Obs") == "caf\xc3\xa9");
        REQUIRE(h.has_header("X-Empty"));
        REQUIRE(h.get_header("X-Empty").empty());
        REQUIRE(h.get_header("X-Spaces") == " padded ");
        REQUIRE(h.get_header("!#$%&'*+-.^_`|~09azAZ") == "symbols");
    }

    SECTION("The setters follow the syntax rules for every byte at every position") {
        // Values are checked eight bytes at a time, and names with a table: compare both with
        // syntax::is_field_value and is_token, at lengths around the word size, between valid
        // ASCII and obs-text bytes
        namespace syntax = thinger::http::syntax;
        size_t checked = 0, mismatches = 0;
        for (size_t size : {1, 7, 8, 9, 15, 16, 17, 24, 31}) {
            for (char filler : {'a', '\x80', '\xff'}) {
                for (size_t at = 0; at < size; at++) {
                    for (int byte = 0; byte < 256; byte++) {
                        std::string value(size, filler);
                        value[at] = static_cast<char>(byte);
                        http_response h;
                        h.add_header("X-A", value);
                        if (h.has_header("X-A") != syntax::is_field_value(value)) mismatches++;
                        std::string name(size, 'a');
                        name[at] = static_cast<char>(byte);
                        h.add_header(name, "v");
                        if (h.has_header(name) != syntax::is_token(name)) mismatches++;
                        checked++;
                    }
                }
            }
        }
        REQUIRE(checked > 50000);
        REQUIRE(mismatches == 0);

        http_response h;
        h.add_header("X-Long", std::string(40, 'a') + "\t" + std::string(40, '\xfe'));
        REQUIRE(h.has_header("X-Long"));
        h.add_header("X-Long-Bad", std::string(40, 'a') + "\r\n" + std::string(40, 'b'));
        REQUIRE_FALSE(h.has_header("X-Long-Bad"));
        h.add_header("X-Tail-Bad", std::string(13, ' ') + "\x7f");
        REQUIRE_FALSE(h.has_header("X-Tail-Bad"));
    }

    SECTION("An empty name is ignored, not refused") {
        http_request h;
        h.add_header("", "value");
        h.set_header("", "value");
        REQUIRE(h.get_headers().empty());
        REQUIRE_FALSE(h.has_refused_header());
    }

    SECTION("Parsed headers are not checked by the setters") {
        // The parsers check the header lines themselves; process_header stores as it is
        http_request h;
        h.process_header("Host", "example.com:8080");
        h.process_header("X-A", "v");
        REQUIRE(h.get_header("Host") == "example.com:8080");
        REQUIRE(h.get_host() == "example.com");
        REQUIRE(h.get_port() == "8080");
        REQUIRE_FALSE(h.has_refused_header());
    }

    SECTION("A refusal logs the escaped name at debug level, never the value") {
        header_log_capture log;
        http_response h;
        h.add_header("X-Secret", "token\r\nX-Injected: 1");
        h.add_header("X-A\r\nX-B", "token");
        auto lines = log.lines();
        REQUIRE(lines.size() == 2);
        for (const auto& line : lines) {
            REQUIRE(line.rfind("[debug] ", 0) == 0);
            REQUIRE(line.find("token") == std::string::npos);
            REQUIRE(line.find('\r') == std::string::npos);
            REQUIRE(line.find('\n') == line.size() - 1);
        }
        REQUIRE(lines[0].find("X-Secret") != std::string::npos);
        REQUIRE(lines[1].find("X-A\\r\\nX-B") != std::string::npos);
    }
}

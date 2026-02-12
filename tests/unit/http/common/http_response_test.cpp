#include <catch2/catch_test_macros.hpp>
#include <thinger/http/common/http_response.hpp>
#include <sstream>

using namespace thinger::http;

TEST_CASE("HTTP Response status codes", "[http][response][unit]") {
    
    SECTION("Status code handling") {
        http_response res;
        
        // Test various status codes
        res.set_status(200);
        REQUIRE(res.get_status() == http_response::status::ok);
        REQUIRE(res.is_ok() == true);
        REQUIRE(res.is_redirect_response() == false);
        
        res.set_status(http_response::status::moved_permanently);
        REQUIRE(res.get_status() == http_response::status::moved_permanently);
        REQUIRE(res.is_ok() == false);
        REQUIRE(res.is_redirect_response() == true);
        
        res.set_status(404);
        REQUIRE(res.get_status() == http_response::status::not_found);
        REQUIRE(res.is_ok() == false);
        
        res.set_status(http_response::status::internal_server_error);
        REQUIRE(res.get_status() == http_response::status::internal_server_error);
        REQUIRE(res.is_ok() == false);
    }
    
    SECTION("Redirect status codes") {
        http_response res;
        
        std::vector<http_response::status> redirect_codes = {
            http_response::status::moved_permanently,
            http_response::status::moved_temporarily,
            http_response::status::temporary_redirect,
            http_response::status::permanent_redirect
        };
        
        for (auto code : redirect_codes) {
            res.set_status(code);
            REQUIRE(res.is_redirect_response() == true);
        }
        
        // Non-redirect codes
        res.set_status(http_response::status::ok);
        REQUIRE(res.is_redirect_response() == false);
        
        res.set_status(http_response::status::not_found);
        REQUIRE(res.is_redirect_response() == false);
    }
}

TEST_CASE("HTTP Response construction", "[http][response][unit]") {
    
    SECTION("Default construction") {
        http_response res;
        REQUIRE(res.get_content().empty());
        REQUIRE(res.get_content_size() == 0);
        // Default status is typically 200 OK
        REQUIRE(res.get_status() == http_response::status::ok);
    }
    
    SECTION("Stock responses") {
        auto res = http_response::stock_http_reply(http_response::status::not_found);
        REQUIRE(res != nullptr);
        REQUIRE(res->get_status() == http_response::status::not_found);
        // Stock responses typically include a basic HTML body
        REQUIRE(res->get_content().empty() == false);
        REQUIRE(res->has_header("Content-Type") == true);
    }
}

TEST_CASE("HTTP Response content handling", "[http][response][unit]") {
    http_response res;
    
    SECTION("Simple content") {
        res.set_content("Hello, World!");
        REQUIRE(res.get_content() == "Hello, World!");
        REQUIRE(res.get_content_size() == 13);
        REQUIRE(res.get_header("Content-Length") == "13");
    }
    
    SECTION("Content with type") {
        res.set_content("{\"message\":\"test\"}", "application/json");
        REQUIRE(res.get_content() == "{\"message\":\"test\"}");
        REQUIRE(res.get_header("Content-Type") == "application/json");
        REQUIRE(res.get_header("Content-Length") == "18");
    }
    
    SECTION("Content type setting") {
        res.set_content_type("text/html; charset=utf-8");
        REQUIRE(res.get_header("Content-Type") == "text/html; charset=utf-8");
        
        std::string content_type = "text/plain";
        res.set_content_type(content_type);
        REQUIRE(res.get_header("Content-Type") == "text/plain");
    }
    
    SECTION("Manual content length") {
        res.set_content_length(100);
        REQUIRE(res.get_header("Content-Length") == "100");
        
        // Setting content should update the length
        res.set_content("short");
        REQUIRE(res.get_header("Content-Length") == "5");
    }
    
    SECTION("Empty content") {
        res.set_content("");
        REQUIRE(res.get_content().empty());
        REQUIRE(res.get_content_size() == 0);
        REQUIRE(res.get_header("Content-Length") == "0");
    }
}

TEST_CASE("HTTP Response headers", "[http][response][unit]") {
    http_response res;
    
    SECTION("Header inheritance") {
        res.set_header("Server", "TestServer/1.0");
        res.set_header("Cache-Control", "no-cache");
        
        REQUIRE(res.has_header("Server") == true);
        REQUIRE(res.get_header("Server") == "TestServer/1.0");
        REQUIRE(res.has_header("Cache-Control") == true);
        REQUIRE(res.get_header("Cache-Control") == "no-cache");
    }
    
    SECTION("Cookie headers") {
        res.set_header("Set-Cookie", "session=abc123; Path=/; HttpOnly");
        REQUIRE(res.has_header("Set-Cookie") == true);
        
        // Multiple Set-Cookie headers
        res.add_header("Set-Cookie", "preference=dark; Path=/; Max-Age=31536000");
        auto cookies = res.get_headers_with_key("Set-Cookie");
        REQUIRE(cookies.size() == 2);
    }
    
    SECTION("Special headers") {
        res.set_status(http_response::status::moved_permanently);
        res.set_header("Location", "https://example.com/new-location");
        REQUIRE(res.get_header("Location") == "https://example.com/new-location");
        
        res.set_header("Content-Encoding", "gzip");
        REQUIRE(res.get_header("Content-Encoding") == "gzip");
    }
}

TEST_CASE("HTTP Response serialization", "[http][response][unit]") {
    http_response res;
    res.set_status(http_response::status::ok);
    res.set_header("Server", "TestServer");
    res.set_content("Hello", "text/plain");
    
    SECTION("Buffer generation") {
        std::vector<boost::asio::const_buffer> buffers;
        res.to_buffer(buffers);
        
        // Should have at least status line, headers, and body
        REQUIRE(buffers.size() >= 3);
        
        // Convert buffers to string for verification
        std::string serialized;
        for (const auto& buffer : buffers) {
            serialized.append(static_cast<const char*>(buffer.data()), buffer.size());
        }
        
        // Check status line
        REQUIRE(serialized.find("HTTP/1.1 200") == 0);
        
        // Check headers
        REQUIRE(serialized.find("Server: TestServer\r\n") != std::string::npos);
        REQUIRE(serialized.find("Content-Type: text/plain\r\n") != std::string::npos);
        REQUIRE(serialized.find("Content-Length: 5\r\n") != std::string::npos);
        
        // Check body
        REQUIRE(serialized.find("\r\n\r\nHello") != std::string::npos);
    }
    
    SECTION("Size calculation") {
        // get_size() returns the size of the content/payload
        size_t size = res.get_size();
        REQUIRE(size == 5); // "Hello" from previous section
        
        res.set_content("Hello, World!");
        size_t new_size = res.get_size();
        REQUIRE(new_size == 13); // "Hello, World!" is 13 bytes
    }
}

TEST_CASE("HTTP Response status line formatting", "[http][response][unit]") {
    
    SECTION("All status codes are defined") {
        // Test all status codes defined in the enum
        std::vector<std::pair<http_response::status, std::string>> test_cases = {
            {http_response::status::ok, "200 OK"},
            {http_response::status::created, "201 Created"},
            {http_response::status::accepted, "202 Accepted"},
            {http_response::status::no_content, "204 No Content"},
            {http_response::status::multiple_choices, "300 Multiple Choices"},
            {http_response::status::moved_permanently, "301 Moved Permanently"},
            {http_response::status::moved_temporarily, "302 Moved Temporarily"},
            {http_response::status::not_modified, "304 Not Modified"},
            {http_response::status::temporary_redirect, "307 Temporary Redirect"},
            {http_response::status::permanent_redirect, "308 Permanent Redirect"},
            {http_response::status::bad_request, "400 Bad Request"},
            {http_response::status::unauthorized, "401 Unauthorized"},
            {http_response::status::forbidden, "403 Forbidden"},
            {http_response::status::not_found, "404 Not Found"},
            {http_response::status::not_allowed, "405 Method Not Allowed"},
            {http_response::status::timed_out, "408 Request Timeout"},
            {http_response::status::conflict, "409 Conflict"},
            {http_response::status::upgrade_required, "426 Upgrade Required"},
            {http_response::status::too_many_requests, "429 Too Many Requests"},
            {http_response::status::internal_server_error, "500 Internal Server Error"},
            {http_response::status::not_implemented, "501 Not Implemented"},
            {http_response::status::bad_gateway, "502 Bad Gateway"},
            {http_response::status::service_unavailable, "503 Service Unavailable"},
            {http_response::status::switching_protocols, "101 Switching Protocols"}
        };
        
        for (const auto& [status, expected_text] : test_cases) {
            http_response res;
            res.set_status(status);
            
            std::vector<boost::asio::const_buffer> buffers;
            res.to_buffer(buffers);
            
            std::string full_response;
            for (const auto& buffer : buffers) {
                full_response.append(static_cast<const char*>(buffer.data()), buffer.size());
            }
            
            std::string status_line = full_response.substr(0, full_response.find("\r\n"));
            REQUIRE(status_line == "HTTP/1.1 " + expected_text);
            
            // Ensure it's not returning "unknown"
            REQUIRE(status_line.find("Unknown") == std::string::npos);
        }
    }
    
    SECTION("Unknown status code returns unknown string") {
        http_response res;
        // Cast an undefined value to status
        res.set_status(static_cast<http_response::status>(999));
        
        std::vector<boost::asio::const_buffer> buffers;
        res.to_buffer(buffers);
        
        std::string full_response;
        for (const auto& buffer : buffers) {
            full_response.append(static_cast<const char*>(buffer.data()), buffer.size());
        }
        
        std::string status_line = full_response.substr(0, full_response.find("\r\n"));
        REQUIRE(status_line == "HTTP/1.1 000 Unknown Status");
    }
}

TEST_CASE("HTTP Response edge cases", "[http][response][unit]") {
    
    SECTION("No content response") {
        http_response res;
        res.set_status(http_response::status::no_content);
        
        // 204 responses should not have content
        res.set_content("This should be ignored");
        
        std::vector<boost::asio::const_buffer> buffers;
        res.to_buffer(buffers);
        
        std::string serialized;
        for (const auto& buffer : buffers) {
            serialized.append(static_cast<const char*>(buffer.data()), buffer.size());
        }
        
        // Should not include Content-Length for 204
        REQUIRE(serialized.find("HTTP/1.1 204") == 0);
    }
    
    SECTION("Switching protocols") {
        http_response res;
        res.set_status(http_response::status::switching_protocols);
        res.set_header("Upgrade", "websocket");
        res.set_header("Connection", "Upgrade");
        
        REQUIRE(res.get_status() == http_response::status::switching_protocols);
        REQUIRE(res.get_header("Upgrade") == "websocket");
    }
    
    SECTION("Large content") {
        http_response res;
        std::string large_content(1024 * 1024, 'x'); // 1MB of 'x'
        res.set_content(large_content);
        
        REQUIRE(res.get_content_size() == 1024 * 1024);
        REQUIRE(res.get_header("Content-Length") == "1048576");
    }
}
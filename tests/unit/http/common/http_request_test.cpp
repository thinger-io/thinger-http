#include <catch2/catch_test_macros.hpp>
#include <thinger/http/common/http_request.hpp>
#include <sstream>

using namespace thinger::http;

TEST_CASE("HTTP Request method handling", "[http][request][unit]") {
    
    SECTION("Method enum to string conversion") {
        REQUIRE(get_method(method::GET) == "GET");
        REQUIRE(get_method(method::POST) == "POST");
        REQUIRE(get_method(method::PUT) == "PUT");
        REQUIRE(get_method(method::DELETE) == "DELETE");
        REQUIRE(get_method(method::HEAD) == "HEAD");
        REQUIRE(get_method(method::OPTIONS) == "OPTIONS");
        REQUIRE(get_method(method::PATCH) == "PATCH");
        REQUIRE(get_method(method::CONNECT) == "CONNECT");
        REQUIRE(get_method(method::TRACE) == "TRACE");
    }
    
    SECTION("String to method enum conversion") {
        REQUIRE(get_method("GET") == method::GET);
        REQUIRE(get_method("POST") == method::POST);
        REQUIRE(get_method("PUT") == method::PUT);
        REQUIRE(get_method("DELETE") == method::DELETE);
        REQUIRE(get_method("HEAD") == method::HEAD);
        REQUIRE(get_method("OPTIONS") == method::OPTIONS);
        REQUIRE(get_method("PATCH") == method::PATCH);
        REQUIRE(get_method("CONNECT") == method::CONNECT);
        REQUIRE(get_method("TRACE") == method::TRACE);
        REQUIRE(get_method("INVALID") == method::UNKNOWN);
    }
}

TEST_CASE("HTTP Request construction", "[http][request][unit]") {
    
    SECTION("Default construction") {
        http_request req;
        REQUIRE(req.get_method() == method::UNKNOWN);
        REQUIRE(req.get_host().empty());
        REQUIRE(req.get_port() == "80");  // Default is HTTP port
        REQUIRE(req.get_uri().empty());
        REQUIRE(req.is_ssl() == false);
        REQUIRE(req.has_content() == false);
    }
    
    SECTION("URL parsing") {
        http_request req;
        
        SECTION("HTTP URL") {
            REQUIRE(req.set_url("http://example.com/path") == true);
            REQUIRE(req.get_host() == "example.com");
            REQUIRE(req.get_port() == "80");
            REQUIRE(req.get_protocol() == "http");
            REQUIRE(req.get_uri() == "/path");
            REQUIRE(req.is_ssl() == false);
        }
        
        SECTION("HTTPS URL") {
            REQUIRE(req.set_url("https://example.com:8443/api/v1") == true);
            REQUIRE(req.get_host() == "example.com");
            REQUIRE(req.get_port() == "8443");
            REQUIRE(req.get_protocol() == "https");
            REQUIRE(req.get_uri() == "/api/v1");
            REQUIRE(req.is_ssl() == true);
        }
        
        SECTION("URL with query parameters") {
            REQUIRE(req.set_url("http://example.com/search?q=test&lang=en") == true);
            REQUIRE(req.get_uri() == "/search?q=test&lang=en");
            REQUIRE(req.get_path() == "/search");  // New test for get_path()
            REQUIRE(req.has_query_parameters() == true);
            REQUIRE(req.has_uri_parameter("q") == true);
            REQUIRE(req.get_uri_parameter("q") == "test");
            REQUIRE(req.has_uri_parameter("lang") == true);
            REQUIRE(req.get_uri_parameter("lang") == "en");
        }
    }
    
    SECTION("Factory methods") {
        auto req1 = http_request::create_http_request("GET", "http://example.com");
        REQUIRE(req1 != nullptr);
        REQUIRE(req1->get_method() == method::GET);
        REQUIRE(req1->get_host() == "example.com");
        
        auto req2 = http_request::create_http_request(method::POST, "https://api.example.com");
        REQUIRE(req2 != nullptr);
        REQUIRE(req2->get_method() == method::POST);
        REQUIRE(req2->is_ssl() == true);
    }
}

TEST_CASE("HTTP Request configuration", "[http][request][unit]") {
    http_request req;
    
    SECTION("Method configuration") {
        req.set_method("POST");
        REQUIRE(req.get_method() == method::POST);
        REQUIRE(req.get_method_string() == "POST");
        
        req.set_method(method::PUT);
        REQUIRE(req.get_method() == method::PUT);
        REQUIRE(req.get_method_string() == "PUT");
    }
    
    SECTION("Host and port configuration") {
        req.set_host("api.example.com");
        req.set_port("8080");
        REQUIRE(req.get_host() == "api.example.com");
        REQUIRE(req.get_port() == "8080");
        REQUIRE(req.is_default_port() == false);
        
        req.set_port("80");
        req.set_ssl(false);
        REQUIRE(req.is_default_port() == true);
        
        req.set_port("443");
        req.set_ssl(true);
        REQUIRE(req.is_default_port() == true);
    }
    
    SECTION("Content configuration") {
        REQUIRE(req.has_content() == false);
        
        req.set_content("test body");
        REQUIRE(req.has_content() == true);
        REQUIRE(req.get_body() == "test body");
        REQUIRE(req.has_header("Content-Length") == true);
        REQUIRE(req.get_header("Content-Length") == "9");
        
        req.set_content("{\"key\":\"value\"}", "application/json");
        REQUIRE(req.get_body() == "{\"key\":\"value\"}");
        REQUIRE(req.has_header("Content-Type") == true);
        REQUIRE(req.get_header("Content-Type") == "application/json");
        REQUIRE(req.has_header("Content-Length") == true);
        REQUIRE(req.get_header("Content-Length") == "15");
    }
    
    SECTION("URI parameters") {
        req.set_uri("/api/users");
        req.add_uri_parameter("page", "1");
        req.add_uri_parameter("limit", "10");
        req.add_uri_parameter("filter", "active");
        
        REQUIRE(req.has_uri_parameters() == true);
        REQUIRE(req.has_uri_parameter("page") == true);
        REQUIRE(req.get_uri_parameter("page") == "1");
        REQUIRE(req.get_uri_parameter("limit") == "10");
        
        // Test typed parameter getter
        REQUIRE(req.get_uri_parameter("page", 0) == 1);
        REQUIRE(req.get_uri_parameter("limit", 0) == 10);
        REQUIRE(req.get_uri_parameter("missing", 42) == 42);
        
        // Test query string generation
        std::string query = req.get_query_string();
        REQUIRE(query.find("page=1") != std::string::npos);
        REQUIRE(query.find("limit=10") != std::string::npos);
        REQUIRE(query.find("filter=active") != std::string::npos);
    }
    
    SECTION("Unix socket configuration") {
        req.set_unix_socket("/tmp/app.sock");
        REQUIRE(req.get_unix_socket() == "/tmp/app.sock");
    }
}

TEST_CASE("HTTP Request headers", "[http][request][unit]") {
    http_request req;
    
    SECTION("Header inheritance from headers class") {
        req.set_header("User-Agent", "TestClient/1.0");
        req.set_header("Accept", "application/json");
        
        REQUIRE(req.has_header("User-Agent") == true);
        REQUIRE(req.get_header("User-Agent") == "TestClient/1.0");
        REQUIRE(req.has_header("Accept") == true);
        REQUIRE(req.get_header("Accept") == "application/json");
    }
    
    SECTION("Special header processing") {
        // Host header should be processed specially
        req.process_header("Host", "example.com:8080");
        REQUIRE(req.get_host() == "example.com");
        REQUIRE(req.get_port() == "8080");
        
        // Content-Length should be handled
        req.process_header("Content-Length", "100");
        REQUIRE(req.get_header("Content-Length") == "100");
    }
    
    SECTION("Cookie handling") {
        auto& cookie_store = req.get_cookie_store();
        // Cookie store functionality would be tested in cookie_store_test.cpp
    }
}

TEST_CASE("HTTP Request serialization", "[http][request][unit]") {
    http_request req;
    req.set_method("GET");
    req.set_host("example.com");
    req.set_uri("/api/test");
    req.set_header("User-Agent", "TestClient");
    
    SECTION("Buffer generation") {
        std::vector<boost::asio::const_buffer> buffers;
        req.to_buffer(buffers);
        
        // Should have at least request line and headers
        REQUIRE(buffers.size() >= 2);
        
        // Convert buffers to string for verification
        std::string serialized;
        for (const auto& buffer : buffers) {
            serialized.append(static_cast<const char*>(buffer.data()), buffer.size());
        }
        
        REQUIRE(serialized.find("GET /api/test HTTP/1.1\r\n") != std::string::npos);
        REQUIRE(serialized.find("Host: example.com\r\n") != std::string::npos);
        REQUIRE(serialized.find("User-Agent: TestClient\r\n") != std::string::npos);
        REQUIRE(serialized.find("\r\n\r\n") != std::string::npos);
    }
    
    SECTION("Size calculation") {
        // get_size() returns the size of the content/payload
        size_t size = req.get_size();
        REQUIRE(size == 0); // No content yet
        
        // After setting content, size should reflect content length
        req.set_content("test body");
        size_t new_size = req.get_size();
        REQUIRE(new_size == 9); // "test body" is 9 bytes
        
        // Changing content updates size
        req.set_content("longer content here");
        REQUIRE(req.get_size() == 19);
    }
}

TEST_CASE("HTTP Request URI and Path handling", "[http][request][unit]") {
    http_request req;
    
    SECTION("URI vs Path distinction") {
        // Test with no query parameters
        req.set_uri("/api/users");
        REQUIRE(req.get_uri() == "/api/users");
        REQUIRE(req.get_path() == "/api/users");
        
        // Test with query parameters in URI
        req.set_uri("/api/users?page=1&limit=10");
        REQUIRE(req.get_uri() == "/api/users?page=1&limit=10");
        REQUIRE(req.get_path() == "/api/users");
        
        // Test with complex path
        req.set_uri("/api/v2/users/123/posts?sort=date&order=desc");
        REQUIRE(req.get_uri() == "/api/v2/users/123/posts?sort=date&order=desc");
        REQUIRE(req.get_path() == "/api/v2/users/123/posts");
        
        // Test with encoded characters in path
        req.set_uri("/search?q=hello%20world");
        REQUIRE(req.get_path() == "/search");
        REQUIRE(req.get_uri() == "/search?q=hello%20world");
        
        // Test with empty query
        req.set_uri("/test?");
        REQUIRE(req.get_path() == "/test");
        REQUIRE(req.get_uri() == "/test?");
        
        // Test root path
        req.set_uri("/");
        REQUIRE(req.get_path() == "/");
        REQUIRE(req.get_uri() == "/");
        
        // Test root with query
        req.set_uri("/?key=value");
        REQUIRE(req.get_path() == "/");
        REQUIRE(req.get_uri() == "/?key=value");
    }
    
    SECTION("Path extraction from URLs") {
        // Test URL parsing with query parameters
        REQUIRE(req.set_url("http://example.com/api/test?foo=bar&baz=qux") == true);
        REQUIRE(req.get_path() == "/api/test");
        REQUIRE(req.get_uri() == "/api/test?foo=bar&baz=qux");
        
        // Test HTTPS URL with port and query
        REQUIRE(req.set_url("https://api.example.com:8443/v1/resource?id=123") == true);
        REQUIRE(req.get_path() == "/v1/resource");
        REQUIRE(req.get_uri() == "/v1/resource?id=123");
        
        // Test URL without query
        REQUIRE(req.set_url("http://localhost:8080/status") == true);
        REQUIRE(req.get_path() == "/status");
        REQUIRE(req.get_uri() == "/status");
    }
}

TEST_CASE("HTTP Request URL building", "[http][request][unit]") {
    http_request req;
    
    SECTION("Basic URL construction") {
        req.set_protocol("https");
        req.set_host("api.example.com");
        req.set_port("443");
        req.set_uri("/v1/users");
        
        std::string url = req.get_url();
        REQUIRE(url == "https://api.example.com/v1/users");
    }
    
    SECTION("URL with non-default port") {
        req.set_protocol("http");
        req.set_host("localhost");
        req.set_port("8080");
        req.set_uri("/api");
        
        std::string url = req.get_url();
        REQUIRE(url == "http://localhost:8080/api");
    }
    
    SECTION("URL with query parameters") {
        req.set_protocol("https");
        req.set_host("search.example.com");
        req.set_port("443");
        req.set_uri("/search");
        req.add_uri_parameter("q", "test query");
        req.add_uri_parameter("page", "2");
        
        req.refresh_uri();
        std::string url = req.get_url();
        REQUIRE(url.find("https://search.example.com/search?") == 0);
        // URL encoding can use + or %20 for spaces
        bool has_query = (url.find("q=test+query") != std::string::npos) || 
                        (url.find("q=test%20query") != std::string::npos);
        REQUIRE(has_query);
        REQUIRE(url.find("page=2") != std::string::npos);
    }
}
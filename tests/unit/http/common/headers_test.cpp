#include <catch2/catch_test_macros.hpp>
#include <thinger/http.hpp>

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
}
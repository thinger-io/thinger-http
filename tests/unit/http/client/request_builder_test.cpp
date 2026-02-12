#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/client/request_builder.hpp>
#include <thinger/http/client/stream_types.hpp>

using namespace thinger::http;

TEST_CASE("Request Builder Basic", "[request_builder][unit]") {

    SECTION("request_builder can be created from client") {
        client c;
        // Move semantics - request() returns by value, movable
        request_builder builder = c.request("https://example.com/test");
        // Should compile and not crash
        REQUIRE(true);
    }

    SECTION("request_builder supports method chaining for headers") {
        client c;
        // Chain directly without storing intermediate reference
        c.request("https://example.com/test")
            .header("Authorization", "Bearer xxx")
            .header("X-Custom", "value");
        // Should compile and not crash
        REQUIRE(true);
    }

    SECTION("request_builder supports headers map") {
        client c;
        std::map<std::string, std::string> hdrs = {
            {"Authorization", "Bearer xxx"},
            {"Content-Type", "application/json"}
        };
        c.request("https://example.com/test")
            .headers(hdrs);
        // Should compile and not crash
        REQUIRE(true);
    }

    SECTION("request_builder supports body") {
        client c;
        c.request("https://example.com/test")
            .body(R"({"key": "value"})", "application/json");
        // Should compile and not crash
        REQUIRE(true);
    }

    SECTION("request_builder supports form body") {
        client c;
        form f;
        f.field("name", "value");

        c.request("https://example.com/test")
            .body(f);
        // Should compile and not crash
        REQUIRE(true);
    }
}

TEST_CASE("Stream Types", "[stream][unit]") {

    SECTION("stream_result default state") {
        stream_result result;
        REQUIRE(result.status_code == 0);
        REQUIRE(result.error.empty());
        REQUIRE(result.bytes_transferred == 0);
        REQUIRE_FALSE(result.ok());
        REQUIRE_FALSE(result.completed());
        REQUIRE_FALSE(static_cast<bool>(result));
    }

    SECTION("stream_result success state") {
        stream_result result;
        result.status_code = 200;

        REQUIRE(result.ok());
        REQUIRE(result.completed());
        REQUIRE(static_cast<bool>(result));
        REQUIRE_FALSE(result.has_network_error());
        REQUIRE_FALSE(result.has_http_error());
    }

    SECTION("stream_result HTTP error state") {
        stream_result result;
        result.status_code = 404;

        REQUIRE_FALSE(result.ok());
        REQUIRE(result.completed());
        REQUIRE_FALSE(static_cast<bool>(result));
        REQUIRE_FALSE(result.has_network_error());
        REQUIRE(result.has_http_error());
    }

    SECTION("stream_result network error state") {
        stream_result result;
        result.error = "Connection refused";

        REQUIRE_FALSE(result.ok());
        REQUIRE_FALSE(result.completed());
        REQUIRE_FALSE(static_cast<bool>(result));
        REQUIRE(result.has_network_error());
        REQUIRE_FALSE(result.has_http_error());
    }

    SECTION("stream_info structure") {
        stream_info info{"test data", 100, 1000, 200};

        REQUIRE(info.data == "test data");
        REQUIRE(info.downloaded == 100);
        REQUIRE(info.total == 1000);
        REQUIRE(info.status_code == 200);
    }
}

TEST_CASE("Stream Callback Types", "[stream][unit]") {

    SECTION("stream_callback can be created from lambda") {
        stream_callback callback = [](const stream_info& info) {
            return info.downloaded < info.total;
        };

        stream_info info{"data", 50, 100, 200};
        REQUIRE(callback(info) == true);

        stream_info info2{"data", 100, 100, 200};
        REQUIRE(callback(info2) == false);
    }

    SECTION("progress_callback can be created from lambda") {
        size_t last_downloaded = 0;
        size_t last_total = 0;

        progress_callback callback = [&](size_t downloaded, size_t total) {
            last_downloaded = downloaded;
            last_total = total;
        };

        callback(500, 1000);
        REQUIRE(last_downloaded == 500);
        REQUIRE(last_total == 1000);
    }
}

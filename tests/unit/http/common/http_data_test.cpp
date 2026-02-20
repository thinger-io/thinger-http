#include <catch2/catch_test_macros.hpp>
#include <thinger/http/common/http_data.hpp>
#include <thinger/http/data/out_string.hpp>

using namespace thinger::http;

TEST_CASE("HTTP Data operations", "[http][data][unit]") {

    SECTION("Default construction has zero size and null data") {
        http_data data;
        REQUIRE(data.get_size() == 0);
        REQUIRE(data.get_data() == nullptr);
    }

    SECTION("Default construction produces empty buffer") {
        http_data data;
        std::vector<boost::asio::const_buffer> buffers;
        data.to_buffer(buffers);
        REQUIRE(buffers.empty());
    }

    SECTION("Construction with out_string") {
        auto str = std::make_shared<data::out_string>("hello world");
        http_data data(str);

        REQUIRE(data.get_size() == 11);
        REQUIRE(data.get_data() == str);

        std::vector<boost::asio::const_buffer> buffers;
        data.to_buffer(buffers);
        REQUIRE(buffers.size() == 1);

        std::string result(static_cast<const char*>(buffers[0].data()), buffers[0].size());
        REQUIRE(result == "hello world");
    }

    SECTION("set_data and get_data") {
        http_data data;
        REQUIRE(data.get_data() == nullptr);

        auto str = std::make_shared<data::out_string>("test");
        data.set_data(str);
        REQUIRE(data.get_data() == str);
        REQUIRE(data.get_size() == 4);
    }

    SECTION("set_data with nullptr resets to empty") {
        auto str = std::make_shared<data::out_string>("initial");
        http_data data(str);
        REQUIRE(data.get_size() == 7);

        data.set_data(nullptr);
        REQUIRE(data.get_size() == 0);
        REQUIRE(data.get_data() == nullptr);
    }

    SECTION("Empty string data has zero size") {
        auto str = std::make_shared<data::out_string>("");
        http_data data(str);
        REQUIRE(data.get_size() == 0);
    }

    SECTION("Replace data updates size") {
        auto str1 = std::make_shared<data::out_string>("short");
        auto str2 = std::make_shared<data::out_string>("a longer string");
        http_data data(str1);
        REQUIRE(data.get_size() == 5);

        data.set_data(str2);
        REQUIRE(data.get_size() == 15);
    }
}

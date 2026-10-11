#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/file_server/file_info.hpp>
#include <thinger/http/common/http_request.hpp>
#include <thinger/http/util/http_date.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace thinger::http;

TEST_CASE("etag_matches", "[file_info][unit]") {
    const std::string etag = "W/\"1a-2b\"";

    SECTION("Weak comparison: W/ is ignored on both sides") {
        REQUIRE(etag_matches("W/\"1a-2b\"", etag));
        REQUIRE(etag_matches("\"1a-2b\"", etag));
        REQUIRE(etag_matches("W/\"1a-2b\"", "\"1a-2b\""));
    }

    SECTION("Any tag of a list") {
        REQUIRE(etag_matches("\"x\", W/\"1a-2b\"", etag));
        REQUIRE(etag_matches("\"x\",\"1a-2b\" ,\"y\"", etag));
        REQUIRE_FALSE(etag_matches("\"x\", W/\"y\"", etag));
    }

    SECTION("A wildcard matches any tag") {
        REQUIRE(etag_matches("*", etag));
        REQUIRE(etag_matches(" * ", etag));
    }

    SECTION("Commas inside a tag do not split it") {
        REQUIRE_FALSE(etag_matches("\"x,1a-2b\"", etag));
        REQUIRE(etag_matches("\"x,y\"", "\"x,y\""));
    }

    SECTION("Partial or malformed tags never match") {
        REQUIRE_FALSE(etag_matches("", etag));
        REQUIRE_FALSE(etag_matches("1a-2b", etag));
        REQUIRE_FALSE(etag_matches("\"1a-2b", etag));
        REQUIRE_FALSE(etag_matches("\"1a\"", etag));
        REQUIRE_FALSE(etag_matches("w/\"1a-2b\"", etag));
    }
}

TEST_CASE("parse_byte_range", "[file_info][unit]") {

    auto require_range = [](std::string_view header, uint64_t size, uint64_t first, uint64_t length) {
        auto range = parse_byte_range(header, size);
        INFO(header);
        REQUIRE(range);
        REQUIRE(range->first == first);
        REQUIRE(range->length == length);
    };

    SECTION("First and last positions") {
        require_range("bytes=0-0", 10, 0, 1);
        require_range("bytes=2-5", 10, 2, 4);
        require_range("bytes=0-9", 10, 0, 10);
        require_range("Bytes=0-9", 10, 0, 10);
    }

    SECTION("A last position past the end stops at the end") {
        require_range("bytes=5-100", 10, 5, 5);
        require_range("bytes=5-", 10, 5, 5);
    }

    SECTION("Suffix") {
        require_range("bytes=-3", 10, 7, 3);
        require_range("bytes=-100", 10, 0, 10);
    }

    SECTION("Unsatisfiable: length 0") {
        REQUIRE(parse_byte_range("bytes=10-", 10)->length == 0);
        REQUIRE(parse_byte_range("bytes=10-20", 10)->length == 0);
        REQUIRE(parse_byte_range("bytes=-0", 10)->length == 0);
        REQUIRE(parse_byte_range("bytes=0-", 0)->length == 0);
        REQUIRE(parse_byte_range("bytes=-5", 0)->length == 0);
    }

    SECTION("Ignored: other units, several ranges or invalid ones") {
        REQUIRE_FALSE(parse_byte_range("", 10));
        REQUIRE_FALSE(parse_byte_range("items=0-1", 10));
        REQUIRE_FALSE(parse_byte_range("bytes=0-1,4-5", 10));
        REQUIRE_FALSE(parse_byte_range("bytes=5-2", 10));
        REQUIRE_FALSE(parse_byte_range("bytes=-", 10));
        REQUIRE_FALSE(parse_byte_range("bytes=a-1", 10));
        REQUIRE_FALSE(parse_byte_range("bytes=0-1a", 10));
        REQUIRE_FALSE(parse_byte_range("bytes=+1-2", 10));
        REQUIRE_FALSE(parse_byte_range("bytes=99999999999999999999-", 10));
        REQUIRE_FALSE(parse_byte_range("bytes=1", 10));
    }
}

TEST_CASE("file_info", "[file_info][unit]") {
    namespace fs = std::filesystem;
    auto directory = fs::temp_directory_path() /
                     ("thinger_file_info_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    auto path = directory / "file.txt";
    std::ofstream(path) << "0123456789";

    SECTION("Metadata and validators of a regular file") {
        auto info = file_info::of(path);
        REQUIRE(info);
        REQUIRE(info->regular);
        REQUIRE(info->size == 10);
        REQUIRE(info->etag.starts_with("W/\"a-"));
        REQUIRE(info->etag.ends_with("\""));
        REQUIRE(info->last_modified == util::format_http_date(info->mtime));
    }

    SECTION("A directory is not a regular file") {
        auto info = file_info::of(directory);
        REQUIRE(info);
        REQUIRE_FALSE(info->regular);
    }

    SECTION("Missing file") {
        REQUIRE_FALSE(file_info::of(directory / "missing"));
    }

    SECTION("Another modification time is another entity tag") {
        auto before = file_info::of(path);
        fs::last_write_time(path, fs::last_write_time(path) + std::chrono::milliseconds(1500));
        auto after = file_info::of(path);
        REQUIRE(before->etag != after->etag);
        REQUIRE(after->mtime > before->mtime);
    }

    SECTION("Conditional requests") {
        auto info = *file_info::of(path);
        auto request = [](std::initializer_list<std::pair<std::string, std::string>> headers) {
            http_request result;
            for (const auto& [key, value] : headers) result.add_header(key, value);
            return result;
        };
        auto later = util::format_http_date(info.mtime + 60);
        auto earlier = util::format_http_date(info.mtime - 60);

        REQUIRE_FALSE(info.not_modified(request({})));
        REQUIRE(info.not_modified(request({{"If-None-Match", info.etag}})));
        REQUIRE(info.not_modified(request({{"If-None-Match", "\"other\""}, {"If-None-Match", info.etag}})));
        REQUIRE(info.not_modified(request({{"If-Modified-Since", info.last_modified}})));
        REQUIRE(info.not_modified(request({{"If-Modified-Since", later}})));
        REQUIRE_FALSE(info.not_modified(request({{"If-Modified-Since", earlier}})));
        REQUIRE_FALSE(info.not_modified(request({{"If-Modified-Since", "not a date"}})));
        // repeated If-Modified-Since: ignored
        REQUIRE_FALSE(info.not_modified(request({{"If-Modified-Since", later}, {"If-Modified-Since", later}})));
        // If-None-Match takes precedence
        REQUIRE_FALSE(info.not_modified(request({{"If-None-Match", "\"other\""}, {"If-Modified-Since", later}})));
        REQUIRE(info.not_modified(request({{"If-None-Match", info.etag}, {"If-Modified-Since", earlier}})));
    }

    fs::remove_all(directory);
}

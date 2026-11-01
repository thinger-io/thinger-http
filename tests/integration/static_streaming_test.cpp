#include "server_test_helpers.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/util/ascii.hpp>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>

using namespace thinger;
using namespace std::chrono_literals;

// Static files over 1 MiB, streamed in pieces of 64 KiB at the pace of the client instead of
// read whole (ranges included), and the 4 MiB cap of the compression on the fly

namespace {

    namespace fs = std::filesystem;
    using namespace server_test;

    constexpr size_t MiB = 1024 * 1024;

    struct raw_response {
        int status = 0;
        std::string head;
        std::string body;

        // Value of a header ("" if missing)
        std::string header(std::string_view name) const {
            size_t start = head.find("\r\n");
            while (start != std::string::npos) {
                start += 2;
                auto end = head.find("\r\n", start);
                auto line = std::string_view(head).substr(start, end == std::string::npos ? std::string::npos : end - start);
                auto colon = line.find(": ");
                if (colon != std::string_view::npos && util::ascii::iequals(line.substr(0, colon), name)) {
                    return std::string(line.substr(colon + 2));
                }
                start = end;
            }
            return {};
        }
    };

    // Send a request (closing the connection after it) and read its response, within 10 s
    raw_response get(uint16_t port, const std::string& target, const std::string& headers = "") {
        auto result = raw_exchange(port, "GET " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" +
                                         headers + "\r\n", 10s);
        REQUIRE(result.closed);
        auto end_headers = result.data.find("\r\n\r\n");
        REQUIRE(end_headers != std::string::npos);
        raw_response response;
        response.status = std::stoi(result.data.substr(9, 3));
        response.head = result.data.substr(0, end_headers);
        response.body = result.data.substr(end_headers + 4);
        return response;
    }

    // The same bytes as `expected`: compared by size and first difference (not printed whole)
    void require_bytes(const std::string& body, const std::string& expected) {
        REQUIRE(body.size() == expected.size());
        auto difference = std::mismatch(body.begin(), body.end(), expected.begin());
        REQUIRE(static_cast<size_t>(difference.first - body.begin()) == body.size());
    }

    std::string random_bytes(size_t size) {
        std::mt19937_64 random(size);
        std::string data(size, '\0');
        for (auto& byte : data) byte = static_cast<char>(random());
        return data;
    }

    struct streaming_site {
        fs::path root;
        http::server server;
        std::unique_ptr<running_server> running;
        uint16_t port = 0;

        streaming_site() {
            static std::atomic<int> sites{0};
            root = fs::temp_directory_path() /
                   ("thinger_static_streaming_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                    "_" + std::to_string(sites++));
            fs::create_directories(root);
        }

        ~streaming_site() {
            running.reset();
            std::error_code ec;
            fs::remove_all(root, ec);
        }

        void write(const std::string& name, const std::string& content) const {
            std::ofstream(root / name, std::ios::binary | std::ios::trunc) << content;
        }

        void start() {
            server.serve_static("/static", root.string());
            running = std::make_unique<running_server>(server);
            port = running->port;
        }
    };

}

TEST_CASE("Static files over 1 MiB are streamed whole and by range", "[server][static][integration]") {
    streaming_site site;
    auto binary = random_bytes(3 * MiB + 123);
    site.write("data.bin", binary);
    site.start();

    SECTION("The whole file") {
        auto response = get(site.port, "/static/data.bin");
        REQUIRE(response.status == 200);
        REQUIRE(response.header("Content-Length") == std::to_string(binary.size()));
        REQUIRE(response.header("Accept-Ranges") == "bytes");
        REQUIRE_FALSE(response.header("ETag").empty());
        REQUIRE(response.header("Cache-Control") == "no-cache");
        require_bytes(response.body, binary);
    }

    SECTION("A range longer than 1 MiB") {
        auto response = get(site.port, "/static/data.bin", "Range: bytes=100-2097251\r\n");
        REQUIRE(response.status == 206);
        REQUIRE(response.header("Content-Range") == "bytes 100-2097251/" + std::to_string(binary.size()));
        require_bytes(response.body, binary.substr(100, 2 * MiB));
    }
}

TEST_CASE("Text files are compressed on the fly up to 4 MiB", "[server][static][integration]") {
    streaming_site site;
    std::string text(2 * MiB, 'a');
    std::string large(5 * MiB, 'b');
    site.write("text.txt", text);
    site.write("large.txt", large);
    site.start();

    SECTION("Under the cap, compressed whole for a client accepting gzip") {
        auto response = get(site.port, "/static/text.txt", "Accept-Encoding: gzip\r\n");
        REQUIRE(response.status == 200);
        REQUIRE(response.header("Content-Encoding") == "gzip");
        REQUIRE(response.header("Vary") == "Accept-Encoding");
        REQUIRE(response.body.size() < text.size());
    }

    SECTION("Under the cap, streamed as it is for a client accepting no coding") {
        auto response = get(site.port, "/static/text.txt");
        REQUIRE(response.status == 200);
        REQUIRE(response.header("Content-Encoding").empty());
        REQUIRE(response.header("Vary") == "Accept-Encoding");
        require_bytes(response.body, text);
    }

    SECTION("Over the cap, streamed as it is for every client") {
        auto response = get(site.port, "/static/large.txt", "Accept-Encoding: gzip\r\n");
        REQUIRE(response.status == 200);
        REQUIRE(response.header("Content-Encoding").empty());
        REQUIRE(response.header("Vary").empty());
        require_bytes(response.body, large);
    }
}

TEST_CASE("The compression cap decides Vary alike on 200, 206 and 304", "[server][static][integration]") {
    streaming_site site;
    std::string text(2 * MiB, 'a');
    std::string large(5 * MiB, 'b');
    site.write("text.txt", text);
    site.write("large.txt", large);
    site.start();

    SECTION("Under the cap: compressed with deflate too, and Vary on every answer") {
        auto deflated = get(site.port, "/static/text.txt", "Accept-Encoding: deflate\r\n");
        REQUIRE(deflated.status == 200);
        REQUIRE(deflated.header("Content-Encoding") == "deflate");
        REQUIRE(deflated.header("Vary") == "Accept-Encoding");

        auto range = get(site.port, "/static/text.txt", "Accept-Encoding: gzip\r\nRange: bytes=0-1048676\r\n");
        REQUIRE(range.status == 206);
        REQUIRE(range.header("Content-Encoding").empty());
        REQUIRE(range.header("Vary") == "Accept-Encoding");
        require_bytes(range.body, text.substr(0, 1048677));

        auto revalidated = get(site.port, "/static/text.txt", "If-None-Match: " + deflated.header("ETag") + "\r\n");
        REQUIRE(revalidated.status == 304);
        REQUIRE(revalidated.header("Vary") == "Accept-Encoding");
    }

    SECTION("Over the cap: as it is for every client, and no Vary on any answer") {
        for (auto accept : {"", "Accept-Encoding: gzip\r\n", "Accept-Encoding: deflate\r\n", "Accept-Encoding: br\r\n",
                            "Accept-Encoding: *\r\n"}) {
            INFO(accept);
            auto response = get(site.port, "/static/large.txt", accept);
            REQUIRE(response.status == 200);
            REQUIRE(response.header("Content-Encoding").empty());
            REQUIRE(response.header("Vary").empty());
            REQUIRE(response.header("Content-Length") == std::to_string(large.size()));
            require_bytes(response.body, large);
        }

        auto range = get(site.port, "/static/large.txt", "Accept-Encoding: gzip\r\nRange: bytes=1000-2049575\r\n");
        REQUIRE(range.status == 206);
        REQUIRE(range.header("Vary").empty());
        require_bytes(range.body, large.substr(1000, 2048576));

        auto revalidated = get(site.port, "/static/large.txt",
                               "Accept-Encoding: gzip\r\nIf-None-Match: " + range.header("ETag") + "\r\n");
        REQUIRE(revalidated.status == 304);
        REQUIRE(revalidated.header("Vary").empty());
    }
}

TEST_CASE("A request dispatched in memory reads a file over 1 MiB whole", "[server][static][integration]") {
    streaming_site site;
    auto binary = random_bytes(2 * MiB);
    site.write("data.bin", binary);
    site.server.serve_static("/static", site.root.string());
    auto response = run_dispatch(site.server, make_request(http::method::GET, "/static/data.bin"));
    REQUIRE(response);
    REQUIRE(response->get_status_code() == 200);
    require_bytes(response->get_content(), binary);
}

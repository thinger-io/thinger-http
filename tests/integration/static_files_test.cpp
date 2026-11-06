// First, as it must compile by itself: it is public, and all a handler adds to serve files
#include <thinger/http/server/file_server/static_files.hpp>
#include "server_test_helpers.hpp"
#include "shutdown_test_helpers.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/file_server/file_info.hpp>
#include <thinger/http/server/openapi.hpp>
#include <thinger/http/util/http_date.hpp>
#include <thinger/util/ascii.hpp>
#include <thinger/util/compression.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace thinger;
using namespace std::chrono_literals;

// Static files (serve_static and send_file) over raw TCP: validators (ETag, Last-Modified),
// conditional requests answered with 304 Not Modified, HEAD, byte ranges, the SPA fallback
// and the containment of the paths in the served directory

namespace {

    namespace fs = std::filesystem;
    using namespace server_test;

    struct raw_response {
        int status = 0;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;

        // Value of a header ("" if missing), and whether it is present
        std::string header(std::string_view name) const {
            for (const auto& [key, value] : headers) {
                if (util::ascii::iequals(key, name)) return value;
            }
            return {};
        }
        bool has_header(std::string_view name) const {
            return std::any_of(headers.begin(), headers.end(),
                               [&](const auto& header) { return util::ascii::iequals(header.first, name); });
        }
    };

    // Send a request (closing the connection after it) and parse its response
    raw_response send_request(uint16_t port, const std::string& method, const std::string& target,
                              const std::string& headers = "") {
        auto result = raw_exchange(port, method + " " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" +
                                         headers + "\r\n");
        INFO(result.data);
        REQUIRE(result.closed);
        auto end_headers = result.data.find("\r\n\r\n");
        REQUIRE(end_headers != std::string::npos);

        raw_response response;
        response.status = std::stoi(result.data.substr(9, 3));
        response.body = result.data.substr(end_headers + 4);
        auto head = result.data.substr(0, end_headers);
        size_t line_start = head.find("\r\n");
        while (line_start != std::string::npos) {
            line_start += 2;
            auto line_end = head.find("\r\n", line_start);
            auto line = head.substr(line_start, line_end == std::string::npos ? std::string::npos : line_end - line_start);
            auto colon = line.find(": ");
            if (colon != std::string::npos) response.headers.emplace_back(line.substr(0, colon), line.substr(colon + 2));
            line_start = line_end;
        }
        return response;
    }

    raw_response get(uint16_t port, const std::string& target, const std::string& headers = "") {
        return send_request(port, "GET", target, headers);
    }

    void write_file(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
    }

    // A 304: no body, nor the length of the file, but its validators
    void require_not_modified(const raw_response& response, const http::file_info& file) {
        REQUIRE(response.status == 304);
        REQUIRE(response.body.empty());
        REQUIRE_FALSE(response.has_header("Content-Length"));
        REQUIRE(response.header("ETag") == file.etag);
        REQUIRE(response.header("Last-Modified") == file.last_modified);
    }

    // A served folder (www) next to a sibling sharing its name as a prefix (www-secret), on a
    // server started by start() once its routes are registered
    struct static_site {
        fs::path base;
        fs::path root;
        http::server server;
        std::unique_ptr<running_server> running;
        uint16_t port = 0;

        static_site() {
            static std::atomic<int> sites{0};
            base = fs::temp_directory_path() /
                   ("thinger_static_files_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                    "_" + std::to_string(sites++));
            root = base / "www";
            write_file(root / "index.html", "<html>index</html>");
            write_file(root / "app.js", "console.log('app');");
            write_file(root / "docs" / "index.html", "<html>docs</html>");
            write_file(root / "digits.txt", "0123456789");
            write_file(root / "big.js", std::string(4096, 'a'));
            write_file(base / "www-secret" / "secret.txt", "top secret");
        }

        ~static_site() {
            running.reset();
            std::error_code ec;
            fs::remove_all(base, ec);
        }

        void start() {
            running = std::make_unique<running_server>(server);
            port = running->port;
        }

        http::file_info info(const std::string& name) const {
            auto file = http::file_info::of(root / name);
            REQUIRE(file);
            return *file;
        }
    };

}

TEST_CASE("Static files carry validators and answer conditional requests with 304", "[server][static][integration]") {
    static_site site;
    site.server.serve_static("/static", site.root.string());
    site.start();
    auto file = site.info("app.js");

    SECTION("First GET: ETag, Last-Modified and Cache-Control: no-cache") {
        auto response = get(site.port, "/static/app.js");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "console.log('app');");
        REQUIRE(response.header("ETag") == file.etag);
        REQUIRE(response.header("ETag").starts_with("W/\""));
        REQUIRE(response.header("Last-Modified") == file.last_modified);
        REQUIRE(http::util::parse_http_date(response.header("Last-Modified")) == file.mtime);
        REQUIRE(response.header("Cache-Control") == "no-cache");
        REQUIRE(response.header("Accept-Ranges") == "bytes");
        REQUIRE(response.header("Content-Length") == std::to_string(response.body.size()));
    }

    SECTION("If-None-Match with the entity tag: 304 without body") {
        auto response = get(site.port, "/static/app.js", "If-None-Match: " + file.etag + "\r\n");
        require_not_modified(response, file);
        REQUIRE(response.header("Cache-Control") == "no-cache");
    }

    SECTION("If-None-Match compares weakly: the tag without W/ matches") {
        auto strong = file.etag.substr(2);
        require_not_modified(get(site.port, "/static/app.js", "If-None-Match: " + strong + "\r\n"), file);
    }

    SECTION("If-None-Match: * matches") {
        require_not_modified(get(site.port, "/static/app.js", "If-None-Match: *\r\n"), file);
    }

    SECTION("If-None-Match listing the tag among others matches") {
        auto list = "If-None-Match: \"other\", " + file.etag + ", W/\"another\"\r\n";
        require_not_modified(get(site.port, "/static/app.js", list), file);
    }

    SECTION("If-None-Match without the tag: 200") {
        auto response = get(site.port, "/static/app.js", "If-None-Match: \"other\", W/\"another\"\r\n");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "console.log('app');");
        REQUIRE(response.header("ETag") == file.etag);
    }

    SECTION("A modified file gets a new entity tag: 200 for the old one") {
        write_file(site.root / "app.js", "console.log('app v2');");
        fs::last_write_time(site.root / "app.js", fs::last_write_time(site.root / "app.js") + 2s);
        auto modified = site.info("app.js");
        REQUIRE(modified.etag != file.etag);

        auto response = get(site.port, "/static/app.js", "If-None-Match: " + file.etag + "\r\n");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "console.log('app v2');");
        REQUIRE(response.header("ETag") == modified.etag);
        REQUIRE(response.header("Last-Modified") == modified.last_modified);

        auto since = get(site.port, "/static/app.js", "If-Modified-Since: " + file.last_modified + "\r\n");
        REQUIRE(since.status == 200);

        require_not_modified(get(site.port, "/static/app.js", "If-None-Match: " + modified.etag + "\r\n"), modified);
    }

    SECTION("A file of the same size modified within the same second gets a new entity tag") {
        write_file(site.root / "app.js", "console.log('ppa');");
        fs::last_write_time(site.root / "app.js", fs::last_write_time(site.root / "app.js") + 1ms);
        auto modified = site.info("app.js");
        REQUIRE(modified.size == file.size);
        REQUIRE(modified.etag != file.etag);
        REQUIRE(get(site.port, "/static/app.js", "If-None-Match: " + file.etag + "\r\n").status == 200);
    }

    SECTION("If-Modified-Since equal to or later than Last-Modified: 304") {
        require_not_modified(get(site.port, "/static/app.js", "If-Modified-Since: " + file.last_modified + "\r\n"), file);
        auto later = http::util::format_http_date(file.mtime + 3600);
        require_not_modified(get(site.port, "/static/app.js", "If-Modified-Since: " + later + "\r\n"), file);
    }

    SECTION("If-Modified-Since earlier than Last-Modified: 200") {
        auto earlier = http::util::format_http_date(file.mtime - 1);
        auto response = get(site.port, "/static/app.js", "If-Modified-Since: " + earlier + "\r\n");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "console.log('app');");
    }

    SECTION("Invalid If-Modified-Since is ignored: 200") {
        auto response = get(site.port, "/static/app.js", "If-Modified-Since: yesterday\r\n");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "console.log('app');");
    }

    SECTION("If-None-Match takes precedence over If-Modified-Since") {
        auto later = http::util::format_http_date(file.mtime + 3600);
        auto earlier = http::util::format_http_date(file.mtime - 3600);

        auto stale_tag = get(site.port, "/static/app.js",
                             "If-None-Match: \"other\"\r\nIf-Modified-Since: " + later + "\r\n");
        REQUIRE(stale_tag.status == 200);
        REQUIRE(stale_tag.body == "console.log('app');");

        auto current_tag = get(site.port, "/static/app.js",
                               "If-None-Match: " + file.etag + "\r\nIf-Modified-Since: " + earlier + "\r\n");
        require_not_modified(current_tag, file);
    }

    SECTION("HEAD: the headers of GET with the length of the file, without body") {
        auto response = send_request(site.port, "HEAD", "/static/app.js");
        REQUIRE(response.status == 200);
        REQUIRE(response.body.empty());
        REQUIRE(response.header("Content-Length") == std::to_string(file.size));
        REQUIRE(response.header("ETag") == file.etag);
        REQUIRE(response.header("Last-Modified") == file.last_modified);
        REQUIRE(response.header("Cache-Control") == "no-cache");
        REQUIRE(response.header("Content-Type").starts_with("application/javascript"));
    }

    SECTION("HEAD answers conditional requests the same") {
        require_not_modified(send_request(site.port, "HEAD", "/static/app.js", "If-None-Match: " + file.etag + "\r\n"), file);
        require_not_modified(send_request(site.port, "HEAD", "/static/app.js", "If-Modified-Since: " + file.last_modified + "\r\n"), file);
        REQUIRE(send_request(site.port, "HEAD", "/static/app.js", "If-None-Match: \"other\"\r\n").status == 200);
    }

    SECTION("Conditional requests apply to the file sent compressed, which varies by Accept-Encoding") {
        auto big = site.info("big.js");
        auto response = get(site.port, "/static/big.js", "Accept-Encoding: gzip\r\n");
        REQUIRE(response.status == 200);
        REQUIRE(response.header("Content-Encoding") == "gzip");
        REQUIRE(response.header("Vary") == "Accept-Encoding");
        REQUIRE(response.header("ETag") == big.etag);

        auto revalidated = get(site.port, "/static/big.js", "Accept-Encoding: gzip\r\nIf-None-Match: " + big.etag + "\r\n");
        require_not_modified(revalidated, big);
        REQUIRE(revalidated.header("Vary") == "Accept-Encoding");
    }
}

TEST_CASE("Static files keep the SPA fallback and stay inside their directory", "[server][static][integration]") {
    static_site site;
    site.server.serve_static("/static", site.root.string());
    site.start();
    auto index = site.info("index.html");

    SECTION("The fallback file is served, with its validators, for paths without a file") {
        auto response = get(site.port, "/static/app/dashboard");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "<html>index</html>");
        REQUIRE(response.header("ETag") == index.etag);
        require_not_modified(get(site.port, "/static/app/dashboard", "If-None-Match: " + index.etag + "\r\n"), index);
    }

    SECTION("The fallback file is served for the root") {
        auto response = get(site.port, "/static/");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "<html>index</html>");
        require_not_modified(get(site.port, "/static/", "If-None-Match: " + index.etag + "\r\n"), index);
        REQUIRE(send_request(site.port, "HEAD", "/static/").header("ETag") == index.etag);
    }

    SECTION("The fallback file of a directory is served for it") {
        auto docs = site.info("docs/index.html");
        auto response = get(site.port, "/static/docs");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "<html>docs</html>");
        REQUIRE(response.header("ETag") == docs.etag);
    }

    SECTION("A symlink to a sibling directory sharing the name as a prefix is not served") {
        // (it resolves to ".../www-secret/secret.txt", which starts with ".../www")
        fs::create_symlink(site.base / "www-secret" / "secret.txt", site.root / "link.txt");
        auto response = get(site.port, "/static/link.txt");
        REQUIRE(response.status == 403);
        REQUIRE(response.body.find("top secret") == std::string::npos);
        REQUIRE(send_request(site.port, "HEAD", "/static/link.txt").status == 403);

        fs::create_directory_symlink(site.base / "www-secret", site.root / "linked");
        REQUIRE(get(site.port, "/static/linked/secret.txt").status == 403);
    }

    SECTION("A symlink to a file inside the directory is served") {
        fs::create_symlink(site.root / "app.js", site.root / "link.js");
        auto response = get(site.port, "/static/link.js");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "console.log('app');");
    }

    SECTION("Paths with dot segments are rejected") {
        // (by the request parser, before any route)
        auto sibling = get(site.port, "/static/../www-secret/secret.txt");
        REQUIRE(sibling.status == 400);
        REQUIRE(sibling.body.find("top secret") == std::string::npos);
        REQUIRE(get(site.port, "/static/docs/../../www-secret/secret.txt").status == 400);
        REQUIRE(get(site.port, "/static/%2E%2E/www-secret/secret.txt").status == 400);
    }

    SECTION("Names with two dots that are not a dot segment are served") {
        write_file(site.root / "v1..v2.txt", "diff");
        auto response = get(site.port, "/static/v1..v2.txt");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "diff");
    }
}

TEST_CASE("Static files with a Cache-Control of their own", "[server][static][integration]") {
    static_site site;
    const std::string immutable = "public, max-age=31536000, immutable";
    site.server.serve_static("/assets", site.root.string(), {.fallback = "", .cache_control = immutable});
    site.start();
    auto file = site.info("app.js");

    SECTION("200 and 304 carry it") {
        auto response = get(site.port, "/assets/app.js");
        REQUIRE(response.status == 200);
        REQUIRE(response.header("Cache-Control") == immutable);
        REQUIRE(response.header("ETag") == file.etag);

        auto revalidated = get(site.port, "/assets/app.js", "If-None-Match: " + file.etag + "\r\n");
        require_not_modified(revalidated, file);
        REQUIRE(revalidated.header("Cache-Control") == immutable);
    }

    SECTION("Without fallback, missing files are 404") {
        auto response = get(site.port, "/assets/missing.js");
        REQUIRE(response.status == 404);
        REQUIRE_FALSE(response.has_header("ETag"));
        REQUIRE(get(site.port, "/assets/").status == 404);
    }
}

namespace {

    // A precompressed sibling of a file of the site (its content is not checked: any bytes
    // do), modified `offset` after the file
    void write_sibling(const static_site& site, const std::string& name, const std::string& content,
                       fs::file_time_type::duration offset = 1s) {
        write_file(site.root / name, content);
        auto original = site.root / name.substr(0, name.rfind('.'));
        fs::last_write_time(site.root / name, fs::last_write_time(original) + offset);
    }

    // The entity tag of a precompressed sibling: its own, telling the coding
    std::string sibling_etag(const http::file_info& sibling, const std::string& coding) {
        auto etag = sibling.etag;
        etag.insert(etag.size() - 1, "-" + coding);
        return etag;
    }

}

TEST_CASE("Static files are sent as their precompressed siblings", "[server][static][compression][integration]") {
    static_site site;
    site.server.serve_static("/static", site.root.string());
    site.server.serve_static("/plain", site.root.string(), {.precompressed = false});
    site.start();
    write_sibling(site, "app.js.br", "BROTLI app.js");
    write_sibling(site, "app.js.gz", "GZIP app.js");
    auto br = site.info("app.js.br");
    auto gz = site.info("app.js.gz");
    auto file = site.info("app.js");

    auto require_sibling = [](const raw_response& response, const http::file_info& sibling,
                              const std::string& coding, const std::string& body) {
        REQUIRE(response.status == 200);
        REQUIRE(response.body == body);
        REQUIRE(response.header("Content-Encoding") == coding);
        REQUIRE(response.header("Content-Type").starts_with("application/javascript"));
        REQUIRE(response.header("Content-Length") == std::to_string(body.size()));
        REQUIRE(response.header("Vary") == "Accept-Encoding");
        REQUIRE(response.header("ETag") == sibling_etag(sibling, coding));
        REQUIRE(response.header("Last-Modified") == sibling.last_modified);
        REQUIRE(response.header("Cache-Control") == "no-cache");
    };
    auto require_original = [&](const raw_response& response) {
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "console.log('app');");
        REQUIRE_FALSE(response.has_header("Content-Encoding"));
        REQUIRE(response.header("ETag") == file.etag);
    };

    SECTION("br first at equal quality, else the coding of higher quality") {
        require_sibling(get(site.port, "/static/app.js", "Accept-Encoding: gzip, deflate, br\r\n"), br, "br", "BROTLI app.js");
        require_sibling(get(site.port, "/static/app.js", "Accept-Encoding: GZIP\r\n"), gz, "gzip", "GZIP app.js");
        require_sibling(get(site.port, "/static/app.js", "Accept-Encoding: x-gzip\r\n"), gz, "gzip", "GZIP app.js");
        require_sibling(get(site.port, "/static/app.js", "Accept-Encoding: br;q=0.5, gzip\r\n"), gz, "gzip", "GZIP app.js");
        require_sibling(get(site.port, "/static/app.js", "Accept-Encoding: gzip;q=0.5, br;q=0.8\r\n"), br, "br", "BROTLI app.js");
        require_sibling(get(site.port, "/static/app.js", "Accept-Encoding: *\r\n"), br, "br", "BROTLI app.js");
        require_sibling(get(site.port, "/static/app.js", "Accept-Encoding: br;q=0, *\r\n"), gz, "gzip", "GZIP app.js");
        // (Accept-Encoding headers make one list)
        require_sibling(get(site.port, "/static/app.js", "Accept-Encoding: deflate\r\nAccept-Encoding: br\r\n"),
                        br, "br", "BROTLI app.js");
    }

    SECTION("Without Accept-Encoding, or accepting neither br nor gzip, the file") {
        require_original(get(site.port, "/static/app.js"));
        require_original(get(site.port, "/static/app.js", "Accept-Encoding: deflate\r\n"));
        require_original(get(site.port, "/static/app.js", "Accept-Encoding: identity\r\n"));
        require_original(get(site.port, "/static/app.js", "Accept-Encoding: br;q=0, gzip;q=0\r\n"));
    }

    SECTION("Identity preferred over the codings, or not acceptable") {
        require_original(get(site.port, "/static/app.js", "Accept-Encoding: gzip;q=0.5, br;q=0.5, identity\r\n"));
        require_sibling(get(site.port, "/static/app.js", "Accept-Encoding: identity;q=0, gzip\r\n"), gz, "gzip", "GZIP app.js");
        require_sibling(get(site.port, "/static/app.js", "Accept-Encoding: gzip;q=0.5, identity;q=0.5\r\n"), gz, "gzip", "GZIP app.js");
    }

    SECTION("A missing sibling: the next accepted one") {
        fs::remove(site.root / "app.js.br");
        require_sibling(get(site.port, "/static/app.js", "Accept-Encoding: br, gzip\r\n"), gz, "gzip", "GZIP app.js");
        fs::remove(site.root / "app.js.gz");
        require_original(get(site.port, "/static/app.js", "Accept-Encoding: br, gzip\r\n"));
    }

    SECTION("A sibling older than the file is not sent") {
        write_sibling(site, "app.js.br", "STALE", -2s);
        require_sibling(get(site.port, "/static/app.js", "Accept-Encoding: br, gzip\r\n"), gz, "gzip", "GZIP app.js");
        write_sibling(site, "app.js.gz", "STALE", -1ns);
        require_original(get(site.port, "/static/app.js", "Accept-Encoding: br, gzip\r\n"));
        // (the same time is not older)
        write_sibling(site, "app.js.gz", "SAME", 0s);
        auto same = get(site.port, "/static/app.js", "Accept-Encoding: br, gzip\r\n");
        REQUIRE(same.header("Content-Encoding") == "gzip");
        REQUIRE(same.body == "SAME");
    }

    SECTION("A sibling that is not a regular file is not sent") {
        fs::remove(site.root / "app.js.br");
        fs::create_directory(site.root / "app.js.br");
        require_sibling(get(site.port, "/static/app.js", "Accept-Encoding: br, gzip\r\n"), gz, "gzip", "GZIP app.js");
    }

    SECTION("HEAD: the headers of GET, with the length of the sibling") {
        auto response = send_request(site.port, "HEAD", "/static/app.js", "Accept-Encoding: br\r\n");
        REQUIRE(response.status == 200);
        REQUIRE(response.body.empty());
        REQUIRE(response.header("Content-Encoding") == "br");
        REQUIRE(response.header("Content-Length") == std::to_string(br.size));
        REQUIRE(response.header("ETag") == sibling_etag(br, "br"));
        REQUIRE(response.header("Vary") == "Accept-Encoding");
    }

    SECTION("Conditional requests: 304 with the entity tag of each representation") {
        auto revalidated = get(site.port, "/static/app.js", "Accept-Encoding: br\r\nIf-None-Match: " + sibling_etag(br, "br") + "\r\n");
        REQUIRE(revalidated.status == 304);
        REQUIRE(revalidated.body.empty());
        REQUIRE(revalidated.header("ETag") == sibling_etag(br, "br"));
        REQUIRE(revalidated.header("Vary") == "Accept-Encoding");
        REQUIRE_FALSE(revalidated.has_header("Content-Length"));

        // The tag of another representation does not match
        auto other = get(site.port, "/static/app.js", "Accept-Encoding: br\r\nIf-None-Match: " + file.etag + "\r\n");
        require_sibling(other, br, "br", "BROTLI app.js");
        auto identity = get(site.port, "/static/app.js", "If-None-Match: " + sibling_etag(br, "br") + "\r\n");
        require_original(identity);
        REQUIRE(get(site.port, "/static/app.js", "If-None-Match: " + file.etag + "\r\n").status == 304);

        auto since = get(site.port, "/static/app.js", "Accept-Encoding: gzip\r\nIf-Modified-Since: " + gz.last_modified + "\r\n");
        REQUIRE(since.status == 304);
        REQUIRE(since.header("ETag") == sibling_etag(gz, "gzip"));
    }

    SECTION("A range is one of the bytes of the sibling, as sent") {
        auto response = get(site.port, "/static/app.js", "Accept-Encoding: br\r\nRange: bytes=0-5\r\n");
        REQUIRE(response.status == 206);
        REQUIRE(response.body == "BROTLI");
        REQUIRE(response.header("Content-Encoding") == "br");
        REQUIRE(response.header("Content-Range") == "bytes 0-5/" + std::to_string(br.size));
        REQUIRE(response.header("Vary") == "Accept-Encoding");

        auto past = get(site.port, "/static/app.js", "Accept-Encoding: br\r\nRange: bytes=1000-\r\n");
        REQUIRE(past.status == 416);
        REQUIRE(past.header("Content-Range") == "bytes */" + std::to_string(br.size));

        auto if_range = get(site.port, "/static/app.js", "Accept-Encoding: br\r\nRange: bytes=0-5\r\nIf-Range: " +
                                                         br.last_modified + "\r\n");
        REQUIRE(if_range.status == 206);
    }

    SECTION("The siblings themselves are files as any other") {
        auto response = get(site.port, "/static/app.js.br", "Accept-Encoding: br, gzip\r\n");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "BROTLI app.js");
        REQUIRE_FALSE(response.has_header("Content-Encoding"));
        REQUIRE(response.header("ETag") == br.etag);
    }

    SECTION("The fallback file too") {
        write_sibling(site, "index.html.br", "BROTLI index");
        auto response = get(site.port, "/static/app/dashboard", "Accept-Encoding: br\r\n");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "BROTLI index");
        REQUIRE(response.header("Content-Encoding") == "br");
        REQUIRE(response.header("Content-Type").starts_with("text/html"));
        REQUIRE(get(site.port, "/static/", "Accept-Encoding: br\r\n").body == "BROTLI index");
    }

    SECTION("A symlinked sibling only while it resolves inside the directory") {
        fs::remove(site.root / "app.js.br");
        fs::remove(site.root / "app.js.gz");
        write_file(site.base / "www-secret" / "app.js.br", "SECRET");
        fs::last_write_time(site.base / "www-secret" / "app.js.br", fs::last_write_time(site.root / "app.js") + 1s);
        fs::create_symlink(site.base / "www-secret" / "app.js.br", site.root / "app.js.br");
        auto outside = get(site.port, "/static/app.js", "Accept-Encoding: br\r\n");
        require_original(outside);
        REQUIRE(outside.body.find("SECRET") == std::string::npos);

        write_file(site.root / "compressed" / "app.js.gz", "LINKED GZIP");
        fs::last_write_time(site.root / "compressed" / "app.js.gz", fs::last_write_time(site.root / "app.js") + 1s);
        fs::create_symlink(site.root / "compressed" / "app.js.gz", site.root / "app.js.gz");
        auto inside = get(site.port, "/static/app.js", "Accept-Encoding: gzip\r\n");
        REQUIRE(inside.body == "LINKED GZIP");
        REQUIRE(inside.header("Content-Encoding") == "gzip");
    }

    SECTION("Disabled: the file, compressed on the fly as usual") {
        require_original(get(site.port, "/plain/app.js", "Accept-Encoding: br, gzip\r\n"));
        write_sibling(site, "big.js.gz", "GZIP big.js");
        auto big = get(site.port, "/plain/big.js", "Accept-Encoding: gzip\r\n");
        REQUIRE(big.header("Content-Encoding") == "gzip");
        REQUIRE(util::gzip::decompress(big.body) == std::string(4096, 'a'));
    }

    SECTION("A text file without sibling is still compressed on the fly") {
        auto big = get(site.port, "/static/big.js", "Accept-Encoding: br, gzip\r\n");
        REQUIRE(big.header("Content-Encoding") == "gzip");
        REQUIRE(util::gzip::decompress(big.body) == std::string(4096, 'a'));
        REQUIRE(big.header("ETag") == site.info("big.js").etag);
    }
}

TEST_CASE("Static files answer single byte ranges", "[server][static][integration]") {
    static_site site;
    site.server.serve_static("/static", site.root.string());
    site.start();
    auto digits = site.info("digits.txt");

    SECTION("First and last positions: 206 with Content-Range") {
        auto response = get(site.port, "/static/digits.txt", "Range: bytes=2-5\r\n");
        REQUIRE(response.status == 206);
        REQUIRE(response.body == "2345");
        REQUIRE(response.header("Content-Range") == "bytes 2-5/10");
        REQUIRE(response.header("Content-Length") == "4");
        REQUIRE(response.header("ETag") == digits.etag);
    }

    SECTION("Open range and suffix") {
        auto open = get(site.port, "/static/digits.txt", "Range: bytes=7-\r\n");
        REQUIRE(open.status == 206);
        REQUIRE(open.body == "789");
        REQUIRE(open.header("Content-Range") == "bytes 7-9/10");

        auto suffix = get(site.port, "/static/digits.txt", "Range: bytes=-3\r\n");
        REQUIRE(suffix.status == 206);
        REQUIRE(suffix.body == "789");
        REQUIRE(suffix.header("Content-Range") == "bytes 7-9/10");
    }

    SECTION("A range starting past the end: 416 with the length") {
        auto response = get(site.port, "/static/digits.txt", "Range: bytes=10-\r\n");
        REQUIRE(response.status == 416);
        REQUIRE(response.header("Content-Range") == "bytes */10");
    }

    SECTION("Several ranges, or an invalid one: the whole file") {
        auto several = get(site.port, "/static/digits.txt", "Range: bytes=0-1,4-5\r\n");
        REQUIRE(several.status == 200);
        REQUIRE(several.body == "0123456789");
        REQUIRE(get(site.port, "/static/digits.txt", "Range: bytes=5-2\r\n").status == 200);
    }

    SECTION("If-Range: the range only while the file has that date") {
        auto same = get(site.port, "/static/digits.txt", "Range: bytes=0-1\r\nIf-Range: " + digits.last_modified + "\r\n");
        REQUIRE(same.status == 206);
        REQUIRE(same.body == "01");

        auto older = http::util::format_http_date(digits.mtime - 60);
        auto changed = get(site.port, "/static/digits.txt", "Range: bytes=0-1\r\nIf-Range: " + older + "\r\n");
        REQUIRE(changed.status == 200);
        REQUIRE(changed.body == "0123456789");

        // a weak entity tag never matches If-Range (strong comparison)
        auto tag = get(site.port, "/static/digits.txt", "Range: bytes=0-1\r\nIf-Range: " + digits.etag + "\r\n");
        REQUIRE(tag.status == 200);
    }

    SECTION("A range is not compressed") {
        auto response = get(site.port, "/static/big.js", "Accept-Encoding: gzip\r\nRange: bytes=0-299\r\n");
        REQUIRE(response.status == 206);
        REQUIRE_FALSE(response.has_header("Content-Encoding"));
        REQUIRE(response.body == std::string(300, 'a'));
    }

    SECTION("A conditional request for a file the client has takes precedence over the range") {
        require_not_modified(get(site.port, "/static/digits.txt", "Range: bytes=0-1\r\nIf-None-Match: " + digits.etag + "\r\n"),
                             digits);
    }

    SECTION("HEAD ignores the range") {
        auto response = send_request(site.port, "HEAD", "/static/digits.txt", "Range: bytes=0-1\r\n");
        REQUIRE(response.status == 200);
        REQUIRE(response.body.empty());
        REQUIRE(response.header("Content-Length") == "10");
    }
}

TEST_CASE("send_file carries validators and answers conditional requests", "[server][static][integration]") {
    static_site site;
    auto path = site.root / "digits.txt";
    site.server.get("/file", [path](http::response& res) {
        res.send_file(path);
    });
    site.server.get("/private", [path](http::response& res) {
        res.header("Cache-Control", "private, max-age=60");
        res.send_file(path);
    });
    site.server.get("/download", [path](http::response& res) {
        res.send_file(path, true);
    });
    site.server.post("/file", [path](http::response& res) {
        res.send_file(path);
    });
    site.start();
    auto file = site.info("digits.txt");

    SECTION("Validators, and Cache-Control: no-cache by default") {
        auto response = get(site.port, "/file");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "0123456789");
        REQUIRE(response.header("ETag") == file.etag);
        REQUIRE(response.header("Last-Modified") == file.last_modified);
        REQUIRE(response.header("Cache-Control") == "no-cache");
        require_not_modified(get(site.port, "/file", "If-None-Match: " + file.etag + "\r\n"), file);
    }

    SECTION("A Cache-Control set by the handler is kept, also on 304") {
        auto response = get(site.port, "/private");
        REQUIRE(response.header("Cache-Control") == "private, max-age=60");
        REQUIRE(count(response.header("Cache-Control"), "no-cache") == 0);

        auto revalidated = get(site.port, "/private", "If-Modified-Since: " + file.last_modified + "\r\n");
        require_not_modified(revalidated, file);
        REQUIRE(revalidated.header("Cache-Control") == "private, max-age=60");
    }

    SECTION("Download") {
        auto response = get(site.port, "/download");
        REQUIRE(response.status == 200);
        REQUIRE(response.header("Content-Disposition") == "attachment; filename=\"digits.txt\"");
        REQUIRE(response.header("ETag") == file.etag);
    }

    SECTION("Conditional headers only apply to GET and HEAD") {
        auto response = send_request(site.port, "POST", "/file", "Content-Length: 0\r\nIf-None-Match: *\r\n");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "0123456789");
    }
}

// static_options::fallthrough: a path without a file to serve goes to the not-found treatment of
// the host of serve_static(), with the response untouched; what is rejected is still answered
namespace {

    // A site served at /static, with a directory without an index and dotfiles, and a not-found
    // handler answering 404 without body, counting its calls
    struct fallthrough_site : static_site {
        std::atomic<int> not_found{0};

        explicit fallthrough_site(http::static_options options = {.fallthrough = true}) {
            fs::create_directories(root / "empty");
            write_file(root / ".env", "API_KEY=secret");
            write_file(root / ".git" / "config", "[core]");
            server.serve_static("/static", root.string(), std::move(options));
            server.set_not_found_handler([this](http::response& res) {
                not_found++;
                res.header("X-Not-Found", "1");
                res.error(http::http_response::status::not_found);
            });
        }

        // A 404 from the not-found handler alone, adding to `calls` its call, and nothing that
        // the static server sets on what it answers
        void require_passed(const raw_response& response, int calls = 1) {
            REQUIRE(response.status == 404);
            REQUIRE(response.header("X-Not-Found") == "1");
            REQUIRE(response.body.empty());
            for (const auto* name : {"ETag", "Last-Modified", "Accept-Ranges", "Cache-Control", "Vary"}) {
                INFO(name);
                REQUIRE_FALSE(response.has_header(name));
            }
            REQUIRE(not_found == calls);
        }

        // A rejection answered before the not-found handler, which is never told: without what
        // it could have served, nor anything that the handler sets
        void require_rejected(const raw_response& response, int status) {
            REQUIRE(response.status == status);
            REQUIRE(count(response.body, "top secret") == 0);
            REQUIRE(count(response.body, "API_KEY") == 0);
            REQUIRE(count(response.body, "[core]") == 0);
            REQUIRE_FALSE(response.has_header("X-Not-Found"));
            REQUIRE(not_found == 0);
        }
    };

}

TEST_CASE("With fallthrough, paths without a file reach the not-found handler", "[server][static][integration]") {
    SECTION("A missing path, without the root fallback") {
        fallthrough_site site;
        site.start();
        auto response = get(site.port, "/static/missing");
        site.require_passed(response);
        REQUIRE(count(response.body, "<html>") == 0);
    }

    SECTION("HEAD") {
        fallthrough_site site;
        site.start();
        site.require_passed(send_request(site.port, "HEAD", "/static/missing"));
    }

    SECTION("Files and the index of a directory are served as ever") {
        fallthrough_site site;
        site.start();

        auto app = get(site.port, "/static/app.js");
        auto file = site.info("app.js");
        REQUIRE(app.status == 200);
        REQUIRE(app.body == "console.log('app');");
        REQUIRE(app.header("ETag") == file.etag);
        REQUIRE(app.header("Last-Modified") == file.last_modified);
        REQUIRE(app.header("Cache-Control") == "no-cache");
        require_not_modified(get(site.port, "/static/app.js", "If-None-Match: " + file.etag + "\r\n"), file);

        auto root_index = get(site.port, "/static/");
        REQUIRE(root_index.status == 200);
        REQUIRE(root_index.body == "<html>index</html>");
        REQUIRE(root_index.header("ETag") == site.info("index.html").etag);

        auto docs_index = get(site.port, "/static/docs");
        REQUIRE(docs_index.status == 200);
        REQUIRE(docs_index.body == "<html>docs</html>");
        require_not_modified(get(site.port, "/static/docs", "If-None-Match: *\r\n"), site.info("docs/index.html"));

        REQUIRE(site.not_found == 0);
    }

    SECTION("A directory without its fallback file") {
        fallthrough_site site;
        site.start();
        site.require_passed(get(site.port, "/static/empty"));
    }

    SECTION("An ignored dotfile, as a path without a file, whether it exists or not") {
        fallthrough_site site;
        site.start();
        site.require_passed(get(site.port, "/static/.env"), 1);
        site.require_passed(get(site.port, "/static/%2Eenv"), 2);
        site.require_passed(get(site.port, "/static/.git/config"), 3);
        site.require_passed(get(site.port, "/static/.nothing"), 4);
    }

    SECTION("Without a fallback, the root too") {
        fallthrough_site site({.fallback = "", .fallthrough = true});
        site.start();
        site.require_passed(get(site.port, "/static/missing"), 1);
        site.require_passed(get(site.port, "/static/"), 2);
        site.require_passed(get(site.port, "/static/docs"), 3);
    }

    SECTION("A name too long to exist") {
        fallthrough_site site;
        site.start();
        site.require_passed(get(site.port, "/static/" + std::string(1000, 'a')));
    }

    SECTION("Conditional and range requests pass as a plain GET") {
        fallthrough_site site;
        site.start();
        site.require_passed(get(site.port, "/static/missing", "If-None-Match: *\r\n"), 1);
        site.require_passed(get(site.port, "/static/missing", "Range: bytes=0-9\r\n"), 2);
    }

#ifndef _WIN32
    SECTION("What is neither a file nor a directory, without opening it") {
        fallthrough_site site;
        site.start();
        auto fifo = site.root / "pipe";
        REQUIRE(mkfifo(fifo.c_str(), 0600) == 0);
        site.require_passed(get(site.port, "/static/pipe"));
    }
#endif

    SECTION("A body sent with the request does not reach the next request of the connection") {
        fallthrough_site site;
        site.start();
        auto result = raw_exchange(site.port,
                                   "GET /static/missing HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello"
                                   "GET /static/app.js HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        INFO(result.data);
        REQUIRE(result.closed);
        REQUIRE(count(result.data, "HTTP/1.1 404") == 1);
        REQUIRE(count(result.data, "HTTP/1.1 200") == 1);
        REQUIRE(count(result.data, "console.log('app');") == 1);
        REQUIRE(site.not_found == 1);
    }

    SECTION("The handler is told as of a request without a route") {
        fallthrough_site site;
        bool matched = true;
        site.server.set_not_found_handler([&matched](http::request& req, http::response& res) {
            matched = req.get_matched_route() != nullptr;
            res.error(http::http_response::status::not_found);
        });
        site.start();
        REQUIRE(get(site.port, "/static/missing").status == 404);
        REQUIRE_FALSE(matched);
    }

    SECTION("A coroutine handler, and one that throws as with any request without a route") {
        fallthrough_site site;
        site.server.set_not_found_handler([](http::request& req, http::response& res) -> thinger::awaitable<void> {
            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 1ms);
            co_await timer.async_wait(thinger::use_awaitable);
            if (req.get_http_request()->get_path().ends_with("/throw")) throw std::runtime_error("failed");
            res.send("from the handler");
        });
        site.start();
        REQUIRE(get(site.port, "/static/missing").body == "from the handler");
        REQUIRE(get(site.port, "/static/throw").status == 500);
        REQUIRE(get(site.port, "/no-route/throw").status == 500);
    }

    SECTION("Without a not-found handler: the answer of the host to a GET without a route") {
        static_site site;
        site.server.serve_static("/static", site.root.string(), {.fallthrough = true});
        site.server.set_error_formatter([](const http::http_error& error, http::http_response& response) {
            response.set_content("formatted " + std::to_string(static_cast<int>(error.status)) + " [" + error.message + "]",
                                 "text/plain");
        });
        site.start();
        auto response = get(site.port, "/static/missing");
        REQUIRE(response.status == 404);
        REQUIRE(response.body == "formatted 404 []");
        REQUIRE(response.body == get(site.port, "/no-route").body);
    }

    SECTION("The not-found handler of the host where it was registered") {
        fallthrough_site site;
        std::atomic<int> host_calls{0};
        auto& host = site.server.host("a.example");
        host.serve_static("/static", site.root.string(), {.fallthrough = true});
        host.set_not_found_handler([&host_calls](http::response& res) {
            host_calls++;
            res.header("X-Host-Not-Found", "1");
            res.error(http::http_response::status::not_found);
        });
        site.start();

        auto result = raw_exchange(site.port, "GET /static/missing HTTP/1.1\r\nHost: a.example\r\nConnection: close\r\n\r\n");
        REQUIRE(count(result.data, "HTTP/1.1 404") == 1);
        REQUIRE(count(result.data, "X-Host-Not-Found: 1") == 1);
        REQUIRE(count(result.data, "X-Not-Found: 1") == 0);
        REQUIRE(host_calls == 1);
        REQUIRE(site.not_found == 0);

        // Another host gets those of the application
        site.require_passed(get(site.port, "/static/missing"));
        REQUIRE(host_calls == 1);
    }

    SECTION("Middlewares run once") {
        fallthrough_site site;
        std::atomic<int> middleware_calls{0};
        site.server.use([&middleware_calls](http::request&, http::response&) -> thinger::awaitable<bool> {
            middleware_calls++;
            co_return true;
        });
        site.start();
        site.require_passed(get(site.port, "/static/missing"));
        REQUIRE(middleware_calls == 1);
    }

    SECTION("Another serve_static() of a shorter prefix is not tried") {
        fallthrough_site site;
        write_file(site.base / "other" / "only-in-other.js", "other");
        write_file(site.base / "other" / "assets" / "only-in-other.js", "other");
        site.server.serve_static("/", (site.base / "other").string(), {.fallthrough = true});
        site.server.serve_static("/assets", (site.root / "docs").string(), {.fallthrough = true});
        site.start();
        REQUIRE(get(site.port, "/only-in-other.js").status == 200);
        site.require_passed(get(site.port, "/assets/only-in-other.js"));
    }

    SECTION("An in-memory dispatch does the same") {
        fallthrough_site site;
        auto missing = run_dispatch(site.server, make_request(http::method::GET, "/static/missing"));
        REQUIRE(missing->get_status_code() == 404);
        REQUIRE(missing->get_header("X-Not-Found") == "1");
        REQUIRE(missing->get_content().empty());
        REQUIRE(site.not_found == 1);
        auto file = run_dispatch(site.server, make_request(http::method::GET, "/static/app.js"));
        REQUIRE(file->get_status_code() == 200);
        REQUIRE(file->get_content() == "console.log('app');");
        REQUIRE(site.not_found == 1);
    }

    SECTION("The route stays out of the OpenAPI document") {
        fallthrough_site site;
        REQUIRE(site.server.openapi().generate()["paths"].empty());
    }
}

TEST_CASE("Without fallthrough, paths without a file are answered as ever", "[server][static][integration]") {
    SECTION("The fallback of the root") {
        fallthrough_site site({});
        site.start();
        auto response = get(site.port, "/static/missing");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "<html>index</html>");
        REQUIRE(site.not_found == 0);
    }

    SECTION("Without a fallback, a 404 of its own") {
        fallthrough_site site({.fallback = ""});
        site.start();
        auto response = get(site.port, "/static/missing");
        REQUIRE(response.status == 404);
        REQUIRE(response.body == "Not found");
        REQUIRE_FALSE(response.has_header("X-Not-Found"));
        REQUIRE(site.not_found == 0);
    }
}

TEST_CASE("With fallthrough, rejected paths are answered and never reach the not-found handler",
          "[server][static][integration]") {
    SECTION("A symlink leaving the directory: 403") {
        fallthrough_site site;
        fs::create_symlink(site.base / "www-secret" / "secret.txt", site.root / "link.txt");
        fs::create_directory_symlink(site.base / "www-secret", site.root / "linked");
        site.start();
        site.require_rejected(get(site.port, "/static/link.txt"), 403);
        site.require_rejected(get(site.port, "/static/linked/secret.txt"), 403);
    }

    SECTION("A denied dotfile: 403, whether it exists or not") {
        fallthrough_site site({.dotfiles = http::dotfiles_policy::deny, .fallthrough = true});
        site.start();
        site.require_rejected(get(site.port, "/static/.env"), 403);
        site.require_rejected(get(site.port, "/static/.git/config"), 403);
        site.require_rejected(get(site.port, "/static/.nothing"), 403);
    }

    SECTION("A malformed path: 400") {
        fallthrough_site site;
        site.start();
        site.require_rejected(get(site.port, "/static/bad%zz"), 400);
        site.require_rejected(get(site.port, "/static/a%00b"), 400);
    }

    SECTION("A traversal in the request target: 400") {
        fallthrough_site site;
        site.start();
        site.require_rejected(get(site.port, "/static/../www-secret/secret.txt"), 400);
        site.require_rejected(get(site.port, "/static/%2E%2E/www-secret/secret.txt"), 400);
    }

    SECTION("An allowed dotfile is served") {
        fallthrough_site site({.dotfiles = http::dotfiles_policy::allow, .fallthrough = true});
        site.start();
        auto response = get(site.port, "/static/.env");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "API_KEY=secret");
        REQUIRE(site.not_found == 0);
    }
}

// file_server::serve() from a handler of its own, with no more than the public header and what
// any handler includes: whether it answered tells the handler what is left for it to do
namespace {

    // serve() has sent the response when it returns, so the client may get it before the handler
    // goes on to count it: wait for the count, with a deadline
    void require_answered(const std::atomic<int>& answered, int expected) {
        bool counted = shutdown_test::eventually([&] { return answered == expected; });
        INFO("answered " << answered.load() << ", expected " << expected);
        REQUIRE(counted);
    }

    // A site without routes: a not-found handler serves the files under /app/ and, for a path
    // without a file, index.html under /app/console/ and a 404 without body elsewhere
    struct handler_site : static_site {
        http::static_options options;
        std::atomic<int> answered{0};
        std::atomic<int> unanswered{0};

        explicit handler_site(http::static_options static_options = {.cache_control = "public, max-age=60",
                                                                     .fallthrough = true})
            : options(std::move(static_options)) {
            fs::create_directories(root / "empty");
            write_file(root / ".env", "API_KEY=secret");
            server.set_not_found_handler([this](http::request& req, http::response& res) {
                auto method = req.get_http_request()->get_method();
                auto path = req.get_http_request()->get_path();
                if ((method == http::method::GET || method == http::method::HEAD) && path.starts_with("/app/")) {
                    if (http::file_server::serve(req, res, root, std::string_view(path).substr(5), options)) {
                        answered++;
                        return;
                    }
                    unanswered++;
                    if (path.starts_with("/app/console/")) {
                        res.send_file(root / "index.html");
                        return;
                    }
                }
                res.error(http::http_response::status::not_found);
            });
        }
    };

}

TEST_CASE("file_server::serve serves the files of a directory from a not-found handler", "[server][static][integration]") {
    SECTION("A file, with its validators, the options and a 304") {
        handler_site site;
        site.start();
        auto file = site.info("app.js");

        auto response = get(site.port, "/app/app.js");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "console.log('app');");
        REQUIRE(response.header("ETag") == file.etag);
        REQUIRE(response.header("Last-Modified") == file.last_modified);
        REQUIRE(response.header("Cache-Control") == "public, max-age=60");

        auto not_modified = get(site.port, "/app/app.js", "If-None-Match: " + file.etag + "\r\n");
        require_not_modified(not_modified, file);
        REQUIRE(not_modified.header("Cache-Control") == "public, max-age=60");
        require_answered(site.answered, 2);
        REQUIRE(site.unanswered == 0);
    }

    SECTION("HEAD, a range, the index of a directory and a query, as serve_static() does") {
        handler_site site;
        site.start();

        auto head = send_request(site.port, "HEAD", "/app/app.js");
        REQUIRE(head.status == 200);
        REQUIRE(head.body.empty());
        REQUIRE(head.header("ETag") == site.info("app.js").etag);

        auto range = get(site.port, "/app/digits.txt", "Range: bytes=2-4\r\n");
        REQUIRE(range.status == 206);
        REQUIRE(range.body == "234");

        REQUIRE(get(site.port, "/app/docs").body == "<html>docs</html>");
        REQUIRE(get(site.port, "/app/").body == "<html>index</html>");
        REQUIRE(get(site.port, "/app/app.js?v=1").body == "console.log('app');");
        require_answered(site.answered, 5);
        REQUIRE(site.unanswered == 0);
    }

    SECTION("A path without a file is left to the handler, which applies its rule by prefix") {
        handler_site site;
        site.start();

        auto console = get(site.port, "/app/console/devices");
        REQUIRE(console.status == 200);
        REQUIRE(console.body == "<html>index</html>");
        REQUIRE(site.unanswered == 1);

        auto other = get(site.port, "/app/other/missing");
        REQUIRE(other.status == 404);
        REQUIRE(other.body.empty());
        REQUIRE_FALSE(other.has_header("ETag"));
        REQUIRE_FALSE(other.has_header("Cache-Control"));
        REQUIRE(site.unanswered == 2);

        // A directory without its fallback and an ignored dotfile are paths without a file too
        REQUIRE(get(site.port, "/app/empty").status == 404);
        REQUIRE(get(site.port, "/app/.env").status == 404);
        REQUIRE(site.unanswered == 4);
        REQUIRE(site.answered == 0);
    }

    SECTION("A rejected path is answered, and the handler knows") {
        handler_site site({.dotfiles = http::dotfiles_policy::deny, .fallthrough = true});
        fs::create_symlink(site.base / "www-secret" / "secret.txt", site.root / "link.txt");
        site.start();

        auto link = get(site.port, "/app/link.txt");
        REQUIRE(link.status == 403);
        REQUIRE(count(link.body, "top secret") == 0);
        require_answered(site.answered, 1);

        auto dotfile = get(site.port, "/app/.env");
        REQUIRE(dotfile.status == 403);
        REQUIRE(count(dotfile.body, "API_KEY") == 0);
        require_answered(site.answered, 2);

        // A decoded NUL is valid for the request parser: this 400 is the one of the function
        REQUIRE(get(site.port, "/app/a%00b").status == 400);
        require_answered(site.answered, 3);

        // A malformed escape is rejected by the request parser over TCP, before any handler...
        REQUIRE(get(site.port, "/app/bad%zz").status == 400);
        REQUIRE(site.answered == 3);
        // ...and by the function when it comes unparsed
        REQUIRE(run_dispatch(site.server, make_request(http::method::GET, "/app/bad%zz"))->get_status_code() == 400);
        require_answered(site.answered, 4);
        REQUIRE(site.unanswered == 0);
    }

    SECTION("A path out of the directory given by the caller gets 403: with \"..\", or absolute") {
        static_site site;
        std::atomic<int> answered{0};
        site.server.get("/raw", [&](http::request& req, http::response& res) {
            auto path = req.get_http_request()->get_uri_parameter("path");
            if (http::file_server::serve(req, res, site.root, path, {.fallthrough = true})) answered++;
            else res.send("left to the handler");
        });
        site.start();
        REQUIRE(get(site.port, "/raw?path=app.js").body == "console.log('app');");
        REQUIRE(get(site.port, "/raw?path=..%2Fwww-secret%2Fsecret.txt").status == 403);
        // A leading "/" left by the caller: not the app.js of the directory
        REQUIRE(get(site.port, "/raw?path=/app.js").status == 403);
        REQUIRE(get(site.port, "/raw?path=" + (site.base / "www-secret" / "secret.txt").string()).status == 403);
        require_answered(answered, 4);
    }

    SECTION("Without fallthrough it always answers, as serve_static()") {
        handler_site site(http::static_options{});
        handler_site without_fallback(http::static_options{.fallback = ""});
        site.start();
        without_fallback.start();

        auto fallback = get(site.port, "/app/missing");
        REQUIRE(fallback.status == 200);
        REQUIRE(fallback.body == "<html>index</html>");
        require_answered(site.answered, 1);

        auto not_found = get(without_fallback.port, "/app/missing");
        REQUIRE(not_found.status == 404);
        REQUIRE(not_found.body == "Not found");
        require_answered(without_fallback.answered, 1);
        REQUIRE(without_fallback.unanswered == 0);
    }
}

TEST_CASE("file_server::serve takes the path from the caller, not from a parameter", "[server][static][integration]") {
    static_site site;
    write_file(site.root / "my file+1.txt", "spaced");
    site.server.get("/files/:rest(.*)", [root = site.root](http::request& req, http::response& res) {
        http::file_server::serve(req, res, root, req["rest"], {});
    });
    site.start();

    auto file = get(site.port, "/files/app.js");
    REQUIRE(file.status == 200);
    REQUIRE(file.body == "console.log('app');");
    REQUIRE(file.header("ETag") == site.info("app.js").etag);
    REQUIRE(get(site.port, "/files/docs").body == "<html>docs</html>");
    // Decoded by the function, with "+" literal
    REQUIRE(get(site.port, "/files/my%20file+1.txt").body == "spaced");
}

TEST_CASE("A rule by prefix needs only serve_static() and a not-found handler", "[server][static][integration]") {
    static_site site;
    site.server.serve_static("/", site.root.string(), {.fallthrough = true});
    site.server.set_not_found_handler([root = site.root](http::request& req, http::response& res) {
        auto method = req.get_http_request()->get_method();
        if ((method == http::method::GET || method == http::method::HEAD) &&
            req.get_http_request()->get_path().starts_with("/console/")) {
            res.send_file(root / "index.html");
            return;
        }
        res.error(http::http_response::status::not_found);
    });
    site.start();

    REQUIRE(get(site.port, "/app.js").body == "console.log('app');");
    REQUIRE(get(site.port, "/console/devices").body == "<html>index</html>");
    auto elsewhere = get(site.port, "/other/missing");
    REQUIRE(elsewhere.status == 404);
    REQUIRE(elsewhere.body.empty());
    REQUIRE(send_request(site.port, "POST", "/console/devices", "Content-Length: 0\r\n").status == 404);
}

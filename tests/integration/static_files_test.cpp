#include "server_test_helpers.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/file_server/file_info.hpp>
#include <thinger/http/util/http_date.hpp>
#include <thinger/util/ascii.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>

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

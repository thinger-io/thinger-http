#include "server_test_helpers.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/file_server/file_info.hpp>
#include <thinger/util/ascii.hpp>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#ifdef __APPLE__
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

// The memory a server holds for a static file over 1 MiB while it is sent: at most 1 MiB above
// what it held before the request, however large the file and however slowly the client reads
// it (or if it does not read at all). The heap bytes held are counted by replacing the global
// operator new in this test program, so it is a program of its own: not part of the runners
// gathering every test, nor built with the sanitizers, which replace it themselves. Its 64 MiB
// file is not compressible, and its content is a function of the offset: the client compares
// what it reads as it arrives, without keeping it

namespace {

    std::atomic<int64_t> live_bytes{0};
    std::atomic<int64_t> peak_bytes{0};

    size_t usable_size(void* memory) {
#ifdef __APPLE__
        return malloc_size(memory);
#else
        return malloc_usable_size(memory);
#endif
    }

}

void* operator new(std::size_t size) {
    void* memory = std::malloc(size ? size : 1);
    if (!memory) throw std::bad_alloc();
    auto bytes = static_cast<int64_t>(usable_size(memory));
    auto now = live_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    auto peak = peak_bytes.load(std::memory_order_relaxed);
    while (now > peak && !peak_bytes.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {}
    return memory;
}

void operator delete(void* memory) noexcept {
    if (!memory) return;
    live_bytes.fetch_sub(static_cast<int64_t>(usable_size(memory)), std::memory_order_relaxed);
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    ::operator delete(memory);
}

using namespace thinger;
using namespace std::chrono_literals;

namespace {

    namespace fs = std::filesystem;
    using namespace server_test;
    using steady = std::chrono::steady_clock;

    constexpr uint64_t MiB = 1024 * 1024;
    constexpr uint64_t file_size = 64 * MiB;

    // What the server may hold for one response: the bound asserted
    constexpr int64_t bound = 1 * MiB;

    // A piece of a streamed file: what an answer without a body must stay under
    constexpr int64_t piece = 64 * 1024;

    // The most bytes a reading client takes at once, and its pause after each read
    constexpr size_t read_piece = 256 * 1024;
    constexpr auto read_pause = 5ms;

    // The most heap bytes held at once, by the whole program, above those held when it was
    // made: the server's, as the client reads into buffers reserved beforehand
    class heap_watch {
    public:
        heap_watch() : baseline_(live_bytes.load()) { peak_bytes.store(baseline_); }
        int64_t held() const { return peak_bytes.load() - baseline_; }

    private:
        int64_t baseline_;
    };

    // Content of the files, as a function of the offset: random-looking bytes (splitmix64,
    // not compressible), and a text
    char binary_at(uint64_t offset) {
        uint64_t x = offset / 8 + 0x9e3779b97f4a7c15;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9;
        x = (x ^ (x >> 27)) * 0x94d049bb133111eb;
        x ^= x >> 31;
        return static_cast<char>(x >> (8 * (offset % 8)));
    }

    char text_at(uint64_t offset) {
        constexpr std::string_view line = "the quick brown fox jumps over the lazy dog\n";
        return line[offset % line.size()];
    }

    using content_at = char (*)(uint64_t);

    void write_content(const fs::path& path, uint64_t size, content_at content) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        std::string buffer;
        for (uint64_t at = 0; at < size;) {
            buffer.resize(std::min(size - at, MiB));
            for (size_t i = 0; i < buffer.size(); ++i) buffer[i] = content(at + i);
            out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            at += buffer.size();
        }
        REQUIRE(out);
    }

    // The files served, written once for the program: data.bin (64 MiB, binary), text.txt (2 MiB,
    // under the compression cap), large.txt (5 MiB, over it), and bundle.js, a small file with a
    // precompressed sibling over 1 MiB (bundle.js.gz: any bytes do)
    struct served_files {
        fs::path root;
        fs::path data;
        uint64_t sibling_size = 3 * MiB + 5;

        served_files() {
            root = fs::temp_directory_path() /
                   ("thinger_static_memory_" + std::to_string(steady::now().time_since_epoch().count()));
            fs::create_directories(root);
            data = root / "data.bin";
            write_content(data, file_size, binary_at);
            write_content(root / "text.txt", 2 * MiB, text_at);
            write_content(root / "large.txt", 5 * MiB, text_at);
            write_content(root / "bundle.js", 100, text_at);
            write_content(root / "bundle.js.gz", sibling_size, binary_at);
            fs::last_write_time(root / "bundle.js.gz", fs::last_write_time(root / "bundle.js") + 1s);
        }

        ~served_files() {
            std::error_code ec;
            fs::remove_all(root, ec);
        }

        http::file_info info(const std::string& name) const {
            auto file = http::file_info::of(root / name);
            REQUIRE(file);
            return *file;
        }
    };

    const served_files& files() {
        static served_files instance;
        return instance;
    }

    // The files under /static, and data.bin through send_file(): /file as it is, /download
    // forced as a download with the handler's Cache-Control. A response write that stalls for
    // `write_timeout` closes the connection
    struct memory_site {
        http::server server;
        std::unique_ptr<running_server> running;

        explicit memory_site(std::chrono::milliseconds write_timeout = 10s) {
            http::connection_options options;
            options.write_timeout = write_timeout;
            server.set_connection_options(options);
            server.serve_static("/static", files().root.string());
            auto data = files().data;
            server.get("/file", [data](http::response& res) { res.send_file(data); });
            server.get("/download", [data](http::response& res) {
                res.header("Cache-Control", "private, max-age=60");
                res.send_file(data, true);
            });
            running = std::make_unique<running_server>(server);
        }
    };

    struct fetched {
        int status = 0;
        std::string head;
        uint64_t body = 0;      // bytes of body received
        bool identical = true;  // whether they were the content from the offset given
        bool closed = false;
        int64_t held = 0;       // the most heap bytes held meanwhile (see heap_watch)

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

    // Send `request` to `site` and read its response slowly to the end (the server closes the
    // connection: the requests say Connection: close), within `limit`: at most 256 KiB, then a
    // pause of 5 ms. The body is compared, as it arrives, with `content` from `offset`
    fetched fetch(const memory_site& site, const std::string& request, content_at content = binary_at,
                  uint64_t offset = 0, std::chrono::milliseconds limit = 30s) {
        http::test::raw_connection connection(site.running->endpoint());
        fetched result;
        result.head.reserve(4096);
        std::string received;
        received.reserve(read_piece);
        auto compare = [&](std::string_view data) {
            for (char byte : data) {
                if (result.identical && byte != content(offset + result.body)) result.identical = false;
                ++result.body;
            }
        };

        heap_watch watch;
        auto until = steady::now() + limit;
        bool in_head = connection.write(request);
        while (!connection.eof() && steady::now() < until) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - steady::now());
            // The head a little at a time, into its reserved buffer
            if (in_head) {
                connection.read_some(result.head, left, std::min<size_t>(512, result.head.capacity() - result.head.size()));
                auto end = result.head.find("\r\n\r\n");
                if (end == std::string::npos) {
                    if (result.head.size() == result.head.capacity()) break;
                    continue;
                }
                compare(std::string_view(result.head).substr(end + 4));
                result.head.resize(end);
                in_head = false;
                continue;
            }
            connection.read_some(received, left, read_piece);
            compare(received);
            received.clear();
            std::this_thread::sleep_for(read_pause);
        }
        result.held = watch.held();
        result.closed = connection.eof();
        if (result.head.starts_with("HTTP/1.1 ") && result.head.size() >= 12) {
            result.status = std::stoi(result.head.substr(9, 3));
        }
        return result;
    }

    std::string get(const std::string& target, const std::string& headers = "") {
        return "GET " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" + headers + "\r\n";
    }

    // A whole body, received to its end and identical to the content, with at most `bound` held
    void require_body(const fetched& response, uint64_t length) {
        INFO("held " << response.held << " bytes; received " << response.body << " of " << length);
        REQUIRE(response.closed);
        REQUIRE(response.body == length);
        REQUIRE(response.identical);
        REQUIRE(response.held <= bound);
    }

}

TEST_CASE("A client that does not read holds at most 1 MiB of a 64 MiB file until the write deadline closes it",
          "[server][static][memory][integration]") {
    // A write that stalls for 2 s closes the connection
    memory_site site(2s);
    // A small receive window: the server stalls as soon as the system buffers are full
    http::test::raw_connection connection(site.running->endpoint(), false, 10s, 64 * 1024);
    std::string received;
    received.reserve(read_piece);

    heap_watch watch;
    auto start = steady::now();
    REQUIRE(connection.write("GET /static/data.bin HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    // Reading nothing for 3 s: past the write deadline
    std::this_thread::sleep_for(3s);
    auto held = watch.held();

    // What was written before the close, then the close: within 5 s more
    std::string head;
    uint64_t total = 0;
    auto until = steady::now() + 5s;
    while (!connection.eof() && steady::now() < until) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - steady::now());
        connection.read_some(received, left, read_piece);
        if (head.size() < 512) head.append(received, 0, std::min<size_t>(received.size(), 512 - head.size()));
        total += received.size();
        received.clear();
    }
    auto took = steady::now() - start;

    INFO("held " << held << " bytes; received " << total << " bytes in "
                 << std::chrono::duration_cast<std::chrono::milliseconds>(took).count() << " ms");
    REQUIRE(held <= bound);
    REQUIRE(head.starts_with("HTTP/1.1 200"));
    REQUIRE(head.find("Content-Length: 67108864\r\n") != std::string::npos);
    // Closed by the write deadline, the body incomplete
    REQUIRE(connection.eof());
    REQUIRE(total < file_size);
    REQUIRE(took < 8s);
}

TEST_CASE("A slow client receives a 64 MiB file whole, with at most 1 MiB held", "[server][static][memory][integration]") {
    memory_site site;
    auto file = files().info("data.bin");

    SECTION("Through serve_static, with the headers of a HEAD request") {
        auto head = fetch(site, "HEAD /static/data.bin HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(head.status == 200);
        auto response = fetch(site, get("/static/data.bin"));
        require_body(response, file_size);
        REQUIRE(response.status == 200);
        REQUIRE(response.header("Content-Length") == "67108864");
        REQUIRE(response.header("Content-Type") == "application/octet-stream");
        REQUIRE(response.header("Accept-Ranges") == "bytes");
        REQUIRE(response.header("ETag") == file.etag);
        REQUIRE(response.header("Last-Modified") == file.last_modified);
        REQUIRE(response.header("Cache-Control") == "no-cache");
        REQUIRE(response.header("Content-Encoding").empty());
        REQUIRE(response.header("Vary").empty());
        for (auto name : {"Content-Length", "Content-Type", "Accept-Ranges", "ETag", "Last-Modified", "Cache-Control"}) {
            REQUIRE(response.header(name) == head.header(name));
        }
    }

    SECTION("Through send_file, with the handler's Cache-Control, forced as a download") {
        auto response = fetch(site, get("/download"));
        require_body(response, file_size);
        REQUIRE(response.status == 200);
        REQUIRE(response.header("Content-Length") == "67108864");
        REQUIRE(response.header("Cache-Control") == "private, max-age=60");
        REQUIRE(response.header("Content-Disposition") == "attachment; filename=\"data.bin\"");
        REQUIRE(response.header("ETag") == file.etag);
    }

    SECTION("Through send_file, as it is: no Content-Disposition") {
        auto response = fetch(site, get("/file", "Range: bytes=0-2097151\r\n"));
        require_body(response, 2 * MiB);
        REQUIRE(response.status == 206);
        REQUIRE(response.header("Cache-Control") == "no-cache");
        REQUIRE(response.header("Content-Disposition").empty());
    }
}

TEST_CASE("Byte ranges of a 64 MiB file", "[server][static][memory][integration]") {
    memory_site site;
    auto file = files().info("data.bin");

    SECTION("A range of 32 MiB is streamed") {
        auto response = fetch(site, get("/static/data.bin", "Range: bytes=1048576-34603007\r\n"), binary_at, MiB);
        require_body(response, 32 * MiB);
        REQUIRE(response.status == 206);
        REQUIRE(response.header("Content-Range") == "bytes 1048576-34603007/67108864");
        REQUIRE(response.header("Content-Length") == "33554432");
        REQUIRE(response.header("ETag") == file.etag);
    }

    SECTION("A suffix, and a range open to the end: the last bytes") {
        auto suffix = fetch(site, get("/static/data.bin", "Range: bytes=-100\r\n"), binary_at, file_size - 100);
        require_body(suffix, 100);
        REQUIRE(suffix.status == 206);
        REQUIRE(suffix.header("Content-Range") == "bytes 67108764-67108863/67108864");

        auto open = fetch(site, get("/static/data.bin", "Range: bytes=67108800-\r\n"), binary_at, 67108800);
        require_body(open, 64);
        REQUIRE(open.status == 206);
        REQUIRE(open.header("Content-Range") == "bytes 67108800-67108863/67108864");
    }

    SECTION("A range past the end: 416") {
        auto response = fetch(site, get("/static/data.bin", "Range: bytes=67108864-\r\n"));
        REQUIRE(response.status == 416);
        REQUIRE(response.header("Content-Range") == "bytes */67108864");
        REQUIRE(response.held < piece);
    }

    SECTION("If-Range with another date, or several ranges: the whole file, streamed") {
        auto other_date = fetch(site, get("/static/data.bin", "Range: bytes=0-99\r\n"
                                                              "If-Range: Thu, 01 Jan 2004 00:00:00 GMT\r\n"));
        require_body(other_date, file_size);
        REQUIRE(other_date.status == 200);
        REQUIRE(other_date.header("Content-Range").empty());

        auto several = fetch(site, get("/static/data.bin", "Range: bytes=0-99,200-299\r\n"));
        require_body(several, file_size);
        REQUIRE(several.status == 200);
        REQUIRE(several.header("Content-Length") == "67108864");
    }

    SECTION("HEAD, and a conditional GET that matches: no body, and the file is not read") {
        auto head = fetch(site, "HEAD /static/data.bin HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        INFO("held " << head.held << " bytes");
        REQUIRE(head.status == 200);
        REQUIRE(head.header("Content-Length") == "67108864");
        REQUIRE(head.header("ETag") == file.etag);
        REQUIRE(head.body == 0);
        REQUIRE(head.held < piece);

        auto revalidated = fetch(site, get("/static/data.bin", "If-None-Match: " + file.etag + "\r\n"));
        INFO("held " << revalidated.held << " bytes");
        REQUIRE(revalidated.status == 304);
        REQUIRE(revalidated.header("ETag") == file.etag);
        REQUIRE(revalidated.header("Last-Modified") == file.last_modified);
        REQUIRE(revalidated.body == 0);
        REQUIRE(revalidated.held < piece);
    }

    SECTION("A precompressed sibling over 1 MiB is streamed, whole or by range of its bytes") {
        auto sibling = files().info("bundle.js.gz");
        auto etag = sibling.etag;
        etag.insert(etag.size() - 1, "-gzip");
        auto size = std::to_string(files().sibling_size);

        auto whole = fetch(site, get("/static/bundle.js", "Accept-Encoding: gzip\r\n"));
        require_body(whole, files().sibling_size);
        REQUIRE(whole.status == 200);
        REQUIRE(whole.header("Content-Encoding") == "gzip");
        REQUIRE(whole.header("Content-Length") == size);
        REQUIRE(whole.header("Vary") == "Accept-Encoding");
        REQUIRE(whole.header("ETag") == etag);

        auto range = fetch(site, get("/static/bundle.js", "Accept-Encoding: gzip\r\nRange: bytes=1048576-3145727\r\n"),
                           binary_at, MiB);
        require_body(range, 2 * MiB);
        REQUIRE(range.status == 206);
        REQUIRE(range.header("Content-Encoding") == "gzip");
        REQUIRE(range.header("Content-Range") == "bytes 1048576-3145727/" + size);
        REQUIRE(range.header("Vary") == "Accept-Encoding");
        REQUIRE(range.header("ETag") == etag);
    }
}

TEST_CASE("Text files over 1 MiB are held whole only to be compressed", "[server][static][memory][integration]") {
    memory_site site;

    SECTION("Under the cap, for a client accepting no coding: streamed") {
        auto response = fetch(site, get("/static/text.txt"), text_at);
        require_body(response, 2 * MiB);
        REQUIRE(response.status == 200);
        REQUIRE(response.header("Content-Encoding").empty());
        REQUIRE(response.header("Vary") == "Accept-Encoding");
    }

    SECTION("Under the cap, for a client accepting gzip: compressed whole") {
        auto response = fetch(site, get("/static/text.txt", "Accept-Encoding: gzip\r\n"), text_at);
        REQUIRE(response.closed);
        REQUIRE(response.status == 200);
        REQUIRE(response.header("Content-Encoding") == "gzip");
        REQUIRE(response.body < 2 * MiB);
        // (the file is read whole: more than the bound)
        REQUIRE(response.held > bound);
    }

    SECTION("Over the cap, for a client accepting gzip: streamed as it is") {
        auto response = fetch(site, get("/static/large.txt", "Accept-Encoding: gzip, deflate, br\r\n"), text_at);
        require_body(response, 5 * MiB);
        REQUIRE(response.status == 200);
        REQUIRE(response.header("Content-Encoding").empty());
        REQUIRE(response.header("Vary").empty());
    }
}

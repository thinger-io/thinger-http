#include "server_test_helpers.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/client/form.hpp>
#include <thinger/util/types.hpp>
#include <thinger/util/compression.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unistd.h>

using namespace thinger;
using namespace std::chrono_literals;
using namespace server_test;
using json = nlohmann::json;
namespace fs = std::filesystem;

// multipart/form-data bodies read by the server: request::read_multipart() (fields in memory,
// files in temporary files), multipart_reader (streaming) and the "multipart" body decoder

namespace {

    const std::string boundary = "----ThingerTestBoundary7MA4YWxkTrZu0gW";

    std::string field(const std::string& name, const std::string& value) {
        return "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" + value + "\r\n";
    }

    std::string file(const std::string& name, const std::string& filename, const std::string& content,
                     const std::string& type = "application/octet-stream") {
        return "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"; filename=\"" + filename +
               "\"\r\nContent-Type: " + type + "\r\n\r\n" + content + "\r\n";
    }

    std::string close() {
        return "--" + boundary + "--\r\n";
    }

    std::string post(const std::string& path, const std::string& body, const std::string& headers = "",
                     const std::string& content_type = "multipart/form-data; boundary=" + boundary) {
        return "POST " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Type: " + content_type +
               "\r\n" + headers + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    }

    std::string read_file(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    std::string binary_content(size_t size, uint32_t seed = 1) {
        std::string content(size, '\0');
        for (auto& c : content) {
            seed = seed * 1103515245 + 12345;
            c = static_cast<char>(seed >> 16);
        }
        return content;
    }

    // A directory of its own for the temporary files of a test, removed with it
    struct temp_directory {
        fs::path path;
        temp_directory() {
            static std::atomic<int> counter{0};
            path = fs::temp_directory_path() / ("thinger-multipart-test-" + std::to_string(::getpid()) + "-" +
                                                std::to_string(counter++));
            fs::create_directories(path);
        }
        ~temp_directory() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
        size_t files() const {
            return static_cast<size_t>(std::distance(fs::directory_iterator(path), fs::directory_iterator()));
        }
    };

    // Temporary files are deleted when the handler ends, right after it responds
    bool eventually_empty(const temp_directory& directory) {
        for (int i = 0; i < 200; ++i) {
            if (directory.files() == 0) return true;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

    // The form as JSON: fields, and files with their content
    json describe(const http::multipart_form& form) {
        json fields = json::array();
        for (const auto& [name, value] : form.fields()) fields.push_back({name, value});
        json files = json::array();
        for (const auto& f : form.files()) {
            files.push_back({{"name", f.name()}, {"filename", f.filename()}, {"safe", f.safe_filename()},
                             {"type", f.content_type()}, {"size", f.size()}, {"content", read_file(f.path())},
                             {"path", f.path().string()}});
        }
        return {{"fields", fields}, {"files", files}};
    }

    // Route answering the form read with read_multipart(), or its error
    void form_route(http::server& server, const std::string& path, http::multipart_options options = {}) {
        server.post(path, [options](http::request& req, http::response& res) -> thinger::awaitable<void> {
            auto form = co_await req.read_multipart(options);
            if (!form) {
                res.error(form.status(), form.message());
                co_return;
            }
            res.send(describe(form).dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
        });
    }

    std::string response_body(const std::string& response) {
        auto end = response.find("\r\n\r\n");
        return end == std::string::npos ? "" : response.substr(end + 4);
    }

}

TEST_CASE("Multipart forms uploaded with the client", "[multipart][integration]") {
    http::server server;
    temp_directory temp;
    http::multipart_options options;
    options.temp_directory = temp.path;
    form_route(server, "/form", options);
    running_server running(server);

    const auto blob = binary_content(300 * 1024);
    http::form form;
    form.field("name", "sensor-1")
        .field("tags", "a")
        .field("tags", "b")
        .field("empty", "")
        .file("config", R"({"interval": 10})", "config.json", "application/json")
        .file("blob", blob, "../../evil.bin");

    http::client client;
    auto response = client.post("http://127.0.0.1:" + std::to_string(running.port) + "/form", form);
    INFO(response.body().substr(0, 500));
    REQUIRE(response.status() == 200);
    auto result = json::parse(response.body());

    REQUIRE(result["fields"] == json::array({{"name", "sensor-1"}, {"tags", "a"}, {"tags", "b"}, {"empty", ""}}));
    REQUIRE(result["files"].size() == 2);
    auto config = result["files"][0];
    REQUIRE(config["name"] == "config");
    REQUIRE(config["filename"] == "config.json");
    REQUIRE(config["type"] == "application/json");
    REQUIRE(config["content"] == R"({"interval": 10})");
    REQUIRE(config["size"] == 16);
    auto evil = result["files"][1];
    REQUIRE(evil["filename"] == "../../evil.bin");
    REQUIRE(evil["safe"] == "evil.bin");
    REQUIRE(evil["size"] == blob.size());
    REQUIRE(fs::path(evil["path"].get<std::string>()).parent_path() == temp.path);
    // (the content is binary: compared through its size and a temporary file read by the test below)

    // Deleted once the handler ends
    REQUIRE(eventually_empty(temp));
    REQUIRE_FALSE(fs::exists(evil["path"].get<std::string>()));
}

TEST_CASE("Multipart files kept by the handler", "[multipart][integration]") {
    http::server server;
    temp_directory temp;
    temp_directory destination;
    const auto blob = binary_content(100 * 1024, 7);
    server.post("/upload", [&](http::request& req, http::response& res) -> thinger::awaitable<void> {
        http::multipart_options options;
        options.temp_directory = temp.path;
        auto form = co_await req.read_multipart(options);
        if (!form) {
            res.error(form.status(), form.message());
            co_return;
        }
        auto* upload = form.file("upload");
        auto* other = form.file("other");
        if (!upload || !other || form.file("none")) {
            res.error(http::http_response::status::bad_request, "missing file");
            co_return;
        }
        // One moved to its destination, under its safe name; the other kept where it is
        bool moved = upload->move_to(destination.path / upload->safe_filename());
        auto kept = other->release();
        fs::rename(kept, destination.path / "kept.bin");
        res.json({{"moved", moved}, {"path", upload->path().string()}});
    });
    running_server running(server);

    auto body = file("upload", "dir/photo.jpg", blob, "image/jpeg") + file("other", "x", "kept") + close();
    auto result = raw_exchange(running.port, post("/upload", body));
    INFO(result.data.substr(0, 300));
    REQUIRE(result.data.starts_with("HTTP/1.1 200"));
    auto answer = json::parse(response_body(result.data));
    REQUIRE(answer["moved"] == true);
    REQUIRE(answer["path"] == (destination.path / "photo.jpg").string());
    REQUIRE(read_file(destination.path / "photo.jpg") == blob);
    REQUIRE(read_file(destination.path / "kept.bin") == "kept");
    REQUIRE(eventually_empty(temp));
    REQUIRE(destination.files() == 2);
}

TEST_CASE("Multipart bodies streamed with multipart_reader", "[multipart][integration]") {
    http::server server;
    server.post("/stream", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
        http::multipart_reader reader(req, {}, 1000);   // a small buffer: many reads per part
        json parts = json::array();
        while (const http::multipart_part* part = co_await reader.next()) {
            std::string content;
            size_t pieces = 0;
            for (auto data = co_await reader.read(); !data.empty(); data = co_await reader.read()) {
                content.append(data);
                ++pieces;
            }
            parts.push_back({{"name", part->name}, {"file", part->is_file()}, {"size", content.size()},
                             {"content", content.size() < 100 ? content : ""}, {"many", pieces > 1}});
        }
        if (reader.failed()) {
            res.error(reader.status(), reader.message());
            co_return;
        }
        res.json({{"parts", parts}, {"type", reader.media_type()}, {"done", reader.done()}});
    });
    running_server running(server);

    const auto big = binary_content(50000, 3);
    const auto body = "preamble\r\n" + field("a", "1") + file("big", "big.bin", big) + field("b", "\r\n--" + boundary + "x") +
                      close() + "epilogue";

    auto check = [&](const exchange_result& result) {
        INFO(result.data.substr(0, 400));
        REQUIRE(result.data.starts_with("HTTP/1.1 200"));
        auto answer = json::parse(response_body(result.data));
        REQUIRE(answer["done"] == true);
        REQUIRE(answer["type"] == "multipart/form-data");
        REQUIRE(answer["parts"].size() == 3);
        REQUIRE(answer["parts"][0]["content"] == "1");
        REQUIRE(answer["parts"][1]["file"] == true);
        REQUIRE(answer["parts"][1]["size"] == big.size());
        REQUIRE(answer["parts"][1]["many"] == true);
        REQUIRE(answer["parts"][2]["content"] == "\r\n--" + boundary + "x");
    };

    SECTION("Content-Length") {
        check(raw_exchange(running.port, post("/stream", body)));
    }

    SECTION("Chunked, in chunks that split delimiters") {
        std::string chunked;
        for (size_t offset = 0; offset < body.size(); offset += 777) {
            auto piece = body.substr(offset, 777);
            char size[16];
            std::snprintf(size, sizeof(size), "%zx", piece.size());
            chunked += std::string(size) + "\r\n" + piece + "\r\n";
        }
        chunked += "0\r\n\r\n";
        check(raw_exchange(running.port,
            "POST /stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Type: multipart/form-data; boundary=" +
            boundary + "\r\nTransfer-Encoding: chunked\r\n\r\n" + chunked));
    }

    SECTION("Any multipart type") {
        auto result = raw_exchange(running.port, post("/stream", field("a", "1") + close(), "",
                                                       "multipart/mixed; boundary=" + boundary));
        REQUIRE(result.data.starts_with("HTTP/1.1 200"));
        REQUIRE(json::parse(response_body(result.data))["type"] == "multipart/mixed");
    }
}

TEST_CASE("Multipart errors", "[multipart][integration]") {
    http::server server;
    temp_directory temp;
    http::multipart_options options;
    options.temp_directory = temp.path;
    options.limits.max_file_size = 1000;
    options.limits.max_parts = 5;
    form_route(server, "/form", options);
    server.post("/limited", [options](http::request& req, http::response& res) -> thinger::awaitable<void> {
        auto form = co_await req.read_multipart(options);
        if (!form) res.error(form.status(), form.message());
        else res.send("ok");
    }).max_body_size(2000);
    running_server running(server);

    auto status = [&](const std::string& request) {
        auto result = raw_exchange(running.port, request);
        INFO(result.data.substr(0, 300));
        REQUIRE(result.data.size() >= 12);
        return std::stoi(result.data.substr(9, 3));
    };
    auto message = [&](const std::string& request) {
        return response_body(raw_exchange(running.port, request).data);
    };

    SECTION("Not a form") {
        REQUIRE(status(post("/form", "{}", "", "application/json")) == 415);
        REQUIRE(status(post("/form", field("a", "1") + close(), "", "multipart/mixed; boundary=" + boundary)) == 415);
        REQUIRE(status(post("/form", field("a", "1") + close(), "", "multipart/form-data; boundary=" + boundary + "; x=\"")) == 415);
        // Several Content-Type headers
        REQUIRE(status(post("/form", field("a", "1") + close(), "Content-Type: text/plain\r\n")) == 415);
        // A compressed body is not decoded
        auto compressed = util::gzip::compress(field("a", "1") + close());
        REQUIRE(status(post("/form", *compressed, "Content-Encoding: gzip\r\n")) == 415);
        REQUIRE(status(post("/form", field("a", "1") + close(), "Content-Encoding: identity\r\n")) == 200);
    }

    SECTION("Boundaries") {
        REQUIRE(status(post("/form", field("a", "1") + close(), "", "multipart/form-data")) == 400);
        REQUIRE(status(post("/form", field("a", "1") + close(), "", "multipart/form-data; boundary=\"\"")) == 400);
        REQUIRE(status(post("/form", "--a\"b\r\n\r\n\r\n--a\"b--", "", "multipart/form-data; boundary=\"a\\\"b\"")) == 400);
        REQUIRE(status(post("/form", field("a", "1") + close(), "",
                            "multipart/form-data; boundary=" + boundary + "; boundary=" + boundary)) == 400);
        REQUIRE(status(post("/form", "--" + std::string(71, 'x') + "--", "", "multipart/form-data; boundary=" + std::string(71, 'x'))) == 400);
        // Quoted, with characters a token cannot have, and case-insensitive names
        std::string quoted = "a:b=c?d/e f";
        auto body = "--" + quoted + "\r\nContent-Disposition: form-data; name=\"x\"\r\n\r\n1\r\n--" + quoted + "--";
        auto answer = message(post("/form", body, "", "Multipart/Form-Data; BOUNDARY=\"" + quoted + "\""));
        REQUIRE(json::parse(answer)["fields"] == json::array({{"x", "1"}}));
    }

    SECTION("Malformed and truncated bodies") {
        REQUIRE(status(post("/form", field("a", "1"))) == 400);                             // no closing delimiter
        REQUIRE(status(post("/form", "")) == 400);
        REQUIRE(status(post("/form", field("a", "1") + "--" + boundary + "--x")) == 400);
        REQUIRE(status(post("/form", "--" + boundary + "\r\nContent-Disposition: form-data;\r\n name=\"a\"\r\n\r\n1\r\n" + close())) == 400);
        REQUIRE(message(post("/form", field("a", "1"))) == "Incomplete multipart body");
        REQUIRE(message(post("/form", field("a", "1") + "--" + boundary + "--x")) == "Malformed multipart body");
        // The connection ends before the body
        auto request = post("/form", field("a", "1") + close());
        boost::asio::io_context ioc;
        boost::asio::ip::tcp::socket sock(ioc);
        sock.connect({boost::asio::ip::make_address("127.0.0.1"), running.port});
        boost::asio::write(sock, boost::asio::buffer(request.substr(0, request.size() - 5)));
        sock.shutdown(boost::asio::ip::tcp::socket::shutdown_send);
        auto result = read_until_closed(ioc, sock, 3s);
        REQUIRE(result.data.starts_with("HTTP/1.1 400"));
    }

    SECTION("Limits, and the files written before them deleted") {
        auto body = file("first", "a.bin", std::string(1000, 'x')) + file("second", "b.bin", std::string(1001, 'y')) + close();
        REQUIRE(status(post("/form", body)) == 413);
        REQUIRE(message(post("/form", body)) == "File too large");
        REQUIRE(eventually_empty(temp));

        std::string parts;
        for (int i = 0; i < 6; ++i) parts += field("f" + std::to_string(i), "v");
        REQUIRE(status(post("/form", parts + close())) == 413);
        REQUIRE(message(post("/form", parts + close())) == "Too many parts");
    }

    SECTION("The maximum body size of the route") {
        REQUIRE(status(post("/limited", file("f", "a.bin", std::string(1000, 'x')) + close())) == 200);
        auto large = file("f", "a.bin", std::string(1000, 'x')) + file("g", "b.bin", std::string(1000, 'x')) + close();
        REQUIRE(status(post("/limited", large)) == 413);
        REQUIRE(message(post("/limited", large)) == "Payload Too Large");
        REQUIRE(eventually_empty(temp));
    }

    SECTION("Parts that are not form fields are skipped") {
        auto body = "--" + boundary + "\r\n\r\nno headers\r\n" +
                    "--" + boundary + "\r\nContent-Disposition: attachment; name=\"a\"\r\n\r\nnot form-data\r\n" +
                    "--" + boundary + "\r\nContent-Disposition: form-data; filename=\"f.txt\"\r\n\r\nno name\r\n" +
                    field("kept", "yes") + close();
        auto answer = json::parse(message(post("/form", body)));
        REQUIRE(answer["fields"] == json::array({{"kept", "yes"}}));
        REQUIRE(answer["files"].empty());
    }
}

TEST_CASE("Multipart bodies read by the server first", "[multipart][integration]") {
    http::server server;
    // Not deferred: the server reads the body (decompressing it), then the handler parses it
    server.post("/read", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
        auto form = co_await req.read_multipart();
        if (!form) res.error(form.status(), form.message());
        else res.json(describe(form)["fields"]);
    }).deferred_body(false);
    running_server running(server);

    auto body = field("a", "1") + field("b", "2") + close();
    auto result = raw_exchange(running.port, post("/read", body));
    REQUIRE(result.data.starts_with("HTTP/1.1 200"));
    REQUIRE(json::parse(response_body(result.data)) == json::array({{"a", "1"}, {"b", "2"}}));

    // gzip, decoded by the server
    auto compressed = util::gzip::compress(body);
    result = raw_exchange(running.port, post("/read", *compressed, "Content-Encoding: gzip\r\n"));
    REQUIRE(result.data.starts_with("HTTP/1.1 200"));
    REQUIRE(json::parse(response_body(result.data)) == json::array({{"a", "1"}, {"b", "2"}}));

    // In memory
    auto request = make_request(http::method::POST, "/read");
    request->set_content(body, "multipart/form-data; boundary=" + boundary);
    auto response = run_dispatch(server, request);
    REQUIRE(response->get_status() == http::http_response::status::ok);
    REQUIRE(json::parse(response->get_content()) == json::array({{"a", "1"}, {"b", "2"}}));
}

TEST_CASE("Multipart body decoder", "[multipart][body][integration]") {
    http::server server;
    server.post("/decoded", [](json& body, http::response& res) {
        res.json(body);
    }).accepts({"form", "multipart"});
    server.post("/json-only", [](json& body, http::response& res) {
        res.json(body);
    });
    running_server running(server);

    auto body = field("name", "sensor-1") + field("tags", "a") + field("tags", "b") +
                file("config", "../c.json", "{}", "application/json") + close();
    auto result = raw_exchange(running.port, post("/decoded", body));
    INFO(result.data);
    REQUIRE(result.data.starts_with("HTTP/1.1 200"));
    auto answer = json::parse(response_body(result.data));
    REQUIRE(answer["name"] == "sensor-1");
    REQUIRE(answer["tags"] == json::array({"a", "b"}));
    REQUIRE(answer["config"]["filename"] == "c.json");
    REQUIRE(answer["config"]["content_type"] == "application/json");
    REQUIRE(answer["config"]["content"]["bytes"] == json::array({123, 125}));

    // The same as a urlencoded form
    result = raw_exchange(running.port, post("/decoded", "name=sensor-1&tags=a&tags=b", "", "application/x-www-form-urlencoded"));
    REQUIRE(json::parse(response_body(result.data)) == json({{"name", "sensor-1"}, {"tags", {"a", "b"}}}));

    // Errors
    REQUIRE(raw_exchange(running.port, post("/decoded", field("a", "1"))).data.starts_with("HTTP/1.1 400"));
    REQUIRE(raw_exchange(running.port, post("/decoded", field("a", "\xff") + close())).data.starts_with("HTTP/1.1 400"));
    REQUIRE(raw_exchange(running.port, post("/decoded", field("a", std::string(1024 * 1024 + 1, 'x')) + close()))
                .data.starts_with("HTTP/1.1 413"));
    // Only routes accepting it
    REQUIRE(raw_exchange(running.port, post("/json-only", body)).data.starts_with("HTTP/1.1 415"));
}

TEST_CASE("Multipart uploads with curl -F", "[multipart][integration][curl]") {
    if (std::system("curl --version > /dev/null 2>&1") != 0) {
        SKIP("curl is not available");
    }
    http::server server;
    temp_directory temp;
    http::multipart_options options;
    options.temp_directory = temp.path;
    form_route(server, "/form", options);
    running_server running(server);

    temp_directory sources;
    const auto blob = binary_content(2 * 1024 * 1024, 11);
    {
        std::ofstream out(sources.path / "data.bin", std::ios::binary);
        out << blob;
    }
    auto output = sources.path / "response.json";
    auto command = "curl -s -o " + output.string() + " -F name=sensor-1 -F 'note=two words' -F 'data=@" +
                   (sources.path / "data.bin").string() + ";type=application/x-thing' -F 'other=@" +
                   (sources.path / "data.bin").string() + ";filename=../renamed.bin' http://127.0.0.1:" +
                   std::to_string(running.port) + "/form";
    REQUIRE(std::system(command.c_str()) == 0);
    auto result = json::parse(read_file(output));
    REQUIRE(result["fields"] == json::array({{"name", "sensor-1"}, {"note", "two words"}}));
    REQUIRE(result["files"].size() == 2);
    REQUIRE(result["files"][0]["filename"] == "data.bin");
    REQUIRE(result["files"][0]["type"] == "application/x-thing");
    REQUIRE(result["files"][0]["size"] == blob.size());
    REQUIRE(result["files"][1]["safe"] == "renamed.bin");
    REQUIRE(result["files"][1]["size"] == blob.size());
    REQUIRE(eventually_empty(temp));
}

#ifdef __linux__
namespace {

    size_t resident_bytes() {
        std::ifstream statm("/proc/self/statm");
        size_t pages = 0;
        size_t resident = 0;
        statm >> pages >> resident;
        return resident * static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    }

    uint64_t fnv1a(uint64_t hash, std::string_view data) {
        for (unsigned char c : data) {
            hash ^= c;
            hash *= 0x100000001b3ULL;
        }
        return hash;
    }

    // Send a multipart body with one file of `size` bytes generated on the fly, and return the
    // response and the FNV-1a hash of the content sent; the peak resident memory growth
    // while uploading is stored in `growth`
    std::pair<std::string, uint64_t> upload(uint16_t port, const std::string& path, size_t size, size_t& growth) {
        const std::string head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"big\"; filename=\"big.bin\"\r\n"
                                 "Content-Type: application/octet-stream\r\n\r\n";
        const std::string tail = "\r\n" + close();
        auto request = "POST " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Type: multipart/form-data; boundary=" +
                       boundary + "\r\nContent-Length: " + std::to_string(head.size() + size + tail.size()) + "\r\n\r\n" + head;

        size_t baseline = resident_bytes();
        std::atomic<bool> uploading{true};
        std::atomic<size_t> peak{baseline};
        std::thread sampler([&] {
            while (uploading) {
                size_t now = resident_bytes();
                if (now > peak) peak = now;
                std::this_thread::sleep_for(5ms);
            }
        });

        boost::asio::io_context ioc;
        boost::asio::ip::tcp::socket sock(ioc);
        sock.connect({boost::asio::ip::make_address("127.0.0.1"), port});
        boost::asio::write(sock, boost::asio::buffer(request));
        // Content with CRLF and dashes now and then, but never the boundary
        auto block = binary_content(64 * 1024, 5);
        for (size_t i = 0; i + 4 < block.size(); i += 4093) block.replace(i, 4, "\r\n--");
        uint64_t hash = 0xcbf29ce484222325ULL;
        for (size_t sent = 0; sent < size;) {
            size_t n = std::min(block.size(), size - sent);
            boost::asio::write(sock, boost::asio::buffer(block.data(), n));
            hash = fnv1a(hash, std::string_view(block.data(), n));
            sent += n;
        }
        boost::asio::write(sock, boost::asio::buffer(tail));
        auto result = read_until_closed(ioc, sock, 60s);
        uploading = false;
        sampler.join();
        growth = peak - baseline;
        return {result.data, hash};
    }

}

TEST_CASE("Large multipart uploads in bounded memory", "[multipart][integration][large]") {
    http::server server;
    temp_directory temp;
    server.post("/stream", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
        http::multipart_reader reader(req);
        uint64_t hash = 0xcbf29ce484222325ULL;
        size_t size = 0;
        while (co_await reader.next()) {
            for (auto data = co_await reader.read(); !data.empty(); data = co_await reader.read()) {
                hash = fnv1a(hash, data);
                size += data.size();
            }
        }
        if (reader.failed()) res.error(reader.status(), reader.message());
        else res.json({{"size", size}, {"hash", std::to_string(hash)}});
    }).max_body_size(1024 * 1024 * 1024);
    server.post("/form", [&temp](http::request& req, http::response& res) -> thinger::awaitable<void> {
        http::multipart_options options;
        options.temp_directory = temp.path;
        auto form = co_await req.read_multipart(options);
        if (!form) {
            res.error(form.status(), form.message());
            co_return;
        }
        // Hashed from the temporary file
        std::ifstream in(form.files().at(0).path(), std::ios::binary);
        std::vector<char> buffer(1024 * 1024);
        uint64_t hash = 0xcbf29ce484222325ULL;
        while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0) {
            hash = fnv1a(hash, std::string_view(buffer.data(), static_cast<size_t>(in.gcount())));
        }
        res.json({{"size", form.files().at(0).size()}, {"hash", std::to_string(hash)}});
    }).max_body_size(1024 * 1024 * 1024);
    running_server running(server);

    const size_t limit = 64 * 1024 * 1024;

    SECTION("Streamed") {
        const size_t size = 300 * 1024 * 1024;
        size_t growth = 0;
        auto [response, hash] = upload(running.port, "/stream", size, growth);
        INFO(response.substr(0, 300));
        REQUIRE(response.starts_with("HTTP/1.1 200"));
        auto answer = json::parse(response_body(response));
        REQUIRE(answer["size"] == size);
        REQUIRE(answer["hash"] == std::to_string(hash));
        INFO("resident memory growth: " << growth);
        REQUIRE(growth < limit);
    }

    SECTION("To a temporary file") {
        const size_t size = 200 * 1024 * 1024;
        size_t growth = 0;
        auto [response, hash] = upload(running.port, "/form", size, growth);
        INFO(response.substr(0, 300));
        REQUIRE(response.starts_with("HTTP/1.1 200"));
        auto answer = json::parse(response_body(response));
        REQUIRE(answer["size"] == size);
        REQUIRE(answer["hash"] == std::to_string(hash));
        INFO("resident memory growth: " << growth);
        REQUIRE(growth < limit);
        REQUIRE(eventually_empty(temp));
    }
}
#endif

#include <catch2/catch_test_macros.hpp>
#include <thinger/http/common/syntax.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>

#include "server_test_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Headers set by the application with a name that is not a token, or a value with CR, LF,
// NUL or another control character, are refused: a response is sent without them, so they
// never add a line to its head. Checked on the raw bytes the server sends.

using namespace thinger;
using namespace std::chrono_literals;
using server_test::raw_exchange;
using server_test::running_server;

namespace {

    struct raw_response {
        std::string status_line;
        std::vector<std::string> header_lines;  // without Date, which changes between responses
        std::string body;

        bool has_line(const std::string& line) const {
            return std::find(header_lines.begin(), header_lines.end(), line) != header_lines.end();
        }

        bool has_header_starting(const std::string& prefix) const {
            return std::any_of(header_lines.begin(), header_lines.end(),
                               [&](const std::string& line) { return line.rfind(prefix, 0) == 0; });
        }
    };

    // The responses in `data`, each framed by its Content-Length
    std::vector<raw_response> parse_responses(const std::string& data) {
        std::vector<raw_response> responses;
        size_t position = 0;
        while (position < data.size()) {
            auto end = data.find("\r\n\r\n", position);
            if (end == std::string::npos) break;
            raw_response response;
            size_t content_length = 0;
            auto line_start = position;
            bool first = true;
            while (line_start < end + 2) {
                auto line_end = data.find("\r\n", line_start);
                auto line = data.substr(line_start, line_end - line_start);
                line_start = line_end + 2;
                if (first) {
                    response.status_line = line;
                    first = false;
                    continue;
                }
                if (line.rfind("Content-Length: ", 0) == 0) content_length = std::stoul(line.substr(16));
                if (line.rfind("Date: ", 0) != 0) response.header_lines.push_back(line);
            }
            response.body = data.substr(end + 4, content_length);
            position = end + 4 + content_length;
            responses.push_back(std::move(response));
        }
        return responses;
    }

    // GET `path` and then /control on the same connection, which the second closes
    std::vector<raw_response> get_twice(const running_server& running, const std::string& path) {
        auto result = raw_exchange(running, "GET " + path + " HTTP/1.1\r\nHost: x\r\n\r\n"
                                            "GET /control HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        auto responses = parse_responses(result.data);
        REQUIRE(responses.size() == 2);
        // the second response is the control one, intact
        REQUIRE(responses[1].status_line == "HTTP/1.1 200 OK");
        REQUIRE(responses[1].has_line("X-A: ok"));
        REQUIRE(responses[1].body == "body");
        return responses;
    }

    const std::vector<std::string> invalid_values = {
        "v\r\nX-Injected: 1", "v\nX-Injected: 1", "v\rX-Injected: 1", std::string("v\0X", 3), "v\x7fX", "v\x01X",
    };

    const std::vector<std::string> invalid_names = {
        "X-A\r\nX-B", "X\nB", "X\rB", std::string("X\0B", 3), "X A", "X:A", "Caf\xc3\xa9",
    };

}

TEST_CASE("Response headers with invalid names or values are not sent", "[server][headers][integration]") {
    http::server server;
    server.get("/control", [](http::response& res) {
        res.header("X-A", "ok");
        res.send("body");
    });
    server.get("/value/:index", [](http::request& req, http::response& res) {
        res.header("X-A", "ok");
        res.header("X-Bad", invalid_values.at(std::stoul(req["index"])));
        res.send("body");
    });
    server.get("/name/:index", [](http::request& req, http::response& res) {
        res.header("X-A", "ok");
        res.header(invalid_names.at(std::stoul(req["index"])), "value");
        res.send("body");
    });
    server.get("/set", [](http::response& res) {
        auto response = std::make_shared<http::http_response>();
        response->add_header("X-A", "first");
        response->set_header("X-A", "bad\nvalue");
        response->set_content("body", "text/plain");
        res.send_response(response);
    });
    server.get("/stream", [](http::response& res) -> thinger::awaitable<void> {
        http::http_response head;
        head.add_header("X-A", "ok");
        head.add_header("X-Bad", "v\r\nX-Injected: 1");
        head.add_header("X-Bad\r\nX-Injected", "1");
        if (!res.start_stream(head, 4)) co_return;
        co_await res.write_stream("body");
        co_await res.end_stream();
    });
    server.get("/valid", [](http::response& res) {
        res.header("X-Tab", "a\tb");
        res.header("X-Obs", "caf\xc3\xa9");
        res.header("X-Empty", "");
        res.header("!#$%&'*+-.^_`|~", "symbols");
        res.send("body");
    });
    running_server running(server);
    auto control = get_twice(running, "/control")[0];

    SECTION("An invalid value: the header is left out, the response is the same") {
        for (size_t i = 0; i < invalid_values.size(); i++) {
            INFO("value " << i);
            auto response = get_twice(running, "/value/" + std::to_string(i))[0];
            REQUIRE(response.status_line == control.status_line);
            REQUIRE(response.header_lines == control.header_lines);
            REQUIRE(response.body == "body");
            REQUIRE_FALSE(response.has_header_starting("X-Bad"));
            REQUIRE_FALSE(response.has_header_starting("X-Injected"));
        }
    }

    SECTION("An invalid name: the header is left out, the response is the same") {
        for (size_t i = 0; i < invalid_names.size(); i++) {
            INFO("name " << i);
            auto response = get_twice(running, "/name/" + std::to_string(i))[0];
            REQUIRE(response.status_line == control.status_line);
            REQUIRE(response.header_lines == control.header_lines);
            REQUIRE(response.body == "body");
        }
    }

    SECTION("A refused set_header keeps the previous value") {
        auto response = get_twice(running, "/set")[0];
        REQUIRE(response.has_line("X-A: first"));
        REQUIRE(response.body == "body");
    }

    SECTION("The head of start_stream() sends only its valid headers") {
        auto response = get_twice(running, "/stream")[0];
        REQUIRE(response.status_line == "HTTP/1.1 200 OK");
        REQUIRE(response.has_line("X-A: ok"));
        REQUIRE_FALSE(response.has_header_starting("X-Bad"));
        REQUIRE_FALSE(response.has_header_starting("X-Injected"));
        REQUIRE(response.body == "body");
    }

    SECTION("Valid values and names are sent byte for byte") {
        auto response = get_twice(running, "/valid")[0];
        REQUIRE(response.has_line("X-Tab: a\tb"));
        REQUIRE(response.has_line("X-Obs: caf\xc3\xa9"));
        REQUIRE(response.has_line("X-Empty: "));
        REQUIRE(response.has_line("!#$%&'*+-.^_`|~: symbols"));
    }
}

TEST_CASE("redirect() percent-encodes the bytes a header value cannot hold", "[server][headers][integration]") {
    const std::vector<std::string> valid_targets = {
        "/a?b=c&d=%20e", "https://example.com/x#y", "/caf\xc3\xa9 x", "/a\tb",
    };
    http::server server;
    server.get("/control", [](http::response& res) {
        res.header("X-A", "ok");
        res.send("body");
    });
    server.get("/go", [](http::request& req, http::response& res) { res.redirect(req.query("next")); });
    server.get("/valid/:index", [&valid_targets](http::request& req, http::response& res) {
        res.redirect(valid_targets.at(std::stoul(req["index"])));
    });
    running_server running(server);

    auto locations = [](const raw_response& response) {
        return std::count_if(response.header_lines.begin(), response.header_lines.end(),
                             [](const std::string& line) { return line.rfind("Location: ", 0) == 0; });
    };

    SECTION("CR and LF are encoded: one Location line, nothing else added") {
        auto response = get_twice(running, "/go?next=/home%0D%0AX-Injected:%201")[0];
        REQUIRE(response.status_line == "HTTP/1.1 302 Moved Temporarily");
        REQUIRE(locations(response) == 1);
        REQUIRE(response.has_line("Location: /home%0D%0AX-Injected: 1"));
        REQUIRE_FALSE(response.has_header_starting("X-Injected"));
        REQUIRE(response.has_line("Content-Length: 0"));
        REQUIRE(response.body.empty());
    }

    SECTION("NUL, a lone LF or CR, DEL and other control bytes are encoded in uppercase hex") {
        auto response = get_twice(running, "/go?next=/a%00b%0Ac%0Dd%7Fe%01f%1Fg")[0];
        REQUIRE(response.status_line == "HTTP/1.1 302 Moved Temporarily");
        REQUIRE(locations(response) == 1);
        REQUIRE(response.has_line("Location: /a%00b%0Ac%0Dd%7Fe%01f%1Fg"));
    }

    SECTION("Valid targets are sent byte for byte") {
        for (size_t i = 0; i < valid_targets.size(); i++) {
            INFO("target " << i);
            auto response = get_twice(running, "/valid/" + std::to_string(i))[0];
            REQUIRE(response.status_line == "HTTP/1.1 302 Moved Temporarily");
            REQUIRE(locations(response) == 1);
            REQUIRE(response.has_line("Location: " + valid_targets[i]));
        }
    }
}

TEST_CASE("Downloads name the file in a single, well-formed Content-Disposition", "[server][headers][integration]") {
    namespace fs = std::filesystem;
    struct download {
        std::string name;
        std::string disposition;
    };
    const std::vector<download> downloads = {
        {"report.pdf", "attachment; filename=\"report.pdf\""},
        {"a\"b\\c.txt", "attachment; filename=\"a\\\"b\\\\c.txt\""},
        {"\xe2\x82\xacuro.txt", "attachment; filename=\"?uro.txt\"; filename*=UTF-8''%E2%82%ACuro.txt"},
        {"in\r\nfo.txt", "attachment; filename=\"in??fo.txt\"; filename*=UTF-8''in%0D%0Afo.txt"},
        {"100%41.txt", "attachment; filename=\"100%41.txt\"; filename*=UTF-8''100%2541.txt"},
        {"caf\xc3\xa9 & co's (1).txt",
         "attachment; filename=\"caf? & co's (1).txt\"; filename*=UTF-8''caf%C3%A9%20&%20co%27s%20%281%29.txt"},
    };

    auto base = fs::temp_directory_path() /
                ("thinger_downloads_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(base);
    for (const auto& file : downloads) std::ofstream(base / file.name, std::ios::binary) << "data";

    http::server server;
    server.get("/control", [](http::response& res) {
        res.header("X-A", "ok");
        res.send("body");
    });
    server.get("/download/:index", [&](http::request& req, http::response& res) {
        res.send_file(base / downloads.at(std::stoul(req["index"])).name, true);
    });
    {
        running_server running(server);
        for (size_t i = 0; i < downloads.size(); i++) {
            INFO("file " << i);
            auto response = get_twice(running, "/download/" + std::to_string(i))[0];
            REQUIRE(response.status_line == "HTTP/1.1 200 OK");
            REQUIRE(response.body == "data");
            std::vector<std::string> dispositions;
            for (const auto& line : response.header_lines) {
                if (line.rfind("Content-Disposition: ", 0) == 0) dispositions.push_back(line.substr(21));
            }
            REQUIRE(dispositions.size() == 1);
            REQUIRE(dispositions[0] == downloads[i].disposition);
            REQUIRE(http::syntax::is_field_value(dispositions[0]));
            REQUIRE_FALSE(response.has_header_starting("fo.txt"));

            // filename*: only attr-char and %XX escapes in uppercase hex
            auto extended = dispositions[0].find("filename*=UTF-8''");
            if (extended == std::string::npos) continue;
            auto encoded = dispositions[0].substr(extended + 17);
            for (size_t j = 0; j < encoded.size(); j++) {
                unsigned char c = encoded[j];
                if (c == '%') {
                    REQUIRE(j + 2 < encoded.size());
                    for (char digit : encoded.substr(j + 1, 2)) {
                        REQUIRE(((digit >= '0' && digit <= '9') || (digit >= 'A' && digit <= 'F')));
                    }
                    j += 2;
                } else {
                    REQUIRE(http::syntax::is_token_char(c));
                    REQUIRE(c != '*');
                    REQUIRE(c != '\'');
                }
            }
        }
    }
    std::error_code ec;
    fs::remove_all(base, ec);
}

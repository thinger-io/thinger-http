#include "server_test_helpers.hpp"
#include "../fixtures/listener_helpers.hpp"
#include <catch2/generators/catch_generators.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/common/syntax.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/reverse_proxy.hpp>
#include <thinger/util/ascii.hpp>
#include <boost/system/errc.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <unistd.h>
#include <utility>
#include <vector>

// CORS of an application (http_application::enable_cors), as cors() in Express and rs/cors, and
// the Fetch standard where they differ

using namespace thinger;
using namespace std::chrono_literals;
using server_test::running_server;

namespace {

    using header_list = std::vector<std::pair<std::string, std::string>>;

    // A response as it came on the wire: what a client merging its headers would hide
    struct answer {
        int status = 0;
        header_list headers;
        bool closed = false;

        size_t count(const std::string& name) const {
            size_t n = 0;
            for (const auto& [key, value] : headers) {
                if (util::ascii::iequals(key, name)) n++;
            }
            return n;
        }

        std::string header(const std::string& name) const {
            for (const auto& [key, value] : headers) {
                if (util::ascii::iequals(key, name)) return value;
            }
            return {};
        }

        bool has_cors_header() const {
            for (const auto& [key, value] : headers) {
                if (util::ascii::istarts_with(key, "access-control-")) return true;
            }
            return false;
        }

        // Times `field` is listed in Vary
        size_t vary(const std::string& field) const {
            size_t n = 0;
            for (const auto& [key, value] : headers) {
                if (!util::ascii::iequals(key, "Vary")) continue;
                http::syntax::for_each_element(value, [&](std::string_view item) {
                    if (util::ascii::iequals(item, field)) n++;
                });
            }
            return n;
        }

        // No response of the library ever pairs them (FR-016)
        bool valid_pair() const {
            return !(header("Access-Control-Allow-Origin") == "*" && count("Access-Control-Allow-Credentials") > 0);
        }
    };

    answer parse_answer(const std::string& data, bool closed = false) {
        answer result;
        result.closed = closed;
        auto end = data.find("\r\n\r\n");
        auto head = data.substr(0, end);
        auto line_end = head.find("\r\n");
        auto status_line = head.substr(0, line_end);
        if (status_line.size() >= 12) result.status = std::stoi(status_line.substr(9, 3));
        while (line_end != std::string::npos) {
            auto start = line_end + 2;
            line_end = head.find("\r\n", start);
            auto line = head.substr(start, line_end == std::string::npos ? std::string::npos : line_end - start);
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            result.headers.emplace_back(line.substr(0, colon), std::string(http::syntax::trim(std::string_view(line).substr(colon + 1))));
        }
        return result;
    }

    std::string make_raw(const std::string& method, const std::string& path, const header_list& headers,
                         const std::string& body, bool close) {
        std::string raw = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n";
        if (close) raw += "Connection: close\r\n";
        for (const auto& [key, value] : headers) raw += key + ": " + value + "\r\n";
        if (!body.empty()) raw += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        return raw + "\r\n" + body;
    }

    // One request on a new connection, answered and closed
    answer send(const running_server& server, const std::string& method, const std::string& path,
                const header_list& headers = {}, const std::string& body = "") {
        auto result = server_test::raw_exchange(server, make_raw(method, path, headers, body, true));
        return parse_answer(result.data, result.closed);
    }

    const header_list from_a{{"Origin", "https://a.example"}};

    // An in-memory request with these headers, answered by `server`
    std::shared_ptr<http::http_response> in_memory(http::server& server, http::method method, const std::string& path,
                                                   const header_list& headers = {}, http::dispatch_options options = {}) {
        auto request = server_test::make_request(method, path);
        for (const auto& [key, value] : headers) request->add_header(key, value);
        return server_test::run_dispatch(server, std::move(request), std::move(options));
    }

    // Whether the CORS in force grants the origin: seen with a later request, in memory
    std::string allowed_origin(http::server& server, const std::string& origin = "https://a.example") {
        auto response = in_memory(server, http::method::GET, "/r", {{"Origin", origin}});
        return response->get_header("Access-Control-Allow-Origin");
    }

    bool invalid(const boost::system::error_code& ec) {
        return ec == boost::system::errc::invalid_argument;
    }

    // A temporary file, removed at the end of the test
    struct temp_file {
        std::filesystem::path path;
        explicit temp_file(const std::string& content)
            : path(std::filesystem::temp_directory_path() / ("cors-test-" + std::to_string(::getpid()) + ".txt")) {
            std::ofstream(path) << content;
        }
        ~temp_file() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    };

    // A preflight from origin A asking for PUT, with more headers if given
    header_list preflight_from_a(const header_list& more = {}) {
        header_list headers{{"Origin", "https://a.example"}, {"Access-Control-Request-Method", "PUT"}};
        headers.insert(headers.end(), more.begin(), more.end());
        return headers;
    }

    constexpr const char* default_methods = "GET, HEAD, PUT, PATCH, POST, DELETE";

    // An authorization middleware as an application has it: 401 for any request whose matched
    // route does not say the permission it requires, and a preflight has no credentials anyway
    struct permission_middleware {
        std::atomic<int> ran{0};
        std::mutex mutex;
        std::string permission_seen;

        void attach(http::server& server) {
            server.use([this](http::request& req, http::response& res) -> awaitable<bool> {
                ran++;
                auto* route = req.get_matched_route();
                if (!route || !route->has_meta("permission")) {
                    res.error(http::http_response::status::unauthorized, "Unauthorized");
                    co_return false;
                }
                std::scoped_lock lock(mutex);
                permission_seen = route->get_meta("permission").get<std::string>();
                co_return true;
            });
        }

        std::string seen() {
            std::scoped_lock lock(mutex);
            return permission_seen;
        }
    };

    // Routes answering in each way the library can, with its errors
    void add_routes(http::server& server, const std::filesystem::path& file) {
        server.get("/r", [](http::response& res) { res.send("ok"); });
        server.get("/json", [](http::response& res) { res.json({{"ok", true}}); });
        server.get("/encode", [](http::response& res) { res.encode({{"ok", true}}); });
        server.get("/large", [](http::response& res) { res.json({{"data", std::string(4096, 'a')}}); });
        server.get("/manual", [](http::response& res) {
            auto message = std::make_shared<http::http_response>();
            message->set_content("manual", "text/plain");
            res.send_response(message);
        });
        server.get("/file", [file](http::response& res) { res.send_file(file); });
        server.get("/chunked", [](http::response& res) {
            res.start_chunked("text/plain");
            res.write_chunk("chunk");
            res.end_chunked();
        });
        server.get("/stream", [](http::response& res) -> awaitable<void> {
            res.start_stream(http::http_response{});
            co_await res.write_stream("stream");
            co_await res.end_stream();
        });
        server.get("/redirect", [](http::response& res) { res.redirect("/r"); });
        server.get("/throws", [](http::response&) { throw std::runtime_error("handler failure"); });
        server.get("/slow", [](http::response& res) -> awaitable<void> {
            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 400ms);
            co_await timer.async_wait(use_nothrow_awaitable);
            res.send("late");
        }).timeout(50ms);
        server.post("/limited", [](http::response& res) { res.send("ok"); }).max_body_size(8);
        server.post("/decoded", [](nlohmann::json&, http::response& res) { res.send("ok"); });
        server.use([](http::request& req, http::response& res, std::function<void()> next) {
            if (req.get_http_request()->get_path() == "/guarded") {
                res.error(http::http_response::status::unauthorized, "Unauthorized");
                return;
            }
            next();
        });
    }

    // A streamed response whose head has these headers
    awaitable<void> stream_head(http::response& res, header_list headers) {
        http::http_response head;
        for (const auto& [key, value] : headers) head.add_header(key, value);
        res.start_stream(head);
        co_await res.write_stream("stream");
        co_await res.end_stream();
    }

    // Streamed responses with each form of Vary in their head
    void add_vary_stream_routes(http::server& server) {
        server.get("/head-vary", [](http::response& res) -> awaitable<void> {
            return stream_head(res, {{"Vary", "Accept-Encoding"}});
        });
        server.get("/head-lowercase", [](http::response& res) -> awaitable<void> {
            return stream_head(res, {{"vary", "Accept"}});
        });
        server.get("/head-origin", [](http::response& res) -> awaitable<void> {
            return stream_head(res, {{"Vary", "origin, Cookie"}});
        });
        server.get("/head-two-fields", [](http::response& res) -> awaitable<void> {
            return stream_head(res, {{"Vary", "Accept"}, {"Vary", "Cookie"}});
        });
        server.get("/head-unspaced", [](http::response& res) -> awaitable<void> {
            return stream_head(res, {{"Vary", "Accept,Cookie"}});
        });
        server.get("/head-any", [](http::response& res) -> awaitable<void> {
            return stream_head(res, {{"Vary", "*"}});
        });
        server.get("/head-no-vary", [](http::response& res) -> awaitable<void> {
            return stream_head(res, {});
        });
        server.get("/head-after-header", [](http::response& res) -> awaitable<void> {
            res.header("Vary", "Cookie");
            return stream_head(res, {{"Vary", "Accept"}});
        });
    }

}

// ==================== US1: the defaults, valid for browsers ====================

TEST_CASE("CORS defaults: any origin, and nothing else on a normal response", "[cors][integration]") {
    temp_file file("content");
    http::server server;
    REQUIRE_FALSE(server.enable_cors());
    add_routes(server, file.path);
    running_server running(server);

    SECTION("With Origin") {
        auto response = send(running, "GET", "/r", from_a);
        CHECK(response.status == 200);
        CHECK(response.count("Access-Control-Allow-Origin") == 1);
        CHECK(response.header("Access-Control-Allow-Origin") == "*");
        CHECK(response.count("Access-Control-Allow-Credentials") == 0);
        CHECK(response.count("Access-Control-Allow-Methods") == 0);
        CHECK(response.count("Access-Control-Allow-Headers") == 0);
        CHECK(response.count("Access-Control-Max-Age") == 0);
        CHECK(response.count("Access-Control-Expose-Headers") == 0);
        CHECK(response.vary("Origin") == 0);
    }

    SECTION("Without Origin too, as Express") {
        auto response = send(running, "GET", "/r");
        CHECK(response.status == 200);
        CHECK(response.header("Access-Control-Allow-Origin") == "*");
        CHECK(response.vary("Origin") == 0);
    }

    SECTION("Origin: null, as any other") {
        CHECK(send(running, "GET", "/r", {{"Origin", "null"}}).header("Access-Control-Allow-Origin") == "*");
    }

    SECTION("With the client of the library") {
        http::client client;
        client.timeout(10s);
        auto response = client.request(running.url("/r")).header("Origin", "https://a.example").get();
        REQUIRE(response.ok());
        CHECK(response.header("Access-Control-Allow-Origin") == "*");
        CHECK_FALSE(response.has_header("Access-Control-Allow-Credentials"));
        CHECK_FALSE(response.has_header("Vary"));
    }

    SECTION("On every error of the application") {
        struct error_case {
            const char* method;
            const char* path;
            std::string body;
            int status;
        };
        for (const auto& [method, path, body, status] : {
                 error_case{"GET", "/missing", "", 404},
                 error_case{"DELETE", "/r", "", 405},
                 error_case{"GET", "/throws", "", 500},
                 error_case{"POST", "/limited", std::string(64, 'x'), 413},
                 error_case{"GET", "/slow", "", 504},
                 error_case{"GET", "/guarded", "", 401},
             }) {
            CAPTURE(method, path);
            for (const auto& headers : {from_a, header_list{}}) {
                auto response = send(running, method, path, headers, body);
                CHECK(response.status == status);
                CHECK(response.count("Access-Control-Allow-Origin") == 1);
                CHECK(response.header("Access-Control-Allow-Origin") == "*");
                CHECK(response.valid_pair());
            }
        }
    }

    SECTION("On a body the application refuses") {
        auto invalid_body = send(running, "POST", "/decoded", {{"Content-Type", "application/json"}}, "{");
        CHECK(invalid_body.status == 400);
        CHECK(invalid_body.header("Access-Control-Allow-Origin") == "*");

        auto unsupported = send(running, "POST", "/decoded", {{"Content-Type", "text/plain"}}, "text");
        CHECK(unsupported.status == 415);
        CHECK(unsupported.header("Access-Control-Allow-Origin") == "*");
    }

    SECTION("Once on every form of answer") {
        for (const auto* path : {"/json", "/encode", "/manual", "/file", "/chunked", "/stream", "/redirect"}) {
            CAPTURE(path);
            auto response = send(running, "GET", path, from_a);
            CHECK(response.status / 100 != 4);
            CHECK(response.status / 100 != 5);
            CHECK(response.count("Access-Control-Allow-Origin") == 1);
            CHECK(response.header("Access-Control-Allow-Origin") == "*");
            CHECK(response.count("Access-Control-Allow-Credentials") == 0);
        }
    }

    SECTION("On a request with invalid framing") {
        auto raw = "POST /limited HTTP/1.1\r\nHost: localhost\r\nContent-Length: nope\r\nConnection: close\r\n\r\n";
        auto response = parse_answer(server_test::raw_exchange(running, raw).data);
        CHECK(response.status == 400);
        CHECK(response.header("Access-Control-Allow-Origin") == "*");
    }
}

TEST_CASE("CORS is off by default", "[cors][integration]") {
    temp_file file("content");
    http::server server;
    add_routes(server, file.path);
    bool with_options_route = GENERATE(false, true);
    if (with_options_route) {
        server.options("/r", [](http::response& res) { res.send("routed"); });
    }
    running_server running(server);

    const header_list preflight{{"Origin", "https://a.example"}, {"Access-Control-Request-Method", "PUT"}};

    for (const auto* path : {"/r", "/json", "/manual", "/chunked", "/stream", "/missing", "/throws", "/slow", "/guarded"}) {
        CAPTURE(path);
        CHECK_FALSE(send(running, "GET", path, from_a).has_cors_header());
    }

    // OPTIONS is routed as any other method, a preflight included
    auto response = send(running, "OPTIONS", "/r", preflight);
    CHECK_FALSE(response.has_cors_header());
    CHECK(response.vary("Origin") == 0);
    CHECK(response.status == (with_options_route ? 200 : 405));

    auto missing = send(running, "OPTIONS", "/missing", preflight);
    CHECK_FALSE(missing.has_cors_header());
    CHECK(missing.status == (with_options_route ? 404 : 405));
}

TEST_CASE("CORS can be turned off again", "[cors][integration]") {
    temp_file file("content");
    http::server server;
    add_routes(server, file.path);
    REQUIRE_FALSE(server.enable_cors({.origins = {"https://a.example"}, .credentials = true}));
    CHECK(allowed_origin(server) == "https://a.example");

    server.disable_cors();

    auto response = in_memory(server, http::method::GET, "/r", from_a);
    CHECK_FALSE(response->has_header("Access-Control-Allow-Origin"));
    CHECK_FALSE(response->has_header("Access-Control-Allow-Credentials"));
    CHECK_FALSE(response->has_header("Vary"));
}

// ==================== US2: list or filter, reflected, with Vary and credentials ====================

TEST_CASE("CORS with a list of origins", "[cors][integration]") {
    temp_file file("content");
    http::server server;
    REQUIRE_FALSE(server.enable_cors({.origins = {"https://a.example", "https://b.example", "null"}}));
    add_routes(server, file.path);
    std::atomic<int> handled{0};
    server.get("/counted", [&handled](http::response& res) {
        handled++;
        res.send("ok");
    });
    running_server running(server);

    SECTION("An origin listed is reflected") {
        auto response = send(running, "GET", "/r", {{"Origin", "https://b.example"}});
        CHECK(response.header("Access-Control-Allow-Origin") == "https://b.example");
        CHECK(response.vary("Origin") == 1);
        CHECK(response.count("Access-Control-Allow-Credentials") == 0);
    }

    SECTION("An origin not listed runs the handler, with no grant") {
        auto response = send(running, "GET", "/counted", {{"Origin", "https://evil.example"}});
        CHECK(response.status == 200);
        CHECK(handled == 1);
        CHECK(response.count("Access-Control-Allow-Origin") == 0);
        CHECK(response.count("Access-Control-Allow-Credentials") == 0);
        CHECK(response.count("Access-Control-Expose-Headers") == 0);
        CHECK(response.vary("Origin") == 1);
    }

    SECTION("Without Origin") {
        auto response = send(running, "GET", "/r");
        CHECK(response.count("Access-Control-Allow-Origin") == 0);
        CHECK(response.vary("Origin") == 1);
    }

    SECTION("Case-insensitive in ASCII, reflected as it came") {
        auto response = send(running, "GET", "/r", {{"Origin", "HTTPS://A.Example"}});
        CHECK(response.header("Access-Control-Allow-Origin") == "HTTPS://A.Example");
    }

    SECTION("null only when listed") {
        CHECK(send(running, "GET", "/r", {{"Origin", "null"}}).header("Access-Control-Allow-Origin") == "null");
    }

    SECTION("Several origins are not one of the list") {
        auto joined = send(running, "GET", "/r", {{"Origin", "https://a.example, https://b.example"}});
        CHECK(joined.count("Access-Control-Allow-Origin") == 0);
        CHECK(joined.vary("Origin") == 1);

        auto repeated = send(running, "GET", "/r", {{"Origin", "https://a.example"}, {"Origin", "https://b.example"}});
        CHECK(repeated.count("Access-Control-Allow-Origin") == 0);
        CHECK(repeated.vary("Origin") == 1);
    }

    SECTION("On errors too, granted or not") {
        for (const auto* path : {"/missing", "/throws", "/slow", "/guarded"}) {
            CAPTURE(path);
            auto granted = send(running, "GET", path, from_a);
            CHECK(granted.header("Access-Control-Allow-Origin") == "https://a.example");
            CHECK(granted.vary("Origin") == 1);

            auto denied = send(running, "GET", path, {{"Origin", "https://evil.example"}});
            CHECK(denied.count("Access-Control-Allow-Origin") == 0);
            CHECK(denied.vary("Origin") == 1);
        }
    }

    SECTION("null is not any other origin") {
        http::server others;
        REQUIRE_FALSE(others.enable_cors({.origins = {"https://a.example"}}));
        others.get("/r", [](http::response& res) { res.send("ok"); });
        CHECK(allowed_origin(others, "null").empty());
    }
}

TEST_CASE("CORS with a filter of origins", "[cors][integration]") {
    temp_file file("content");
    http::server server;
    std::mutex mutex;
    std::vector<std::string> seen;
    bool credentials = GENERATE(false, true);
    REQUIRE_FALSE(server.enable_cors({
        .origin_filter = [&](std::string_view origin, const http::http_request& request) {
            std::scoped_lock lock(mutex);
            seen.emplace_back(origin);
            return origin.ends_with(".example.com") && request.get_method() == http::method::GET;
        },
        .credentials = credentials,
    }));
    add_routes(server, file.path);
    running_server running(server);

    auto expected_credentials = credentials ? 1u : 0u;

    SECTION("Accepted") {
        auto response = send(running, "GET", "/r", {{"Origin", "https://x.example.com"}});
        CHECK(response.header("Access-Control-Allow-Origin") == "https://x.example.com");
        CHECK(response.count("Access-Control-Allow-Credentials") == expected_credentials);
        CHECK(response.vary("Origin") == 1);
        std::scoped_lock lock(mutex);
        CHECK(seen == std::vector<std::string>{"https://x.example.com"});
    }

    SECTION("Rejected") {
        auto response = send(running, "GET", "/r", {{"Origin", "https://example.org"}});
        CHECK(response.count("Access-Control-Allow-Origin") == 0);
        CHECK(response.count("Access-Control-Allow-Credentials") == 0);
        CHECK(response.vary("Origin") == 1);
    }

    SECTION("It sees the request") {
        auto response = send(running, "DELETE", "/r", {{"Origin", "https://x.example.com"}});
        CHECK(response.status == 405);
        CHECK(response.count("Access-Control-Allow-Origin") == 0);
    }

    SECTION("Not called without Origin") {
        auto response = send(running, "GET", "/r");
        CHECK(response.count("Access-Control-Allow-Origin") == 0);
        CHECK(response.vary("Origin") == 1);
        std::scoped_lock lock(mutex);
        CHECK(seen.empty());
    }

    SECTION("It gets a value as it came, once per request, repeated Origin joined") {
        send(running, "GET", "/r", {{"Origin", "null"}});
        send(running, "GET", "/r", {{"Origin", "https://a.example, https://b.example"}});
        send(running, "GET", "/r", {{"Origin", "https://a.example"}, {"Origin", "https://b.example"}});
        std::scoped_lock lock(mutex);
        CHECK(seen == std::vector<std::string>{"null", "https://a.example, https://b.example",
                                               "https://a.example, https://b.example"});
    }
}

TEST_CASE("CORS filter granting an origin '*' never gets credentials", "[cors][integration]") {
    http::server server;
    REQUIRE_FALSE(server.enable_cors({
        .origin_filter = [](std::string_view, const http::http_request&) { return true; },
        .credentials = true,
    }));
    server.get("/r", [](http::response& res) { res.send("ok"); });
    running_server running(server);

    auto response = send(running, "GET", "/r", {{"Origin", "*"}});
    CHECK(response.header("Access-Control-Allow-Origin") == "*");
    CHECK(response.count("Access-Control-Allow-Credentials") == 0);
}

TEST_CASE("CORS filter that throws is answered 500, with nothing granted", "[cors][integration]") {
    http::server server;
    std::atomic<int> ran{0};
    REQUIRE_FALSE(server.enable_cors({
        .origin_filter = [](std::string_view origin, const http::http_request&) -> bool {
            if (origin == "https://throws.example") throw std::runtime_error("filter failure");
            return true;
        },
        .credentials = true,
    }));
    server.use([&ran](http::request&, http::response&, std::function<void()> next) {
        ran++;
        next();
    });
    server.get("/r", [&ran](http::response& res) {
        ran++;
        res.send("ok");
    });
    server.post("/r", [&ran](http::response& res) {
        ran++;
        res.send("ok");
    });
    running_server running(server);

    const header_list from_throwing{{"Origin", "https://throws.example"}};

    SECTION("Neither middlewares nor handler run") {
        auto response = send(running, "GET", "/r", from_throwing);
        CHECK(response.status == 500);
        CHECK(response.count("Access-Control-Allow-Origin") == 0);
        CHECK(response.count("Access-Control-Allow-Credentials") == 0);
        CHECK(response.vary("Origin") == 1);
        CHECK(ran == 0);
    }

    SECTION("Its body is not read: the connection is not reused") {
        auto raw = make_raw("POST", "/r", from_throwing, "hello", false);
        auto result = server_test::raw_exchange(running, raw);
        auto response = parse_answer(result.data, result.closed);
        CHECK(response.status == 500);
        CHECK(response.closed);
        CHECK(ran == 0);
    }

    SECTION("Dispatched in memory too") {
        auto response = in_memory(server, http::method::GET, "/r", from_throwing);
        CHECK(response->get_status_code() == 500);
        CHECK_FALSE(response->has_header("Access-Control-Allow-Origin"));
        CHECK(ran == 0);
    }
}

TEST_CASE("CORS credentials and exposed headers", "[cors][integration]") {
    temp_file file("content");
    http::server server;
    REQUIRE_FALSE(server.enable_cors({
        .origins = {"https://a.example"},
        .exposed_headers = {"X-Total", "X-Page"},
        .credentials = true,
    }));
    add_routes(server, file.path);
    running_server running(server);

    SECTION("To an origin allowed") {
        auto response = send(running, "GET", "/r", from_a);
        CHECK(response.header("Access-Control-Allow-Origin") == "https://a.example");
        CHECK(response.header("Access-Control-Allow-Credentials") == "true");
        CHECK(response.header("Access-Control-Expose-Headers") == "X-Total, X-Page");
        // Those of a preflight, never here
        CHECK(response.count("Access-Control-Allow-Methods") == 0);
        CHECK(response.count("Access-Control-Allow-Headers") == 0);
        CHECK(response.count("Access-Control-Max-Age") == 0);
    }

    SECTION("To an origin not allowed") {
        auto response = send(running, "GET", "/r", {{"Origin", "https://evil.example"}});
        CHECK(response.count("Access-Control-Allow-Credentials") == 0);
        CHECK(response.count("Access-Control-Expose-Headers") == 0);
    }

    SECTION("On every form of answer, once") {
        for (const auto* path : {"/json", "/encode", "/manual", "/file", "/chunked", "/stream", "/redirect"}) {
            CAPTURE(path);
            auto response = send(running, "GET", path, from_a);
            CHECK(response.count("Access-Control-Allow-Origin") == 1);
            CHECK(response.count("Access-Control-Allow-Credentials") == 1);
            CHECK(response.count("Access-Control-Expose-Headers") == 1);
            CHECK(response.vary("Origin") == 1);
        }
    }

    SECTION("Without exposed headers it is not sent") {
        http::server others;
        REQUIRE_FALSE(others.enable_cors());
        others.get("/r", [](http::response& res) { res.send("ok"); });
        auto response = in_memory(others, http::method::GET, "/r", from_a);
        CHECK_FALSE(response->has_header("Access-Control-Expose-Headers"));
    }
}

TEST_CASE("CORS adds Origin to the other values of Vary", "[cors][integration]") {
    temp_file file("content");
    http::server server;
    REQUIRE_FALSE(server.enable_cors({.origins = {"https://a.example"}}));
    add_routes(server, file.path);
    server.get("/own-vary", [](http::response& res) {
        res.header("Vary", "origin, Cookie");
        res.send("ok");
    });
    server.get("/vary-any", [](http::response& res) {
        res.header("Vary", "*");
        res.send("ok");
    });
    server.get("/manual-vary", [](http::response& res) {
        auto message = std::make_shared<http::http_response>();
        message->add_header("Vary", "Cookie");
        message->set_content("manual", "text/plain");
        res.send_response(message);
    });
    running_server running(server);

    SECTION("Compressed") {
        auto response = send(running, "GET", "/large", {{"Origin", "https://a.example"}, {"Accept-Encoding", "gzip"}});
        CHECK(response.header("Content-Encoding") == "gzip");
        CHECK(response.vary("Origin") == 1);
        CHECK(response.vary("Accept-Encoding") == 1);
        CHECK(response.count("Vary") == 1);
    }

    SECTION("Negotiated") {
        auto response = send(running, "GET", "/encode", {{"Origin", "https://a.example"}, {"Accept", "application/json"}});
        CHECK(response.vary("Origin") == 1);
        CHECK(response.vary("Accept") == 1);
        CHECK(response.count("Vary") == 1);
    }

    SECTION("Set by the handler, with Origin already") {
        auto response = send(running, "GET", "/own-vary", from_a);
        CHECK(response.vary("Origin") == 1);
        CHECK(response.vary("Cookie") == 1);
        CHECK(response.count("Vary") == 1);
    }

    SECTION("Vary: * says it all") {
        auto response = send(running, "GET", "/vary-any", from_a);
        CHECK(response.header("Vary") == "*");
    }

    SECTION("On a message built by hand") {
        auto response = send(running, "GET", "/manual-vary", from_a);
        CHECK(response.vary("Origin") == 1);
        CHECK(response.vary("Cookie") == 1);
        CHECK(response.count("Vary") == 1);
    }
}

TEST_CASE("CORS adds Origin to the Vary of a streamed head", "[cors][integration]") {
    http::server server;
    REQUIRE_FALSE(server.enable_cors({.origins = {"https://a.example"}, .credentials = true}));
    add_vary_stream_routes(server);
    running_server running(server);

    SECTION("Granted") {
        auto response = send(running, "GET", "/head-vary", from_a);
        CHECK(response.status == 200);
        CHECK(response.header("Access-Control-Allow-Origin") == "https://a.example");
        CHECK(response.vary("Origin") == 1);
        CHECK(response.vary("Accept-Encoding") == 1);
        CHECK(response.count("Vary") == 1);
    }

    SECTION("Not granted") {
        auto response = send(running, "GET", "/head-vary", {{"Origin", "https://evil.example"}});
        CHECK(response.status == 200);
        CHECK(response.count("Access-Control-Allow-Origin") == 0);
        CHECK(response.vary("Origin") == 1);
        CHECK(response.vary("Accept-Encoding") == 1);
    }

    SECTION("Without Origin") {
        auto response = send(running, "GET", "/head-vary");
        CHECK(response.vary("Origin") == 1);
        CHECK(response.vary("Accept-Encoding") == 1);
    }

    SECTION("With another case in its name") {
        auto response = send(running, "GET", "/head-lowercase", from_a);
        CHECK(response.vary("Origin") == 1);
        CHECK(response.vary("Accept") == 1);
        CHECK(response.count("Vary") == 1);
    }

    SECTION("With Origin already, not repeated") {
        auto response = send(running, "GET", "/head-origin", from_a);
        CHECK(response.vary("Origin") == 1);
        CHECK(response.vary("Cookie") == 1);
        CHECK(response.count("Vary") == 1);
    }

    SECTION("In two fields") {
        auto response = send(running, "GET", "/head-two-fields", from_a);
        CHECK(response.vary("Origin") == 1);
        CHECK(response.vary("Accept") == 1);
        CHECK(response.vary("Cookie") == 1);
        CHECK(response.count("Vary") == 1);
    }

    SECTION("Vary: * says it all") {
        auto response = send(running, "GET", "/head-any", from_a);
        CHECK(response.count("Vary") == 1);
        CHECK(response.header("Vary") == "*");
    }

    SECTION("Without Vary") {
        auto response = send(running, "GET", "/head-no-vary", from_a);
        CHECK(response.header("Vary") == "Origin");
    }

    SECTION("With a Vary set before the head too") {
        auto response = send(running, "GET", "/head-after-header", from_a);
        CHECK(response.vary("Origin") == 1);
        CHECK(response.vary("Cookie") == 1);
        CHECK(response.vary("Accept") == 1);
        CHECK(response.count("Vary") == 1);
    }
}

TEST_CASE("Without Origin in Vary, a streamed head keeps its own", "[cors][integration]") {
    http::server server;
    const bool cors_on = GENERATE(false, true);
    // With any origin the response does not depend on it: CORS has nothing to add to Vary
    if (cors_on) REQUIRE_FALSE(server.enable_cors());
    add_vary_stream_routes(server);
    running_server running(server);

    auto response = send(running, "GET", "/head-unspaced", from_a);
    CHECK(response.header("Vary") == "Accept,Cookie");

    auto lowercase = send(running, "GET", "/head-lowercase", from_a);
    CHECK(lowercase.count("Vary") == 1);
    CHECK(lowercase.header("vary") == "Accept");

    auto fields = send(running, "GET", "/head-two-fields", from_a);
    CHECK(fields.count("Vary") == 2);
    CHECK(fields.vary("Accept") == 1);
    CHECK(fields.vary("Cookie") == 1);

    auto any = send(running, "GET", "/head-any", from_a);
    CHECK(any.header("Vary") == "*");

    auto none = send(running, "GET", "/head-no-vary", from_a);
    CHECK(none.count("Vary") == 0);

    auto both = send(running, "GET", "/head-after-header", from_a);
    CHECK(both.vary("Cookie") == 1);
    CHECK(both.vary("Accept") == 1);
    CHECK(both.vary("Origin") == 0);
}

TEST_CASE("CORS adds Origin to the Vary of a response from a reverse proxy", "[cors][integration]") {
    http::server target;
    target.get("/t", [](http::response& res) {
        res.header("Vary", "Accept-Encoding");
        res.send("from target");
    });
    target.get("/own-origin", [](http::response& res) {
        res.header("Vary", "origin, Cookie");
        res.send("from target");
    });
    running_server target_running(target);

    http::server server;
    http::reverse_proxy proxy;
    const auto to = *http::proxy_target::from_url("http://127.0.0.1:" + std::to_string(target_running.port));
    server.set_not_found_handler([&proxy, to](http::request& req, http::response& res) -> awaitable<void> {
        co_await proxy.forward(req, res, to);
    });

    SECTION("A list of origins") {
        REQUIRE_FALSE(server.enable_cors({.origins = {"https://a.example"}, .credentials = true}));
        running_server running(server);

        auto granted = send(running, "GET", "/t", from_a);
        CHECK(granted.status == 200);
        CHECK(granted.header("Access-Control-Allow-Origin") == "https://a.example");
        CHECK(granted.count("Access-Control-Allow-Credentials") == 1);
        CHECK(granted.vary("Origin") == 1);
        CHECK(granted.vary("Accept-Encoding") == 1);
        CHECK(granted.count("Vary") == 1);

        auto denied = send(running, "GET", "/t", {{"Origin", "https://evil.example"}});
        CHECK(denied.status == 200);
        CHECK(denied.count("Access-Control-Allow-Origin") == 0);
        CHECK(denied.vary("Origin") == 1);
        CHECK(denied.vary("Accept-Encoding") == 1);

        auto repeated = send(running, "GET", "/own-origin", from_a);
        CHECK(repeated.vary("Origin") == 1);
        CHECK(repeated.vary("Cookie") == 1);
    }

    SECTION("Any origin: that of the target, as it came") {
        REQUIRE_FALSE(server.enable_cors());
        running_server running(server);
        auto response = send(running, "GET", "/t", from_a);
        CHECK(response.header("Access-Control-Allow-Origin") == "*");
        CHECK(response.header("Vary") == "Accept-Encoding");
    }

    SECTION("CORS off: that of the target, as it came") {
        running_server running(server);
        auto response = send(running, "GET", "/own-origin", from_a);
        CHECK_FALSE(response.has_cors_header());
        CHECK(response.header("Vary") == "origin, Cookie");
    }
}

TEST_CASE("CORS leaves the headers set by the application", "[cors][integration]") {
    http::server server;
    bool list = GENERATE(false, true);
    if (list) {
        REQUIRE_FALSE(server.enable_cors({
            .origins = {"https://a.example"},
            .exposed_headers = {"X-Library"},
            .credentials = true,
        }));
    } else {
        REQUIRE_FALSE(server.enable_cors());
    }

    server.get("/own", [](http::response& res) {
        res.header("Access-Control-Allow-Origin", "https://own.example");
        res.send("ok");
    });
    server.get("/lowercase", [](http::response& res) {
        res.header("access-control-allow-origin", "https://own.example");
        res.send("ok");
    });
    server.get("/manual", [](http::response& res) {
        auto message = std::make_shared<http::http_response>();
        message->add_header("Access-Control-Allow-Origin", "https://own.example");
        message->set_content("manual", "text/plain");
        res.send_response(message);
    });
    server.get("/own-expose", [](http::response& res) {
        res.header("Access-Control-Expose-Headers", "X-Own");
        res.header("Access-Control-Allow-Credentials", "own");
        res.send("ok");
    });
    server.get("/own-any", [](http::response& res) {
        res.header("Access-Control-Allow-Origin", "*");
        res.send("ok");
    });
    server.get("/stream-origin", [](http::response& res) -> awaitable<void> {
        http::http_response head;
        head.add_header("Access-Control-Allow-Origin", "https://own.example");
        res.start_stream(head);
        co_await res.end_stream();
    });
    server.get("/stream-any", [](http::response& res) -> awaitable<void> {
        http::http_response head;
        head.add_header("Access-Control-Allow-Origin", "*");
        res.start_stream(head);
        co_await res.end_stream();
    });
    server.use([](http::request& req, http::response& res, std::function<void()> next) {
        if (req.get_http_request()->get_path() == "/guarded") {
            res.header("Access-Control-Allow-Origin", "https://own.example");
            res.error(http::http_response::status::unauthorized, "Unauthorized");
            return;
        }
        next();
    });
    running_server running(server);

    for (const auto* path : {"/own", "/lowercase", "/manual", "/stream-origin", "/guarded"}) {
        CAPTURE(path);
        auto response = send(running, "GET", path, from_a);
        CHECK(response.count("Access-Control-Allow-Origin") == 1);
        CHECK(response.header("Access-Control-Allow-Origin") == "https://own.example");
    }

    if (list) {
        // Each one on its own, and not what the handler set
        auto own = send(running, "GET", "/own-expose", from_a);
        CHECK(own.count("Access-Control-Expose-Headers") == 1);
        CHECK(own.header("Access-Control-Expose-Headers") == "X-Own");
        CHECK(own.count("Access-Control-Allow-Credentials") == 1);
        CHECK(own.header("Access-Control-Allow-Credentials") == "own");
        CHECK(own.header("Access-Control-Allow-Origin") == "https://a.example");
    }

    // "*" set by the application takes the credentials of the library away (FR-016)
    for (const auto* path : {"/own-any", "/stream-any"}) {
        CAPTURE(path);
        auto response = send(running, "GET", path, from_a);
        CHECK(response.count("Access-Control-Allow-Origin") == 1);
        CHECK(response.header("Access-Control-Allow-Origin") == "*");
        CHECK(response.count("Access-Control-Allow-Credentials") == 0);
    }
}

TEST_CASE("CORS applies to requests dispatched in memory", "[cors][integration]") {
    temp_file file("content");
    http::server server;
    REQUIRE_FALSE(server.enable_cors({.origins = {"https://a.example"}, .credentials = true}));
    add_routes(server, file.path);
    server.get("/forever", [](http::response& res) -> awaitable<void> {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 400ms);
        co_await timer.async_wait(use_nothrow_awaitable);
        res.send("late");
    });

    for (const auto* path : {"/r", "/json", "/manual", "/missing", "/throws", "/guarded"}) {
        CAPTURE(path);
        auto response = in_memory(server, http::method::GET, path, from_a);
        CHECK(response->get_header("Access-Control-Allow-Origin") == "https://a.example");
        CHECK(response->get_header("Access-Control-Allow-Credentials") == "true");
        CHECK(response->get_header("Vary") == "Origin");
    }

    SECTION("Timed out") {
        http::dispatch_options options;
        options.timeout = 50ms;
        auto response = in_memory(server, http::method::GET, "/forever", from_a, std::move(options));
        CHECK(response->get_status_code() == 504);
        CHECK(response->get_header("Access-Control-Allow-Origin") == "https://a.example");
        CHECK(response->get_header("Vary") == "Origin");
    }

    SECTION("Not granted") {
        auto response = in_memory(server, http::method::GET, "/r", {{"Origin", "https://evil.example"}});
        CHECK(response->get_status_code() == 200);
        CHECK_FALSE(response->has_header("Access-Control-Allow-Origin"));
        CHECK(response->get_header("Vary") == "Origin");
    }
}

// ==================== US3: the preflight ====================

TEST_CASE("CORS preflight is answered before routes and middlewares", "[cors][preflight][integration]") {
    http::server server;
    REQUIRE_FALSE(server.enable_cors());
    permission_middleware middleware;
    middleware.attach(server);
    std::atomic<int> handled{0};
    server.get("/api/devices", [&handled](http::response& res) {
        handled++;
        res.send("devices");
    }).meta("permission", "Device:Read");
    server.options("/api/own", [&handled](http::response& res) {
        handled++;
        res.send("own");
    }).meta("permission", "Own:Options");
    running_server running(server);

    auto check = [&](const answer& response) {
        CHECK(response.status == 204);
        CHECK(response.count("Access-Control-Allow-Origin") == 1);
        CHECK(response.header("Access-Control-Allow-Origin") == "*");
        CHECK(response.count("Access-Control-Allow-Methods") == 1);
        CHECK(response.header("Access-Control-Allow-Methods") == default_methods);
        CHECK(response.count("Access-Control-Allow-Credentials") == 0);
        CHECK(response.count("Access-Control-Max-Age") == 0);
        CHECK(response.count("Content-Length") == 0);
        CHECK(response.count("Content-Type") == 0);
        CHECK(response.count("Transfer-Encoding") == 0);
        CHECK(response.vary("Origin") == 0);
        CHECK(response.valid_pair());
        CHECK(middleware.ran == 0);
        CHECK(handled == 0);
    };

    SECTION("On a route of another method") {
        check(send(running, "OPTIONS", "/api/devices", preflight_from_a()));
    }

    SECTION("On a path with no route: neither 404 nor 405") {
        check(send(running, "OPTIONS", "/api/missing", preflight_from_a()));
    }

    SECTION("On an OPTIONS route of the application, which does not run") {
        check(send(running, "OPTIONS", "/api/own", preflight_from_a()));
    }

    SECTION("Its body is empty on the wire") {
        auto result = server_test::raw_exchange(running, make_raw("OPTIONS", "/api/devices", preflight_from_a(), "", true));
        CHECK(result.data.substr(result.data.find("\r\n\r\n") + 4).empty());
        CHECK(result.closed);
    }

    SECTION("Without an Origin as well: what makes it a preflight is the method it asks for") {
        check(send(running, "OPTIONS", "/api/devices", {{"Access-Control-Request-Method", "PUT"}}));
    }

    SECTION("The name of the header is not case sensitive") {
        check(send(running, "OPTIONS", "/api/devices", {{"access-control-request-method", "PUT"}}));
    }

    SECTION("Any other request goes through the middleware") {
        auto response = send(running, "GET", "/api/missing", from_a);
        CHECK(response.status == 401);
        CHECK(response.header("Access-Control-Allow-Origin") == "*");
        CHECK(middleware.ran == 1);
    }
}

TEST_CASE("CORS preflight on a virtual host", "[cors][preflight][integration]") {
    http::server server;
    REQUIRE_FALSE(server.enable_cors());
    server.host("api.example.com").get("/r", [](http::response& res) { res.send("ok"); });
    running_server running(server);

    std::string raw = "OPTIONS /anything HTTP/1.1\r\nHost: api.example.com\r\nConnection: close\r\n"
                      "Origin: https://a.example\r\nAccess-Control-Request-Method: GET\r\n\r\n";
    auto result = server_test::raw_exchange(running, raw);
    auto response = parse_answer(result.data, result.closed);
    CHECK(response.status == 204);
    CHECK(response.header("Access-Control-Allow-Origin") == "*");
    CHECK(response.header("Access-Control-Allow-Methods") == default_methods);
}

TEST_CASE("CORS: an OPTIONS that is not a preflight is routed", "[cors][preflight][integration]") {
    http::server server;
    REQUIRE_FALSE(server.enable_cors({.max_age = std::chrono::seconds(600)}));
    server.get("/r", [](http::response& res) { res.send("ok"); });
    bool with_options_route = GENERATE(false, true);
    if (with_options_route) {
        server.options("/own", [](http::response& res) { res.send("own"); });
    }
    running_server running(server);

    // Without the header, or with an empty one
    auto headers = GENERATE(header_list{{"Origin", "https://a.example"}},
                            header_list{{"Origin", "https://a.example"}, {"Access-Control-Request-Method", ""}},
                            header_list{{"Origin", "https://a.example"}, {"Access-Control-Request-Headers", "x-custom"}});
    CAPTURE(headers.size());

    auto check_normal = [](const answer& response) {
        CHECK(response.header("Access-Control-Allow-Origin") == "*");
        CHECK(response.count("Access-Control-Allow-Methods") == 0);
        CHECK(response.count("Access-Control-Allow-Headers") == 0);
        CHECK(response.count("Access-Control-Max-Age") == 0);
        CHECK(response.vary("Access-Control-Request-Headers") == 0);
    };

    auto no_route = send(running, "OPTIONS", "/r", headers);
    CHECK(no_route.status == (with_options_route ? 404 : 405));
    check_normal(no_route);

    auto missing = send(running, "OPTIONS", "/missing", headers);
    CHECK(missing.status == (with_options_route ? 404 : 405));
    check_normal(missing);

    if (with_options_route) {
        auto own = send(running, "OPTIONS", "/own", headers);
        CHECK(own.status == 200);
        check_normal(own);
    }
}

TEST_CASE("CORS preflight: what it allows", "[cors][preflight][integration]") {
    http::server server;
    server.get("/r", [](http::response& res) { res.send("ok"); });

    auto preflight = [&](const header_list& more = {}) {
        auto response = in_memory(server, http::method::OPTIONS, "/r", preflight_from_a(more));
        CHECK(response->get_status_code() == 204);
        return response;
    };
    auto vary_has = [](const std::shared_ptr<http::http_response>& response, const std::string& field) {
        size_t n = 0;
        http::syntax::for_each_element(response->get_header("Vary"), [&](std::string_view item) {
            if (util::ascii::iequals(item, field)) n++;
        });
        return n;
    };

    SECTION("The headers it asks for are reflected, without a list") {
        REQUIRE_FALSE(server.enable_cors());
        auto response = preflight({{"Access-Control-Request-Headers", "content-type, x-custom"}});
        CHECK(response->get_header("Access-Control-Allow-Headers") == "content-type, x-custom");
        CHECK(vary_has(response, "Access-Control-Request-Headers") == 1);
        CHECK(vary_has(response, "Origin") == 0);
    }

    SECTION("If it asks for none, none are allowed; the answer still depends on what it asks") {
        REQUIRE_FALSE(server.enable_cors());
        auto response = preflight();
        CHECK_FALSE(response->has_header("Access-Control-Allow-Headers"));
        CHECK(vary_has(response, "Access-Control-Request-Headers") == 1);
    }

    SECTION("Repeated headers asked for make one list") {
        REQUIRE_FALSE(server.enable_cors());
        auto response = preflight({{"Access-Control-Request-Headers", "x-one"},
                                   {"Access-Control-Request-Headers", "x-two"}});
        CHECK(response->get_header("Access-Control-Allow-Headers") == "x-one, x-two");
    }

    SECTION("With a list, the list, whatever it asks for") {
        REQUIRE_FALSE(server.enable_cors({.allowed_headers = std::vector<std::string>{"Content-Type", "Authorization"}}));
        for (const auto& more : {header_list{}, header_list{{"Access-Control-Request-Headers", "x-other"}}}) {
            auto response = preflight(more);
            CHECK(response->get_header("Access-Control-Allow-Headers") == "Content-Type, Authorization");
            CHECK(vary_has(response, "Access-Control-Request-Headers") == 0);
        }
    }

    SECTION("With an empty list, none and no reflection") {
        REQUIRE_FALSE(server.enable_cors({.allowed_headers = std::vector<std::string>{}}));
        auto response = preflight({{"Access-Control-Request-Headers", "x-custom"}});
        CHECK_FALSE(response->has_header("Access-Control-Allow-Headers"));
        CHECK(vary_has(response, "Access-Control-Request-Headers") == 0);
    }

    SECTION("Max-Age: not sent by default") {
        REQUIRE_FALSE(server.enable_cors());
        CHECK_FALSE(preflight()->has_header("Access-Control-Max-Age"));
    }

    SECTION("Max-Age: as configured") {
        REQUIRE_FALSE(server.enable_cors({.max_age = std::chrono::seconds(600)}));
        CHECK(preflight()->get_header("Access-Control-Max-Age") == "600");
    }

    SECTION("Max-Age: 0 is sent") {
        REQUIRE_FALSE(server.enable_cors({.max_age = std::chrono::seconds(0)}));
        CHECK(preflight()->get_header("Access-Control-Max-Age") == "0");
    }

    SECTION("A method out of the list gets the list too: the browser decides") {
        REQUIRE_FALSE(server.enable_cors({.methods = {"GET", "POST"}}));
        auto response = preflight();
        CHECK(response->get_header("Access-Control-Allow-Methods") == "GET, POST");
    }

    SECTION("No methods: not sent") {
        REQUIRE_FALSE(server.enable_cors({.methods = {}}));
        CHECK_FALSE(preflight()->has_header("Access-Control-Allow-Methods"));
    }

    SECTION("Exposed headers") {
        REQUIRE_FALSE(server.enable_cors({.exposed_headers = {"X-Total"}}));
        CHECK(preflight()->get_header("Access-Control-Expose-Headers") == "X-Total");
    }
}

TEST_CASE("CORS preflight with a list of origins and credentials", "[cors][preflight][integration]") {
    http::server server;
    REQUIRE_FALSE(server.enable_cors({
        .origins = {"https://a.example"},
        .exposed_headers = {"X-Total"},
        .max_age = std::chrono::seconds(600),
        .credentials = true,
    }));
    permission_middleware middleware;
    middleware.attach(server);
    running_server running(server);

    const header_list asks{{"Access-Control-Request-Headers", "x-custom"}};

    SECTION("Granted") {
        auto response = send(running, "OPTIONS", "/r", preflight_from_a(asks));
        CHECK(response.status == 204);
        CHECK(response.header("Access-Control-Allow-Origin") == "https://a.example");
        CHECK(response.header("Access-Control-Allow-Credentials") == "true");
        CHECK(response.header("Access-Control-Allow-Methods") == default_methods);
        CHECK(response.header("Access-Control-Allow-Headers") == "x-custom");
        CHECK(response.header("Access-Control-Max-Age") == "600");
        CHECK(response.header("Access-Control-Expose-Headers") == "X-Total");
        CHECK(response.count("Vary") == 1);
        CHECK(response.vary("Origin") == 1);
        CHECK(response.vary("Access-Control-Request-Headers") == 1);
    }

    auto check_nothing_granted = [](const answer& response) {
        CHECK(response.status == 204);
        CHECK_FALSE(response.has_cors_header());
        CHECK(response.header("Vary") == "Origin");
    };

    SECTION("Not granted: only Vary") {
        check_nothing_granted(send(running, "OPTIONS", "/r",
                                   {{"Origin", "https://evil.example"}, {"Access-Control-Request-Method", "PUT"},
                                    {"Access-Control-Request-Headers", "x-custom"}}));
    }

    SECTION("Without an Origin: only Vary") {
        check_nothing_granted(send(running, "OPTIONS", "/r", {{"Access-Control-Request-Method", "PUT"}}));
    }

    CHECK(middleware.ran == 0);
}

TEST_CASE("CORS preflight with a filter of origins", "[cors][preflight][integration]") {
    http::server server;
    std::atomic<int> calls{0};
    REQUIRE_FALSE(server.enable_cors({
        .origin_filter = [&calls](std::string_view origin, const http::http_request&) -> bool {
            calls++;
            if (origin == "https://throws.example") throw std::runtime_error("filter failure");
            return origin == "https://a.example";
        },
        .credentials = true,
    }));
    running_server running(server);

    SECTION("Granted, deciding once") {
        auto response = send(running, "OPTIONS", "/r", preflight_from_a());
        CHECK(response.status == 204);
        CHECK(response.header("Access-Control-Allow-Origin") == "https://a.example");
        CHECK(response.header("Access-Control-Allow-Credentials") == "true");
        CHECK(response.header("Access-Control-Allow-Methods") == default_methods);
        CHECK(calls == 1);
    }

    SECTION("Not granted") {
        auto response = send(running, "OPTIONS", "/r", {{"Origin", "https://b.example"}, {"Access-Control-Request-Method", "PUT"}});
        CHECK(response.status == 204);
        CHECK_FALSE(response.has_cors_header());
        CHECK(response.header("Vary") == "Origin");
    }

    SECTION("A filter that throws: 500, with nothing granted") {
        auto response = send(running, "OPTIONS", "/r", {{"Origin", "https://throws.example"}, {"Access-Control-Request-Method", "PUT"}});
        CHECK(response.status == 500);
        CHECK_FALSE(response.has_cors_header());
        CHECK(response.header("Vary") == "Origin");
    }
}

TEST_CASE("CORS preflight_continue: the application answers it", "[cors][preflight][integration]") {
    http::server server;
    permission_middleware middleware;
    middleware.attach(server);
    std::atomic<int> handled{0};
    server.options("/api/devices", [&handled](http::response& res) {
        handled++;
        res.send("own");
    }).meta("permission", "Device:Preflight");

    const header_list asks{{"Access-Control-Request-Headers", "x-custom"}};

    SECTION("Any origin") {
        REQUIRE_FALSE(server.enable_cors({.max_age = std::chrono::seconds(600), .preflight_continue = true}));
        running_server running(server);
        auto response = send(running, "OPTIONS", "/api/devices", preflight_from_a(asks));
        CHECK(response.status == 200);
        CHECK(middleware.ran == 1);
        CHECK(middleware.seen() == "Device:Preflight");
        CHECK(handled == 1);
        for (const auto* name : {"Access-Control-Allow-Origin", "Access-Control-Allow-Methods",
                                 "Access-Control-Allow-Headers", "Access-Control-Max-Age"}) {
            CAPTURE(name);
            CHECK(response.count(name) == 1);
        }
        CHECK(response.header("Access-Control-Allow-Origin") == "*");
        CHECK(response.header("Access-Control-Allow-Methods") == default_methods);
        CHECK(response.header("Access-Control-Allow-Headers") == "x-custom");
        CHECK(response.header("Access-Control-Max-Age") == "600");
        CHECK(response.vary("Access-Control-Request-Headers") == 1);
    }

    SECTION("A list with credentials") {
        REQUIRE_FALSE(server.enable_cors({
            .origins = {"https://a.example"},
            .allowed_headers = std::vector<std::string>{"Authorization"},
            .credentials = true,
            .preflight_continue = true,
        }));
        running_server running(server);
        auto response = send(running, "OPTIONS", "/api/devices", preflight_from_a());
        CHECK(response.status == 200);
        CHECK(handled == 1);
        CHECK(response.count("Access-Control-Allow-Origin") == 1);
        CHECK(response.header("Access-Control-Allow-Origin") == "https://a.example");
        CHECK(response.count("Access-Control-Allow-Credentials") == 1);
        CHECK(response.header("Access-Control-Allow-Headers") == "Authorization");
        CHECK(response.count("Vary") == 1);
        CHECK(response.vary("Origin") == 1);
    }

    SECTION("An origin not granted still runs, with no header") {
        REQUIRE_FALSE(server.enable_cors({.origins = {"https://b.example"}, .preflight_continue = true}));
        running_server running(server);
        auto response = send(running, "OPTIONS", "/api/devices", preflight_from_a());
        CHECK(response.status == 200);
        CHECK(handled == 1);
        CHECK_FALSE(response.has_cors_header());
        CHECK(response.vary("Origin") == 1);
    }

    SECTION("A route with no permission is for the middleware to reject") {
        REQUIRE_FALSE(server.enable_cors({.preflight_continue = true}));
        running_server running(server);
        auto response = send(running, "OPTIONS", "/api/other", preflight_from_a());
        CHECK(response.status == 401);
        CHECK(middleware.ran == 1);
        CHECK(handled == 0);
        CHECK(response.header("Access-Control-Allow-Origin") == "*");
        CHECK(response.header("Access-Control-Allow-Methods") == default_methods);
    }
}

TEST_CASE("CORS preflight_continue: a streamed head keeps the fields of Vary", "[cors][preflight][integration]") {
    http::server server;
    server.options("/s", [](http::response& res) -> awaitable<void> {
        return stream_head(res, {{"Vary", "Cookie"}});
    });
    const header_list asks{{"Access-Control-Request-Headers", "x-a"}};

    SECTION("A list of origins, granted") {
        REQUIRE_FALSE(server.enable_cors({.origins = {"https://a.example"}, .preflight_continue = true}));
        running_server running(server);
        auto response = send(running, "OPTIONS", "/s", preflight_from_a(asks));
        CHECK(response.header("Access-Control-Allow-Origin") == "https://a.example");
        CHECK(response.header("Access-Control-Allow-Headers") == "x-a");
        CHECK(response.vary("Cookie") == 1);
        CHECK(response.vary("Origin") == 1);
        CHECK(response.vary("Access-Control-Request-Headers") == 1);
        CHECK(response.count("Vary") == 1);
    }

    SECTION("A list of origins, not granted") {
        REQUIRE_FALSE(server.enable_cors({.origins = {"https://b.example"}, .preflight_continue = true}));
        running_server running(server);
        auto response = send(running, "OPTIONS", "/s", preflight_from_a(asks));
        CHECK_FALSE(response.has_cors_header());
        CHECK(response.vary("Cookie") == 1);
        CHECK(response.vary("Origin") == 1);
    }

    SECTION("Any origin") {
        REQUIRE_FALSE(server.enable_cors({.preflight_continue = true}));
        running_server running(server);
        auto response = send(running, "OPTIONS", "/s", preflight_from_a(asks));
        CHECK(response.header("Access-Control-Allow-Origin") == "*");
        CHECK(response.vary("Cookie") == 1);
        CHECK(response.vary("Access-Control-Request-Headers") == 1);
        CHECK(response.vary("Origin") == 0);
    }
}

TEST_CASE("CORS preflight_continue: what the application sets stays", "[cors][preflight][integration]") {
    http::server server;
    REQUIRE_FALSE(server.enable_cors({.preflight_continue = true}));
    server.options("/header", [](http::response& res) {
        res.header("Access-Control-Allow-Methods", "PUT");
        res.send("own");
    });
    server.options("/manual", [](http::response& res) {
        auto message = std::make_shared<http::http_response>();
        message->add_header("Access-Control-Allow-Methods", "PUT");
        message->set_status(http::http_response::status::no_content);
        res.send_response(message);
    });
    running_server running(server);

    for (const auto* path : {"/header", "/manual"}) {
        CAPTURE(path);
        auto response = send(running, "OPTIONS", path, preflight_from_a());
        CHECK(response.count("Access-Control-Allow-Methods") == 1);
        CHECK(response.header("Access-Control-Allow-Methods") == "PUT");
        CHECK(response.count("Access-Control-Allow-Origin") == 1);
    }
}

TEST_CASE("CORS preflight_continue: with no route, as any request with none", "[cors][preflight][integration]") {
    http::server server;
    REQUIRE_FALSE(server.enable_cors({.preflight_continue = true}));
    server.get("/r", [](http::response& res) { res.send("ok"); });

    auto check_headers = [](const answer& response) {
        CHECK(response.count("Access-Control-Allow-Origin") == 1);
        CHECK(response.header("Access-Control-Allow-Origin") == "*");
        CHECK(response.header("Access-Control-Allow-Methods") == default_methods);
    };

    SECTION("405: no route has the method") {
        running_server running(server);
        auto response = send(running, "OPTIONS", "/r", preflight_from_a());
        CHECK(response.status == 405);
        check_headers(response);
    }

    SECTION("404: other routes have the method") {
        server.options("/own", [](http::response& res) { res.send("own"); });
        running_server running(server);
        auto response = send(running, "OPTIONS", "/missing", preflight_from_a());
        CHECK(response.status == 404);
        check_headers(response);
    }

    SECTION("The not-found handler") {
        server.set_not_found_handler([](http::response& res) { res.send("fallback"); });
        running_server running(server);
        auto response = send(running, "OPTIONS", "/missing", preflight_from_a());
        CHECK(response.status == 200);
        check_headers(response);
    }
}

TEST_CASE("CORS preflight_continue: the limits of the matched route apply", "[cors][preflight][integration]") {
    http::server server;
    server.options("/slow", [](http::response& res) -> awaitable<void> {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 400ms);
        co_await timer.async_wait(use_nothrow_awaitable);
        res.send("late");
    }).timeout(50ms);
    server.options("/limited", [](http::response& res) { res.send("ok"); }).max_body_size(2);

    SECTION("Going on") {
        REQUIRE_FALSE(server.enable_cors({.preflight_continue = true}));
        running_server running(server);

        auto slow = send(running, "OPTIONS", "/slow", preflight_from_a());
        CHECK(slow.status == 504);
        CHECK(slow.header("Access-Control-Allow-Methods") == default_methods);

        auto limited = send(running, "OPTIONS", "/limited", preflight_from_a(), "hello");
        CHECK(limited.status == 413);
        CHECK(limited.header("Access-Control-Allow-Methods") == default_methods);
    }

    SECTION("Answered by the application: there is no route") {
        REQUIRE_FALSE(server.enable_cors());
        running_server running(server);
        CHECK(send(running, "OPTIONS", "/slow", preflight_from_a()).status == 204);
        CHECK(send(running, "OPTIONS", "/limited", preflight_from_a(), "hello").status == 204);
    }
}

TEST_CASE("CORS preflight is reported to the response hook", "[cors][preflight][integration]") {
    http::server server;
    REQUIRE_FALSE(server.enable_cors());
    std::mutex mutex;
    std::vector<std::pair<std::string, int>> reported;
    server.on_response([&](const http::request& req, const http::response_info& info) {
        std::scoped_lock lock(mutex);
        reported.emplace_back(req.get_http_request()->get_path(), static_cast<int>(info.status));
    });
    auto reports = [&] {
        std::scoped_lock lock(mutex);
        return reported;
    };
    using expected_reports = std::vector<std::pair<std::string, int>>;

    SECTION("On a connection") {
        running_server running(server);
        CHECK(send(running, "OPTIONS", "/anywhere", preflight_from_a()).status == 204);
        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (reports().empty() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
        CHECK(reports() == expected_reports{{"/anywhere", 204}});
    }

    SECTION("In memory") {
        auto response = in_memory(server, http::method::OPTIONS, "/anywhere", preflight_from_a());
        CHECK(response->get_status_code() == 204);
        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (reports().empty() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
        CHECK(reports() == expected_reports{{"/anywhere", 204}});
    }
}

TEST_CASE("CORS preflight dispatched in memory", "[cors][preflight][integration]") {
    http::server server;
    REQUIRE_FALSE(server.enable_cors({
        .origins = {"https://a.example"},
        .max_age = std::chrono::seconds(600),
        .credentials = true,
    }));
    permission_middleware middleware;
    middleware.attach(server);

    const header_list headers = preflight_from_a({{"Access-Control-Request-Headers", "x-custom"}});

    auto check = [&](const std::shared_ptr<http::http_response>& memory) {
        CHECK(memory->get_status_code() == 204);
        CHECK(memory->get_content().empty());
        CHECK_FALSE(memory->has_header("Content-Length"));
        CHECK_FALSE(memory->has_header("Content-Type"));
        CHECK(memory->get_header("Access-Control-Allow-Origin") == "https://a.example");
        CHECK(memory->get_header("Access-Control-Allow-Credentials") == "true");
        CHECK(memory->get_header("Access-Control-Allow-Methods") == default_methods);
        CHECK(memory->get_header("Access-Control-Allow-Headers") == "x-custom");
        CHECK(memory->get_header("Access-Control-Max-Age") == "600");
        CHECK(memory->get_header("Vary") == "Origin, Access-Control-Request-Headers");
        CHECK(middleware.ran == 0);
    };

    SECTION("The same as on a connection") {
        auto memory = in_memory(server, http::method::OPTIONS, "/r", headers);
        check(memory);

        running_server running(server);
        auto wire = send(running, "OPTIONS", "/r", headers);
        CHECK(wire.status == memory->get_status_code());
        for (const auto* name : {"Access-Control-Allow-Origin", "Access-Control-Allow-Credentials",
                                 "Access-Control-Allow-Methods", "Access-Control-Allow-Headers",
                                 "Access-Control-Max-Age", "Vary"}) {
            CAPTURE(name);
            CHECK(wire.header(name) == memory->get_header(name));
        }
    }

    SECTION("With a body") {
        auto request = server_test::make_request(http::method::OPTIONS, "/r", "hello");
        for (const auto& [key, value] : headers) request->add_header(key, value);
        check(server_test::run_dispatch(server, std::move(request)));
    }
}

TEST_CASE("CORS adds no route and nothing to the OpenAPI document", "[cors][preflight][integration]") {
    http::server server;
    server.get("/r", [](http::response& res) { res.send("ok"); });
    auto without_cors = server.openapi().generate();

    bool preflight_continue = GENERATE(false, true);
    REQUIRE_FALSE(server.enable_cors({.preflight_continue = preflight_continue}));
    auto document = server.openapi().generate();

    CHECK(document == without_cors);
    REQUIRE(document["paths"].contains("/r"));
    for (const auto& [path, item] : document["paths"].items()) {
        CAPTURE(path);
        CHECK_FALSE(item.contains("options"));
    }
}

TEST_CASE("CORS preflight with a body keeps the connection in sync", "[cors][preflight][integration]") {
    http::server server;
    REQUIRE_FALSE(server.enable_cors());
    server.get("/next", [](http::response& res) { res.send("next"); });

    const std::string next = "GET /next HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

    auto after_first = [](const std::string& data) {
        auto end = data.find("\r\n\r\n");
        return end == std::string::npos ? std::string{} : data.substr(end + 4);
    };

    SECTION("With a Content-Length") {
        running_server running(server);
        auto result = server_test::raw_exchange(running, make_raw("OPTIONS", "/r", preflight_from_a(), "hello", false) + next);
        auto first = parse_answer(result.data);
        CHECK(first.status == 204);
        CHECK(first.header("Connection") != "close");
        auto second = after_first(result.data);
        CHECK(parse_answer(second).status == 200);
        CHECK(second.find("next") != std::string::npos);
        CHECK(result.closed);
    }

    SECTION("Chunked") {
        running_server running(server);
        auto raw = make_raw("OPTIONS", "/r", preflight_from_a({{"Transfer-Encoding", "chunked"}}), "", false);
        raw += "5\r\nhello\r\n0\r\n\r\n";
        auto result = server_test::raw_exchange(running, raw + next);
        CHECK(parse_answer(result.data).status == 204);
        auto second = after_first(result.data);
        CHECK(parse_answer(second).status == 200);
        CHECK(second.find("next") != std::string::npos);
    }

    SECTION("Sent after the answer") {
        running_server running(server);
        http::test::raw_connection connection(running.endpoint());
        auto head = make_raw("OPTIONS", "/r", preflight_from_a({{"Content-Length", "5"}}), "", false);
        connection.write(head);
        std::string answered;
        REQUIRE(connection.read_until(answered, "\r\n\r\n", 3s));
        CHECK(parse_answer(answered).status == 204);
        connection.write("hello" + next);
        auto result = connection.read_until_closed(3s);
        CHECK(parse_answer(result.data).status == 200);
        CHECK(result.data.find("next") != std::string::npos);
    }

    SECTION("Larger than the limit: dropped with the connection, as the unread body of any request") {
        server.set_max_body_size(2);
        running_server running(server);
        auto result = server_test::raw_exchange(running, make_raw("OPTIONS", "/r", preflight_from_a(), "hello", false) + next);
        auto first = parse_answer(result.data, result.closed);
        CHECK(first.status == 204);
        CHECK(first.closed);
        CHECK(after_first(result.data).empty());
    }

    SECTION("With a 100-continue not sent: the body is not asked for, and the connection closes") {
        running_server running(server);
        auto raw = make_raw("OPTIONS", "/r", preflight_from_a({{"Expect", "100-continue"}, {"Content-Length", "5"}}), "", false);
        auto result = server_test::raw_exchange(running, raw);
        auto first = parse_answer(result.data, result.closed);
        CHECK(first.status == 204);
        CHECK(result.data.find("100 Continue") == std::string::npos);
        CHECK(first.closed);
    }
}

TEST_CASE("CORS turned off again: a preflight is routed", "[cors][preflight][integration]") {
    http::server server;
    REQUIRE_FALSE(server.enable_cors());
    server.disable_cors();
    server.get("/r", [](http::response& res) { res.send("ok"); });
    auto response = in_memory(server, http::method::OPTIONS, "/r", preflight_from_a());
    CHECK(response->get_status_code() == 405);
    CHECK_FALSE(response->has_header("Access-Control-Allow-Origin"));
    CHECK_FALSE(response->has_header("Access-Control-Allow-Methods"));
}

// ==================== US4: invalid options, rejected ====================

TEST_CASE("CORS options that cannot be applied are rejected, and nothing changes", "[cors][unit]") {
    using http::cors_options;

    auto with_origins = [](std::vector<std::string> origins) {
        cors_options options;
        options.origins = std::move(origins);
        return options;
    };
    auto with_methods = [](std::vector<std::string> methods) {
        cors_options options;
        options.methods = std::move(methods);
        return options;
    };
    auto with_allowed = [](std::vector<std::string> headers) {
        cors_options options;
        options.allowed_headers = std::move(headers);
        return options;
    };
    auto with_exposed = [](std::vector<std::string> headers) {
        cors_options options;
        options.exposed_headers = std::move(headers);
        return options;
    };

    std::vector<cors_options> rejected;
    rejected.push_back({.credentials = true});
    rejected.push_back({.origins = {"*"}, .credentials = true});
    rejected.push_back(with_origins({"https://a.example", "*"}));
    rejected.push_back(with_origins({"*", "*"}));
    for (const auto& origin : {std::string("https://a.example\r\nX-Evil: 1"), std::string("https://a.example\n"),
                               std::string("https://a\0.example", 18), std::string("https://a\x7f.example"),
                               std::string("https://a\x01.example"), std::string("https://a\t.example")}) {
        rejected.push_back(with_origins({"https://ok.example", origin}));
    }
    for (const auto* value : {"", "NOT VALID", "GET,POST", "G\r\nET", "(GET)"}) {
        rejected.push_back(with_methods({"GET", value}));
    }
    for (const auto* value : {"", "Bad Header", "X-A,X-B", "X-A\r\nX-B", "X-A:"}) {
        rejected.push_back(with_allowed({"Content-Type", value}));
        rejected.push_back(with_exposed({"X-Total", value}));
    }
    rejected.push_back({.max_age = std::chrono::seconds(-1)});

    SECTION("With CORS off: still off") {
        temp_file file("content");
        http::server server;
        add_routes(server, file.path);
        for (const auto& options : rejected) {
            CHECK(invalid(server.enable_cors(options)));
            auto response = in_memory(server, http::method::GET, "/r", from_a);
            CHECK_FALSE(response->has_header("Access-Control-Allow-Origin"));
            CHECK_FALSE(response->has_header("Vary"));
        }
    }

    SECTION("With CORS on: the previous one goes on") {
        temp_file file("content");
        http::server server;
        add_routes(server, file.path);
        REQUIRE_FALSE(server.enable_cors({.origins = {"https://a.example"}, .credentials = true}));
        for (const auto& options : rejected) {
            CHECK(invalid(server.enable_cors(options)));
            auto response = in_memory(server, http::method::GET, "/r", from_a);
            CHECK(response->get_header("Access-Control-Allow-Origin") == "https://a.example");
            CHECK(response->get_header("Access-Control-Allow-Credentials") == "true");
            CHECK(allowed_origin(server, "https://other.example").empty());
        }
    }

    SECTION("The reason goes to the log") {
        http::server server;
        for (const auto& [options, reason] : std::vector<std::pair<cors_options, std::string>>{
                 {cors_options{.credentials = true}, "credentials cannot be used with any origin"},
                 {with_origins({"https://a.example", "*"}), "cannot be listed with other origins"},
                 {with_origins({"https://a.example\r\n"}), "has control characters"},
                 {with_methods({"NOT VALID"}), "is not a valid method"},
                 {with_allowed({"Bad Header"}), "is not a valid header name"},
                 {with_exposed({"Bad Header"}), "is not a valid header name"},
                 {cors_options{.max_age = std::chrono::seconds(-1)}, "max_age cannot be negative"},
             }) {
            CAPTURE(reason);
            http::test::log_capture logs;
            CHECK(invalid(server.enable_cors(options)));
            CHECK(logs.count("error", reason) == 1);
        }
    }
}

TEST_CASE("CORS options that can be applied", "[cors][unit]") {
    temp_file file("content");
    http::server server;
    add_routes(server, file.path);

    SECTION("A filter with credentials: it decides") {
        REQUIRE_FALSE(server.enable_cors({
            .origin_filter = [](std::string_view origin, const http::http_request&) { return origin == "https://a.example"; },
            .credentials = true,
        }));
        auto response = in_memory(server, http::method::GET, "/r", from_a);
        CHECK(response->get_header("Access-Control-Allow-Origin") == "https://a.example");
        CHECK(response->get_header("Access-Control-Allow-Credentials") == "true");
        CHECK(allowed_origin(server, "https://other.example").empty());
    }

    SECTION("A filter next to a list: the list is ignored, whatever it holds") {
        REQUIRE_FALSE(server.enable_cors({
            .origins = {"https://a.example", "*"},
            .origin_filter = [](std::string_view origin, const http::http_request&) { return origin == "https://b.example"; },
        }));
        CHECK(allowed_origin(server).empty());
        CHECK(allowed_origin(server, "https://b.example") == "https://b.example");
    }

    SECTION("An empty list: no origin is granted") {
        REQUIRE_FALSE(server.enable_cors({.origins = {}}));
        auto response = in_memory(server, http::method::GET, "/r", from_a);
        CHECK_FALSE(response->has_header("Access-Control-Allow-Origin"));
        CHECK(response->get_header("Vary") == "Origin");
    }

    SECTION("'*' in methods, allowed and exposed headers is a value like any other") {
        REQUIRE_FALSE(server.enable_cors({
            .methods = {"*"},
            .allowed_headers = std::vector<std::string>{"*"},
            .exposed_headers = {"*"},
        }));
        auto response = in_memory(server, http::method::GET, "/r", from_a);
        CHECK(response->get_header("Access-Control-Allow-Origin") == "*");
        CHECK(response->get_header("Access-Control-Expose-Headers") == "*");
    }

    SECTION("A max-age of 0") {
        REQUIRE_FALSE(server.enable_cors({.max_age = std::chrono::seconds(0)}));
    }

    SECTION("Replaced by a later one") {
        REQUIRE_FALSE(server.enable_cors());
        CHECK(allowed_origin(server) == "*");
        REQUIRE_FALSE(server.enable_cors({.origins = {"https://b.example"}}));
        CHECK(allowed_origin(server).empty());
        CHECK(allowed_origin(server, "https://b.example") == "https://b.example");
    }
}

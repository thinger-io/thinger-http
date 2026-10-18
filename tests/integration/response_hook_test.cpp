#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>

#include "server_test_helpers.hpp"
#include "../fixtures/stream_test_helpers.hpp"

#include <atomic>
#include <future>
#include <mutex>
#include <stdexcept>
#include <vector>

// http_application::on_response(): called once per request with its response, whoever
// answered it (the handler, a middleware, or the server itself), once it ended

using namespace thinger;
using namespace std::chrono_literals;
using server_test::running_server;

namespace {

    template<typename F>
    bool eventually(F condition, std::chrono::milliseconds timeout = 5s) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!condition()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    struct reported {
        std::string path;
        std::string client_ip;
        http::response_info info;
    };

    // The responses reported by the hook
    struct response_log {
        std::mutex mutex;
        std::vector<reported> responses;

        void attach(http::http_application& app) {
            app.on_response([this](const http::request& req, const http::response_info& info) {
                std::scoped_lock lock(mutex);
                responses.push_back({req.get_http_request()->get_path(), req.get_request_ip(), info});
            });
        }

        size_t size() {
            std::scoped_lock lock(mutex);
            return responses.size();
        }

        // The only response reported, once it is
        reported single() {
            REQUIRE(eventually([&] { return size() > 0; }));
            // a second report would come right after the first one
            std::this_thread::sleep_for(50ms);
            std::scoped_lock lock(mutex);
            REQUIRE(responses.size() == 1);
            return responses.front();
        }
    };

    std::shared_ptr<http::http_response> dispatch(http::server& server, http::method method, const std::string& path,
                                                  const std::string& body = "",
                                                  const std::string& content_type = "application/json",
                                                  http::dispatch_options options = {}) {
        auto request = http::http_request::create_http_request(method, "http://localhost" + path);
        if (!body.empty()) request->set_content(body, content_type);
        return server_test::run_dispatch(server, request, std::move(options));
    }

}

TEST_CASE("Response hook reports the responses of the handlers", "[server][response_hook][integration]") {
    http::server server;
    response_log log;
    log.attach(server);
    server.get("/hello", [](http::response& res) { res.send("hello"); });
    server.get("/slow", [](http::response& res) -> awaitable<void> {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 50ms);
        co_await timer.async_wait(use_nothrow_awaitable);
        res.send("done");
    });

    SECTION("Over a connection: status, bytes written and the client address") {
        running_server running(server);
        auto result = server_test::raw_exchange(running.port, "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(result.data.ends_with("hello"));
        auto response = log.single();
        REQUIRE(response.path == "/hello");
        REQUIRE(response.client_ip == "127.0.0.1");
        REQUIRE(response.info.status == http::http_response::status::ok);
        REQUIRE(response.info.outcome == http::response_outcome::completed);
        REQUIRE_FALSE(response.info.streamed);
        REQUIRE(response.info.bytes == result.data.size());
    }

    SECTION("The time to answer") {
        running_server running(server);
        auto result = server_test::raw_exchange(running.port, "GET /slow HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(result.data.ends_with("done"));
        auto response = log.single();
        REQUIRE(response.info.duration >= 50ms);
        REQUIRE(response.info.duration < 5s);
    }

    SECTION("Once per request of a keep-alive connection") {
        running_server running(server);
        std::string requests;
        for (int i = 0; i < 3; ++i) requests += "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
        requests += "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        auto result = server_test::raw_exchange(running.port, requests);
        REQUIRE(server_test::count(result.data, "HTTP/1.1 200") == 4);
        REQUIRE(eventually([&] { return log.size() == 4; }));
        std::this_thread::sleep_for(50ms);
        REQUIRE(log.size() == 4);
        uint64_t bytes = 0;
        for (auto& response : log.responses) bytes += response.info.bytes;
        REQUIRE(bytes == result.data.size());
    }

    SECTION("In memory: the size of the body") {
        auto response = dispatch(server, http::method::GET, "/hello");
        REQUIRE(response->get_status_code() == 200);
        auto reported = log.single();
        REQUIRE(reported.info.status == http::http_response::status::ok);
        REQUIRE(reported.info.outcome == http::response_outcome::completed);
        REQUIRE(reported.info.bytes == 5);
    }
}

TEST_CASE("Response hook reports the responses of the server", "[server][response_hook][integration]") {
    http::server server;
    response_log log;
    log.attach(server);
    server.post("/json", [](nlohmann::json& body, http::response& res) { res.json(body); });
    server.post("/small", [](nlohmann::json& body, http::response& res) { res.json(body); }).max_body_size(16);
    server.post("/shallow", [](nlohmann::json& body, http::response& res) { res.json(body); }).decode_limits({.max_depth = 1});
    server.post("/user", [](nlohmann::json& body, http::response& res) { res.json(body); }).schema({
        {"type", "object"}, {"required", {"name"}}, {"properties", {{"name", {{"type", "string"}}}}}
    });
    server.get("/stuck", [](http::response& res) -> awaitable<void> {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 300ms);
        co_await timer.async_wait(use_nothrow_awaitable);
        res.send("late");
    }).timeout(100ms);
    server.get("/private", [](http::response& res) { res.send("secret"); });
    server.use([](http::request& req, http::response& res, std::function<void()> next) {
        if (req.get_http_request()->get_path() == "/private") {
            res.error(http::http_response::status::unauthorized, "No credentials");
            return;
        }
        next();
    });

    using status = http::http_response::status;
    struct expectation {
        http::method method;
        std::string path;
        std::string body;
        std::string content_type;
        status expected;
    };
    std::vector<expectation> expectations{
        {http::method::GET, "/missing", "", "", status::not_found},
        {http::method::DELETE, "/json", "", "", status::not_allowed},
        {http::method::POST, "/small", R"({"too": "large for the route"})", "application/json", status::payload_too_large},
        {http::method::POST, "/json", "hello", "text/plain", status::unsupported_media_type},
        {http::method::POST, "/json", "{not json", "application/json", status::bad_request},
        {http::method::POST, "/shallow", R"({"a": {"b": 1}})", "application/json", status::bad_request},
        {http::method::POST, "/user", R"({"age": 3})", "application/json", status::bad_request},
        {http::method::GET, "/stuck", "", "", status::gateway_timeout},
        {http::method::GET, "/private", "", "", status::unauthorized},
        {http::method::POST, "/json", R"({"ok": true})", "application/json", status::ok},
    };

    for (const auto& expected : expectations) {
        INFO(expected.path << " " << static_cast<int>(expected.expected));
        auto response = dispatch(server, expected.method, expected.path, expected.body, expected.content_type);
        REQUIRE(response->get_status() == expected.expected);
        auto reported = log.single();
        REQUIRE(reported.path == expected.path);
        REQUIRE(reported.info.status == expected.expected);
        REQUIRE(reported.info.outcome == http::response_outcome::completed);
        REQUIRE(reported.info.bytes == response->get_content().size());
        log.responses.clear();
    }

    SECTION("Over a connection") {
        running_server running(server);
        auto result = server_test::raw_exchange(running.port,
            "POST /small HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: 40\r\n\r\n"
            "{\"too\": \"large for the route, and more\"}");
        REQUIRE(result.data.starts_with("HTTP/1.1 413"));
        auto reported = log.single();
        REQUIRE(reported.info.status == status::payload_too_large);
        REQUIRE(reported.info.bytes == result.data.size());
        log.responses.clear();

        // a request with an ambiguous framing
        result = server_test::raw_exchange(running.port,
            "POST /json HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n");
        REQUIRE(result.data.starts_with("HTTP/1.1 400"));
        REQUIRE(log.single().info.status == status::bad_request);
    }
}

TEST_CASE("Response hook reports streamed responses once they end", "[server][response_hook][integration]") {
    http::server server;
    response_log log;
    log.attach(server);
    server.get("/chunked", [](http::response& res) {
        res.start_chunked("text/plain");
        res.write_chunk("one ");
        res.write_chunk("two");
        res.end_chunked();
    });
    server.get("/stream", [](http::response& res) -> awaitable<void> {
        http::http_response head;
        head.set_status(http::http_response::status::ok);
        res.start_stream(head);
        co_await res.write_stream("part");
        co_await res.end_stream();
    });
    server.get("/aborted", [](http::response& res) -> awaitable<void> {
        http::http_response head;
        head.set_status(http::http_response::status::ok);
        res.start_stream(head, 100);
        co_await res.write_stream("only a part");
        res.abort_stream();
    });
    running_server running(server);

    SECTION("Chunked") {
        auto result = server_test::raw_exchange(running.port, "GET /chunked HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(result.data.ends_with("0\r\n\r\n"));
        auto reported = log.single();
        REQUIRE(reported.info.status == http::http_response::status::ok);
        REQUIRE(reported.info.outcome == http::response_outcome::completed);
        REQUIRE(reported.info.streamed);
        REQUIRE(reported.info.bytes == result.data.size());
    }

    SECTION("Streamed") {
        auto result = server_test::raw_exchange(running.port, "GET /stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(result.data.ends_with("0\r\n\r\n"));
        auto reported = log.single();
        REQUIRE(reported.info.outcome == http::response_outcome::completed);
        REQUIRE(reported.info.streamed);
        REQUIRE(reported.info.bytes == result.data.size());
    }

    SECTION("Aborted") {
        auto result = server_test::raw_exchange(running.port, "GET /aborted HTTP/1.1\r\nHost: localhost\r\n\r\n");
        REQUIRE(result.closed);
        REQUIRE(result.data.ends_with("only a part"));
        auto reported = log.single();
        REQUIRE(reported.info.status == http::http_response::status::ok);
        REQUIRE(reported.info.outcome == http::response_outcome::aborted);
        REQUIRE(reported.info.streamed);
        REQUIRE(reported.info.bytes == result.data.size());
    }

    SECTION("Aborted, in memory: a 502") {
        auto response = dispatch(server, http::method::GET, "/aborted");
        REQUIRE(response->get_status() == http::http_response::status::bad_gateway);
        auto reported = log.single();
        REQUIRE(reported.info.status == http::http_response::status::bad_gateway);
        REQUIRE(reported.info.outcome == http::response_outcome::aborted);
        // the 502 that ended it is whole
        REQUIRE_FALSE(reported.info.streamed);
    }
}

TEST_CASE("Response hook reports connections handed over once their response is written",
          "[server][response_hook][integration]") {
    http::server server;
    response_log log;
    log.attach(server);
    std::promise<std::weak_ptr<http::websocket_connection>> created;
    server.get("/ws", [&](http::response& res) {
        res.upgrade_websocket([&](std::shared_ptr<http::websocket_connection> ws) {
            ws->on_message([](std::string, bool) {});
            created.set_value(ws);
        });
    });
    server.get("/events", [](http::response& res) {
        res.start_sse([](std::shared_ptr<http::sse_connection> sse) { sse->send_data("hello"); });
    });
    running_server running(server);

    SECTION("WebSocket") {
        stream_test::raw_ws_client client(running.port);
        auto head = client.handshake("/ws");
        REQUIRE(head.starts_with("HTTP/1.1 101"));
        // while it is open
        auto reported = log.single();
        REQUIRE(reported.info.status == http::http_response::status::switching_protocols);
        REQUIRE(reported.info.outcome == http::response_outcome::taken_over);
        REQUIRE_FALSE(reported.info.streamed);
        REQUIRE(reported.info.bytes == head.size());
        REQUIRE_FALSE(created.get_future().get().expired());
    }

    SECTION("SSE") {
        server_test::exchange_result result;
        {
            boost::asio::io_context ioc;
            boost::asio::ip::tcp::socket sock(ioc);
            sock.connect({boost::asio::ip::make_address("127.0.0.1"), running.port});
            boost::asio::write(sock, boost::asio::buffer(std::string("GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n")));
            result = server_test::read_until_closed(ioc, sock, 300ms);
        }
        REQUIRE(result.data.find("data: hello") != std::string::npos);
        auto reported = log.single();
        REQUIRE(reported.info.status == http::http_response::status::ok);
        REQUIRE(reported.info.outcome == http::response_outcome::taken_over);
        REQUIRE(reported.info.bytes == result.data.find("data: hello"));
    }
}

TEST_CASE("Response hook reports requests never answered", "[server][response_hook][integration]") {
    http::server server;
    response_log log;
    log.attach(server);
    // keeps the response without answering it
    auto pending = std::make_shared<std::vector<http::response>>();
    auto kept = std::make_shared<std::atomic<int>>(0);
    server.get("/never", [pending, kept](http::response& res) {
        pending->push_back(res);
        ++*kept;
    });

    SECTION("The client leaves") {
        running_server running(server);
        {
            boost::asio::io_context ioc;
            boost::asio::ip::tcp::socket sock(ioc);
            sock.connect({boost::asio::ip::make_address("127.0.0.1"), running.port});
            boost::asio::write(sock, boost::asio::buffer(std::string("GET /never HTTP/1.1\r\nHost: localhost\r\n\r\n")));
            REQUIRE(eventually([&] { return kept->load() > 0; }));
        }
        auto reported = log.single();
        REQUIRE(reported.path == "/never");
        REQUIRE(reported.client_ip == "127.0.0.1");
        REQUIRE(reported.info.status == http::http_response::status{});
        REQUIRE(reported.info.outcome == http::response_outcome::aborted);
        REQUIRE(reported.info.bytes == 0);
    }

    SECTION("In memory, past the timeout: the 504 returned") {
        // the response kept belongs to this io_context: released while it lives
        boost::asio::io_context ioc;
        auto request = http::http_request::create_http_request(http::method::GET, "http://localhost/never");
        auto result = co_spawn(ioc, server.dispatch(request, {.timeout = 100ms}), boost::asio::use_future);
        ioc.run();
        auto response = result.get();
        REQUIRE(response->get_status() == http::http_response::status::gateway_timeout);
        auto reported = log.single();
        REQUIRE(reported.info.status == http::http_response::status::gateway_timeout);
        REQUIRE(reported.info.outcome == http::response_outcome::completed);
        // a late answer changes nothing
        pending->front().send("late");
        std::this_thread::sleep_for(50ms);
        REQUIRE(log.size() == 1);
        pending->clear();
    }
    pending->clear();
}

TEST_CASE("Response hook errors are logged", "[server][response_hook][integration]") {
    http::server server;
    std::atomic<int> calls{0};
    server.on_response([&](const http::request&, const http::response_info&) {
        ++calls;
        throw std::runtime_error("hook failed");
    });
    server.get("/hello", [](http::response& res) { res.send("hello"); });
    running_server running(server);
    std::string requests = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n"
                           "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    auto result = server_test::raw_exchange(running.port, requests);
    REQUIRE(server_test::count(result.data, "HTTP/1.1 200") == 2);
    REQUIRE(eventually([&] { return calls.load() == 2; }));
}

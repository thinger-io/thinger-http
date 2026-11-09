#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/sse_connection.hpp>
#include <thinger/http/server/websocket_connection.hpp>

#include "server_test_helpers.hpp"
#include "../fixtures/stream_test_helpers.hpp"

#include <boost/asio/thread_pool.hpp>

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
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
        res.upgrade_websocket({}, [&](std::shared_ptr<http::websocket_connection> ws) {
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
            // A reset: a half-close (a FIN alone) is no leaving, the response is still awaited.
            // Closed here: asio clears the linger option of a socket closed by its destructor
            sock.set_option(boost::asio::socket_base::linger(true, 0));
            sock.close();
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

// --- In-memory dispatch: it returns after the hook ---

namespace {

    // Not an atomic, and in the frame of the caller: a hook running late is a use after free
    // under ASan and a race under TSan
    struct hook_seen {
        int calls = 0;
        http::response_info info;
    };

    // Runs the hook a little late, so an ordering bug shows on every run
    void watch_hook(http::http_application& app, hook_seen& seen) {
        app.on_response([&seen](const http::request&, const http::response_info& info) {
            std::this_thread::sleep_for(1ms);
            ++seen.calls;
            seen.info = info;
        });
    }

    struct dispatched {
        http::http_response::status status{};
        int calls = 0;
        http::response_info info;
    };

    std::shared_ptr<http::http_request> memory_request(const std::string& path) {
        return http::http_request::create_http_request(http::method::GET, "http://localhost" + path);
    }

    awaitable<dispatched> dispatch_with_local_hook(http::http_application& app, std::string path) {
        hook_seen seen;
        watch_hook(app, seen);
        auto response = co_await app.dispatch(memory_request(path));
        co_return dispatched{response->get_status(), seen.calls, seen.info};
    }

    dispatched run_with_local_hook(boost::asio::thread_pool& pool, http::http_application& app, const std::string& path) {
        return co_spawn(pool, dispatch_with_local_hook(app, path), boost::asio::use_future).get();
    }

} // namespace

TEST_CASE("A dispatch returns after the response hook has run", "[server][response_hook][dispatch][integration]") {
    http::http_application app;
    app.get("/now", [](http::response& res) { res.send("ok"); });
    boost::asio::thread_pool pool(4);

    SECTION("Awaited") {
        for (int repetition = 0; repetition < 50; ++repetition) {
            auto result = run_with_local_hook(pool, app, "/now");
            REQUIRE(result.status == http::http_response::status::ok);
            REQUIRE(result.calls == 1);
            REQUIRE(result.info.outcome == http::response_outcome::completed);
        }
    }

    SECTION("With a callback") {
        for (int repetition = 0; repetition < 50; ++repetition) {
            auto seen = std::make_shared<hook_seen>();
            watch_hook(app, *seen);
            std::promise<int> called;
            app.dispatch(pool.get_executor(), memory_request("/now"),
                         [&called, seen](std::shared_ptr<http::http_response>) mutable {
                             called.set_value(seen->calls);
                             seen.reset();
                         });
            REQUIRE(called.get_future().get() == 1);
        }
    }
}

TEST_CASE("A dispatch returns after the response hook, however the route ends", "[server][response_hook][dispatch][integration]") {
    using status = http::http_response::status;
    http::http_application app;
    std::vector<std::thread> responders;
    app.get("/other-thread", [&responders](http::response& res) {
        responders.emplace_back([res]() mutable {
            std::this_thread::sleep_for(5ms);
            res.send("later");
        });
    });
    app.get("/route-timeout", [](http::response& res) -> awaitable<void> {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 200ms);
        co_await timer.async_wait(use_nothrow_awaitable);
        res.send("late");
    }).timeout(20ms);
    app.get("/throws", [](http::response&) { throw std::runtime_error("failed"); });
    boost::asio::thread_pool pool(4);

    struct expectation {
        std::string path;
        status code;
    };
    auto expected = GENERATE(expectation{"/other-thread", status::ok}, expectation{"/route-timeout", status::gateway_timeout},
                             expectation{"/throws", status::internal_server_error});
    INFO(expected.path);
    auto result = run_with_local_hook(pool, app, expected.path);
    pool.join();
    for (auto& responder : responders) responder.join();
    REQUIRE(result.status == expected.code);
    REQUIRE(result.calls == 1);
    REQUIRE(result.info.status == expected.code);
    REQUIRE(result.info.outcome == http::response_outcome::completed);
}

TEST_CASE("A dispatch does not wait past its timeout for a handler still running", "[server][response_hook][dispatch][integration]") {
    http::http_application app;
    std::atomic<int> hooks{0};
    app.on_response([&hooks](const http::request&, const http::response_info&) { ++hooks; });
    app.get("/keeps-working", [](http::response& res) -> awaitable<void> {
        res.send("done");
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 1s);
        co_await timer.async_wait(use_nothrow_awaitable);
    });
    boost::asio::thread_pool pool(4);

    auto start = std::chrono::steady_clock::now();
    auto response = co_spawn(pool, app.dispatch(memory_request("/keeps-working"), {.timeout = 100ms}),
                             boost::asio::use_future).get();
    auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE(response->get_status() == http::http_response::status::ok);
    REQUIRE(response->get_content() == "done");
    // it waited for the timeout, and no more: the hook runs when the handler ends
    REQUIRE(elapsed >= 90ms);
    REQUIRE(elapsed < 800ms);
    REQUIRE(hooks == 0);
    REQUIRE(eventually([&] { return hooks.load() == 1; }));
}

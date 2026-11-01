#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/sse_connection.hpp>
#include <thinger/http/server/websocket_connection.hpp>
#include <thinger/util/types.hpp>
#include <boost/asio.hpp>
#include <boost/asio/use_future.hpp>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include "../fixtures/handler_probe.hpp"

using namespace thinger;
using namespace std::chrono_literals;

// In-memory dispatch: no listener, no sockets

namespace {
    std::shared_ptr<http::http_request> make_request(http::method method, const std::string& path,
                                                     const std::string& body = "",
                                                     const std::string& authorization = "") {
        auto request = http::http_request::create_http_request(method, "http://localhost" + path);
        if (!body.empty()) request->set_content(body, "application/json");
        if (!authorization.empty()) request->add_header("Authorization", authorization);
        return request;
    }

    std::shared_ptr<http::http_response> run_dispatch(http::server& server, std::shared_ptr<http::http_request> request,
                                                      http::dispatch_options options = {}) {
        boost::asio::io_context ioc;
        auto result = co_spawn(ioc, server.dispatch(std::move(request), std::move(options)), boost::asio::use_future);
        ioc.run();
        return result.get();
    }
}

TEST_CASE("In-memory dispatch runs routes like network requests", "[server][dispatch][integration]") {
    http::server server;
    std::atomic<int> handler_calls{0};

    server.use([](http::request& req, http::response& res) -> thinger::awaitable<bool> {
        if (req.get_matched_route() && req.get_matched_route()->has_meta("auth") && req.header("Authorization") != "Bearer ok") {
            res.error(http::http_response::status::unauthorized, "denied");
            co_return false;
        }
        co_return true;
    });

    server.get("/sync/:id", [](http::request& req, http::response& res) {
        res.json({{"id", req["id"]}, {"ip", req.get_request_ip()}, {"query", req.query("q")}});
    });

    server.get("/coro", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 20ms);
        co_await timer.async_wait(thinger::use_awaitable);
        res.send("coro-ok");
    });

    server.post("/devices", [&handler_calls](nlohmann::json& body, http::response& res) -> thinger::awaitable<void> {
        handler_calls++;
        res.json({{"created", body["name"]}}, http::http_response::status::created);
        co_return;
    }).schema({{"type", "object"}, {"required", {"name"}}});

    server.get("/private", [](http::response& res) { res.send("secret"); }).meta("auth", true);

    server.get("/chunked", [](http::response& res) {
        res.start_chunked("text/plain");
        res.write_chunk("hello ");
        res.write_chunk("world");
        res.end_chunked();
    });

    // Deferred body route reading the body itself
    server.put("/stream", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
        std::string received;
        uint8_t buf[4];
        while (size_t bytes = co_await req.read_some(buf, sizeof(buf))) {
            received.append(reinterpret_cast<char*>(buf), bytes);
        }
        res.json({{"received", received}});
    });

    server.get("/throws", [](http::response& res) -> thinger::awaitable<void> {
        throw std::runtime_error("handler failure");
        co_return;
    });

    SECTION("Synchronous route with parameters, query and remote IP") {
        auto response = run_dispatch(server, make_request(http::method::GET, "/sync/42?q=x"), {.remote_ip = "10.0.0.7"});
        REQUIRE(response->get_status_code() == 200);
        auto json = nlohmann::json::parse(response->get_content());
        REQUIRE(json["id"] == "42");
        REQUIRE(json["query"] == "x");
        REQUIRE(json["ip"] == "10.0.0.7");
    }

    SECTION("Remote IP is empty unless given") {
        auto response = run_dispatch(server, make_request(http::method::GET, "/sync/1"));
        REQUIRE(nlohmann::json::parse(response->get_content())["ip"] == "");
    }

    SECTION("Coroutine route") {
        auto response = run_dispatch(server, make_request(http::method::GET, "/coro"));
        REQUIRE(response->get_status_code() == 200);
        REQUIRE(response->get_content() == "coro-ok");
    }

    SECTION("JSON body validated against the schema") {
        auto created = run_dispatch(server, make_request(http::method::POST, "/devices", R"({"name":"sensor"})"));
        REQUIRE(created->get_status_code() == 201);
        REQUIRE(nlohmann::json::parse(created->get_content())["created"] == "sensor");

        auto invalid = run_dispatch(server, make_request(http::method::POST, "/devices", R"({"other":1})"));
        REQUIRE(invalid->get_status_code() == 400);
        REQUIRE(handler_calls == 1);
    }

    SECTION("404 and 405") {
        REQUIRE(run_dispatch(server, make_request(http::method::GET, "/missing"))->get_status_code() == 404);
        REQUIRE(run_dispatch(server, make_request(http::method::PATCH, "/sync/1"))->get_status_code() == 405);
    }

    SECTION("Middlewares see the headers of the internal request") {
        REQUIRE(run_dispatch(server, make_request(http::method::GET, "/private"))->get_status_code() == 401);
        auto allowed = run_dispatch(server, make_request(http::method::GET, "/private", "", "Bearer ok"));
        REQUIRE(allowed->get_status_code() == 200);
        REQUIRE(allowed->get_content() == "secret");
    }

    SECTION("Chunked response is collected whole") {
        auto response = run_dispatch(server, make_request(http::method::GET, "/chunked"));
        REQUIRE(response->get_status_code() == 200);
        REQUIRE(response->get_content() == "hello world");
        REQUIRE_FALSE(response->has_header("Transfer-Encoding"));
    }

    SECTION("Deferred body route reads the body from memory") {
        auto response = run_dispatch(server, make_request(http::method::PUT, "/stream", "0123456789"));
        REQUIRE(response->get_status_code() == 200);
        REQUIRE(nlohmann::json::parse(response->get_content())["received"] == "0123456789");
    }

    SECTION("Exception in a handler gives 500") {
        REQUIRE(run_dispatch(server, make_request(http::method::GET, "/throws"))->get_status_code() == 500);
    }
}

TEST_CASE("In-memory dispatch of features that need a connection", "[server][dispatch][integration]") {
    http::server server;

    server.get("/ws", [](http::request& req, http::response& res) {
        res.upgrade_websocket({}, [](std::shared_ptr<http::websocket_connection>) {});
    });
    server.get("/sse", [](http::response& res) {
        res.start_sse([](std::shared_ptr<http::sse_connection>) {});
    });
    server.get("/tunnel", [](http::response& res) {
        res.take_over([](http::upgraded_connection) {});
    });

    for (std::string path : {"/ws", "/sse", "/tunnel"}) {
        DYNAMIC_SECTION(path << " answers 501") {
            auto request = make_request(http::method::GET, path);
            request->add_header("Upgrade", "websocket");
            request->add_header("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
            auto response = run_dispatch(server, request);
            REQUIRE(response->get_status_code() == 501);
        }
    }
}

TEST_CASE("In-memory dispatch of handlers answering later", "[server][dispatch][integration]") {
    http::server server;

    // Keeps a copy of the response and answers from another thread
    server.get("/later", [](http::response& res) {
        std::thread([res]() mutable {
            std::this_thread::sleep_for(50ms);
            res.send("late answer");
        }).detach();
    });

    server.get("/never", [](http::response&) {});

    // Coroutine that would hang far longer than the timeout, on a route its timeout interrupts
    std::atomic<bool> cancelled{false};
    server.get("/hangs", [&cancelled](http::response& res) -> thinger::awaitable<void> {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 30s);
        auto [ec] = co_await timer.async_wait(boost::asio::as_tuple(thinger::use_awaitable));
        if (ec == boost::asio::error::operation_aborted) cancelled = true;
        res.send("too late");
    }).cancel_on({http::cancel_reason::timeout});

    SECTION("Response sent later from a copy") {
        auto response = run_dispatch(server, make_request(http::method::GET, "/later"));
        REQUIRE(response->get_status_code() == 200);
        REQUIRE(response->get_content() == "late answer");
    }

    SECTION("No response within the timeout gives 504") {
        auto start = std::chrono::steady_clock::now();
        auto response = run_dispatch(server, make_request(http::method::GET, "/never"), {.timeout = 100ms});
        REQUIRE(response->get_status_code() == 504);
        REQUIRE(std::chrono::steady_clock::now() - start < 2s);
    }

    SECTION("Timeout also covers a handler that is still running, interrupted if its route declares it") {
        auto start = std::chrono::steady_clock::now();
        auto response = run_dispatch(server, make_request(http::method::GET, "/hangs"), {.timeout = 100ms});
        REQUIRE(response->get_status_code() == 504);
        REQUIRE(std::chrono::steady_clock::now() - start < 2s);
        REQUIRE(cancelled);
    }

    SECTION("Callback variant") {
        boost::asio::io_context ioc;
        std::shared_ptr<http::http_response> result;
        server.dispatch(ioc.get_executor(), make_request(http::method::GET, "/later"),
                        [&result](std::shared_ptr<http::http_response> response) { result = response; });
        ioc.run();
        REQUIRE(result);
        REQUIRE(result->get_content() == "late answer");
    }
}

TEST_CASE("In-memory dispatch follows the cancellation scope of the route", "[server][dispatch][cancellation][integration]") {
    using clock = std::chrono::steady_clock;
    http::test::handler_probe probe;
    http::server server;
    http::test::waiting_route(server, "/default", probe, 300ms);
    http::test::waiting_route(server, "/timeout", probe, 10s).cancel_on({http::cancel_reason::timeout});
    http::test::waiting_route(server, "/gone", probe, 10s).cancel_on({http::cancel_reason::client_gone});

    SECTION("Its timeout answers 504; a default route runs to its end, and finds the reason") {
        auto response = run_dispatch(server, make_request(http::method::GET, "/default"), {.timeout = 100ms});
        CHECK(response->get_status_code() == 504);
        // run_dispatch runs its io_context until the handler ends too
        REQUIRE(probe.ended);
        CHECK_FALSE(probe.interrupted);
        CHECK(probe.reason == http::cancel_reason::timeout);
        CHECK(probe.end_time() - probe.start_time() >= 290ms);
    }

    SECTION("Its timeout interrupts a route declaring it within 50 ms") {
        auto expiry = clock::now() + 100ms;
        auto response = run_dispatch(server, make_request(http::method::GET, "/timeout"), {.timeout = 100ms});
        CHECK(response->get_status_code() == 504);
        REQUIRE(probe.ended);
        INFO("interrupted " << probe.ended_after(expiry).count() << " ms after the timeout");
        CHECK(probe.interrupted);
        CHECK(probe.reason == http::cancel_reason::timeout);
        CHECK(probe.ended_after(expiry) <= 50ms);
    }

    SECTION("A caller cancelling the dispatch cancels the scope with client_gone") {
        bool declared = GENERATE(false, true);
        INFO((declared ? "cancel_on(client_gone)" : "default route"));
        boost::asio::io_context ioc;
        boost::asio::cancellation_signal signal;
        std::exception_ptr error;
        co_spawn(ioc, server.dispatch(make_request(http::method::GET, declared ? "/gone" : "/default")),
                 boost::asio::bind_cancellation_slot(signal.slot(),
                     [&error](std::exception_ptr e, std::shared_ptr<http::http_response>) { error = e; }));
        boost::asio::steady_timer timer(ioc, 50ms);
        timer.async_wait([&signal](const boost::system::error_code&) {
            signal.emit(boost::asio::cancellation_type::terminal);
        });
        // Until the handler ends too
        ioc.run();

        // The dispatch ends with operation_aborted
        REQUIRE(error);
        boost::system::error_code ec;
        try {
            std::rethrow_exception(error);
        } catch (const boost::system::system_error& e) {
            ec = e.code();
        }
        CHECK(ec == boost::asio::error::operation_aborted);
        // And its handler is interrupted only where the route declared it
        REQUIRE(probe.ended);
        CHECK(probe.reason == http::cancel_reason::client_gone);
        CHECK(probe.interrupted == declared);
    }
}

TEST_CASE("In-memory dispatch replaces every Content-Length header", "[server][dispatch][integration]") {
    http::server server;
    server.post("/echo", [](http::request& req, http::response& res) {
        res.send("body=[" + req.body() + "]");
    });

    // The body length is computed again: stale or repeated Content-Length headers of the
    // request (added by hand) do not make it look ambiguous
    SECTION("Repeated with the body length") {
        auto request = make_request(http::method::POST, "/echo", "hello");
        request->add_header("Content-Length", "5");
        auto response = run_dispatch(server, request);
        REQUIRE(response->get_status() == http::http_response::status::ok);
        REQUIRE(response->get_content() == "body=[hello]");
    }
    SECTION("Repeated with other values") {
        auto request = make_request(http::method::POST, "/echo", "hello");
        request->add_header("Content-Length", "1");
        request->add_header("Content-Length", "2");
        auto response = run_dispatch(server, request);
        REQUIRE(response->get_status() == http::http_response::status::ok);
        REQUIRE(response->get_content() == "body=[hello]");
        REQUIRE(request->get_headers_with_key("Content-Length") == std::vector<std::string>{"5"});
    }
}

// Host is required only of requests received from a connection: a dispatched request is built
// by its caller (the core dispatches requests made with set_method() and set_uri() only)
TEST_CASE("In-memory dispatch serves requests without Host", "[server][dispatch][host][integration]") {
    http::server server;
    server.get("/items/:id", [](http::request& req, http::response& res) {
        res.send("id=[" + req["id"] + "] host=[" + req.get_http_request()->get_host() + "]");
    });

    auto request = std::make_shared<http::http_request>();
    request->set_method(http::method::GET);
    request->set_uri("/items/7");
    REQUIRE_FALSE(request->has_header("Host"));
    REQUIRE(request->get_http_version_major() == 1);
    REQUIRE(request->get_http_version_minor() == 1);

    auto response = run_dispatch(server, request);
    REQUIRE(response->get_status() == http::http_response::status::ok);
    REQUIRE(response->get_content() == "id=[7] host=[]");
}

TEST_CASE("Response copies share the response", "[server][dispatch][response][integration]") {
    http::server server;
    std::atomic<bool> original_saw_answer{false};

    server.get("/copy", [&original_saw_answer](http::response& res) {
        http::response copy = res;
        copy.header("X-From", "copy");
        copy.send("from copy");

        // The original sees the answer, and cannot send another one
        original_saw_answer = res.has_responded();
        res.send("from original");
    });

    auto response = run_dispatch(server, make_request(http::method::GET, "/copy"));
    REQUIRE(original_saw_answer);
    REQUIRE(response->get_content() == "from copy");
    REQUIRE(response->get_header("X-From") == "copy");
}

namespace {
    void function_handler(http::request& req, http::response& res) {
        res.send("function " + req["id"]);
    }

    thinger::awaitable<void> delayed_send(http::response& res, std::string text) {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 5ms);
        co_await timer.async_wait(thinger::use_awaitable);
        res.send(text);
    }
}

TEST_CASE("Route registration accepts every callback form on every method", "[server][dispatch][routes][integration]") {
    http::server server;
    const std::vector<http::method> methods = {http::method::GET, http::method::POST, http::method::PUT,
        http::method::DELETE, http::method::PATCH, http::method::HEAD, http::method::OPTIONS};

    // Coroutines answering after a suspension: they would never answer if they were
    // taken as synchronous callbacks (and their awaitable discarded)
    auto group = server.group("/group");
    for (auto method : methods) {
        auto name = http::get_method(method);
        auto coroutine = [name](http::response& res) -> thinger::awaitable<void> {
            co_await delayed_send(res, "coroutine " + name);
        };
        auto json_coroutine = [name](nlohmann::json& body, http::response& res) -> thinger::awaitable<void> {
            co_await delayed_send(res, "json " + name + " " + body["value"].get<std::string>());
        };
        switch (method) {
            case http::method::GET: server.get("/coro", coroutine); group.get("/json", json_coroutine); break;
            case http::method::POST: server.post("/coro", coroutine); group.post("/json", json_coroutine); break;
            case http::method::PUT: server.put("/coro", coroutine); group.put("/json", json_coroutine); break;
            case http::method::DELETE: server.del("/coro", coroutine); group.del("/json", json_coroutine); break;
            case http::method::PATCH: server.patch("/coro", coroutine); group.patch("/json", json_coroutine); break;
            case http::method::HEAD: server.head("/coro", coroutine); group.head("/json", json_coroutine); break;
            default: server.options("/coro", coroutine); group.options("/json", json_coroutine); break;
        }
    }

    // Synchronous forms: function, mutable lambda, std::function, JSON body
    server.get("/function/:id", function_handler);
    int calls = 0;
    server.get("/mutable", [calls](http::response& res) mutable { res.send("call " + std::to_string(++calls)); });
    http::route_callback_request_json_response std_function = [](http::request&, nlohmann::json& body, http::response& res) {
        res.json(body);
    };
    server.post("/std-function", std_function);
    server.set_not_found_handler([](http::response& res) -> thinger::awaitable<void> {
        co_await delayed_send(res, "fallback");
    });

    for (auto method : methods) {
        DYNAMIC_SECTION("Coroutines on " << http::get_method(method)) {
            // (a response to HEAD has no content, but the Content-Length of its body)
            auto require_body = [method](const std::shared_ptr<http::http_response>& response, const std::string& body) {
                if (method == http::method::HEAD) {
                    REQUIRE(response->get_content().empty());
                    REQUIRE(response->get_header("Content-Length") == std::to_string(body.size()));
                } else {
                    REQUIRE(response->get_content() == body);
                }
            };
            require_body(run_dispatch(server, make_request(method, "/coro")), "coroutine " + http::get_method(method));
            require_body(run_dispatch(server, make_request(method, "/group/json", R"({"value":"v"})")),
                         "json " + http::get_method(method) + " v");
        }
    }

    SECTION("Synchronous forms") {
        REQUIRE(run_dispatch(server, make_request(http::method::GET, "/function/7"))->get_content() == "function 7");
        REQUIRE(run_dispatch(server, make_request(http::method::GET, "/mutable"))->get_content() == "call 1");
        REQUIRE(run_dispatch(server, make_request(http::method::GET, "/mutable"))->get_content() == "call 2");
        auto response = run_dispatch(server, make_request(http::method::POST, "/std-function", R"({"a":1})"));
        REQUIRE(nlohmann::json::parse(response->get_content()) == nlohmann::json{{"a", 1}});
        REQUIRE(run_dispatch(server, make_request(http::method::POST, "/std-function", "{bad"))->get_status_code() == 400);
    }

    SECTION("Coroutine fallback") {
        REQUIRE(run_dispatch(server, make_request(http::method::GET, "/missing"))->get_content() == "fallback");
    }
}

TEST_CASE("Empty not found handlers answer 404 and 405", "[server][dispatch][routes][integration]") {
    http::server server;
    server.get("/exists", [](http::response& res) { res.send("exists"); });
    server.set_not_found_handler([](http::response& res) { res.send("fallback"); });
    REQUIRE(run_dispatch(server, make_request(http::method::GET, "/missing"))->get_content() == "fallback");

    SECTION("nullptr removes the handler") {
        server.set_not_found_handler(nullptr);
    }

    SECTION("An empty std::function removes the handler") {
        server.set_not_found_handler(std::function<void(http::response&)>{});
    }

    SECTION("An empty coroutine std::function removes the handler") {
        server.set_not_found_handler(std::function<thinger::awaitable<void>(http::request&, http::response&)>{});
    }

    SECTION("A null function pointer removes the handler") {
        void (*handler)(http::request&, http::response&) = nullptr;
        server.set_not_found_handler(handler);
    }

    REQUIRE(run_dispatch(server, make_request(http::method::GET, "/missing"))->get_status_code() == 404);
    REQUIRE(run_dispatch(server, make_request(http::method::POST, "/missing"))->get_status_code() == 405);
}

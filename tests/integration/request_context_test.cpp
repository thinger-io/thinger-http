#include "server_test_helpers.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/client/client.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace thinger;
using namespace std::chrono_literals;
using server_test::make_request;
using server_test::run_dispatch;

// Values stored by middlewares in the request context, read by the handlers, over the
// network and in memory (dispatch)

namespace {
    struct principal {
        std::string user;
        std::vector<std::string> scopes;
    };

    // Counts the instances destroyed (moved-from ones excluded)
    std::atomic<int> destroyed_values{0};

    struct counted {
        explicit counted(std::string label) : label(std::move(label)) {}
        counted(counted&& other) noexcept : label(std::move(other.label)), live(std::exchange(other.live, false)) {}
        counted(const counted& other) = default;
        counted& operator=(const counted&) = delete;
        counted& operator=(counted&&) = delete;
        ~counted() { if (live) destroyed_values++; }
        std::string label;
        bool live = true;
    };

    // Keys of the application: two of them of the same type
    inline constexpr http::context_key<principal> principal_key{"principal"};
    inline constexpr http::context_key<std::string> trace_key{"trace"};
    inline constexpr http::context_key<std::string> tenant_key{"tenant"};
    inline constexpr http::context_key<counted> counted_key{"counted"};
    inline constexpr http::context_key<std::unique_ptr<principal>> owned_key{"owned"};

    principal principal_of(const std::string& user) {
        return principal{user, {user + ":read", user + ":write"}};
    }

    nlohmann::json principal_json(const principal& who) {
        return {{"user", who.user}, {"scopes", who.scopes}};
    }

    // Answers with the principal stored by the middlewares, or 401 without one
    void answer_principal(http::request& req, http::response& res) {
        const principal* who = req.context().get(principal_key);
        if (!who) {
            res.error(http::http_response::status::unauthorized, "no principal");
            return;
        }
        res.json(principal_json(*who));
    }

    // Middlewares of every kind storing a principal (each one for its X-Auth value), and
    // the routes reading what they stored
    void configure(http::server& server) {
        // Asynchronous middleware
        server.use([](http::request& req, http::response&) -> thinger::awaitable<bool> {
            if (req.header("X-Auth") == "async") req.context().set(principal_key, principal_of("alice"));
            co_return true;
        });

        // Synchronous middleware with next()
        server.use([](http::request& req, http::response&, std::function<void()> next) {
            if (req.header("X-Auth") == "sync") req.context().set(principal_key, principal_of("bob"));
            next();
        });

        // Coroutine middleware with next(), storing after a suspension
        server.use([](http::request& req, http::response&, std::function<void()> next) -> thinger::awaitable<void> {
            if (req.header("X-Auth") == "coroutine") {
                boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 5ms);
                co_await timer.async_wait(thinger::use_awaitable);
                req.context().set(principal_key, principal_of("carol"));
            }
            next();
        });

        // Two values of the same type under two keys; the trace replaces one seeded by a dispatch
        server.use([](http::request& req, http::response&) -> thinger::awaitable<bool> {
            if (auto trace = req.header("X-Trace"); !trace.empty()) req.context().set(trace_key, trace);
            if (auto tenant = req.header("X-Tenant"); !tenant.empty()) req.context().set(tenant_key, tenant);
            co_return true;
        });

        // A value replaced by a later middleware; a move-only value
        server.use([](http::request& req, http::response&) -> thinger::awaitable<bool> {
            auto path = req.get_http_request()->get_path();
            if (path == "/replaced" || path == "/blocked") req.context().emplace(counted_key, "first");
            if (path == "/owned") req.context().set(owned_key, std::make_unique<principal>(principal_of("dave")));
            co_return true;
        });
        server.use([](http::request& req, http::response& res) -> thinger::awaitable<bool> {
            auto path = req.get_http_request()->get_path();
            if (path == "/replaced") req.context().emplace(counted_key, "second");
            if (path == "/blocked") {
                res.error(http::http_response::status::unauthorized, "blocked");
                co_return false;
            }
            // Requests matching no route run the middlewares too, with the context
            if (!req.get_matched_route()) {
                const std::string* trace = req.context().get(trace_key);
                res.error(http::http_response::status::not_found, "no route; trace " + (trace ? *trace : "none"));
                co_return false;
            }
            co_return true;
        });

        server.get("/me", answer_principal);
        server.get("/me-coroutine", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
            answer_principal(req, res);
            co_return;
        });
        server.post("/me-json", [](http::request& req, nlohmann::json& body, http::response& res) {
            const principal* who = req.context().get(principal_key);
            res.json({{"user", who ? who->user : ""}, {"body", body}});
        });
        server.get("/values", [](http::request& req, http::response& res) {
            const std::string* trace = req.context().get(trace_key);
            const std::string* tenant = req.context().get(tenant_key);
            res.json({{"trace", trace ? nlohmann::json(*trace) : nlohmann::json()},
                      {"tenant", tenant ? nlohmann::json(*tenant) : nlohmann::json()}});
        });
        server.get("/replaced", [](http::request& req, http::response& res) {
            // the first value is destroyed once replaced, before the handler runs
            res.json({{"label", req.context().get(counted_key)->label}, {"destroyed", destroyed_values.load()}});
        });
        server.get("/owned", [](http::request& req, http::response& res) {
            const std::unique_ptr<principal>* owned = req.context().get(owned_key);
            res.json(principal_json(**owned));
        });
        server.get("/blocked", [](http::response& res) { res.send("unreachable"); });
        server.get("/hangs", [](http::response& res) -> thinger::awaitable<void> {
            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 30s);
            co_await timer.async_wait(boost::asio::as_tuple(thinger::use_awaitable));
            res.send("too late");
        });
    }

    struct result {
        int status = 0;
        std::string body;
    };

    // The same request, over the network and in memory
    result over_network(uint16_t port, http::method method, const std::string& path,
                        http::headers_map headers = {}, const std::string& body = "") {
        http::client client;
        client.timeout(10s);
        auto url = "http://127.0.0.1:" + std::to_string(port) + path;
        auto response = method == http::method::POST ? client.post(url, body, "application/json", headers)
                                                      : client.get(url, headers);
        return {response.status(), response.body()};
    }

    result in_memory(http::server& server, http::method method, const std::string& path,
                     const http::headers_map& headers = {}, const std::string& body = "",
                     http::dispatch_options options = {}) {
        auto request = make_request(method, path, body);
        for (const auto& [name, value] : headers) request->add_header(name, value);
        auto response = run_dispatch(server, request, std::move(options));
        return {response->get_status_code(), response->get_content()};
    }

    // Waits until `count` values were destroyed (the server may destroy the request after
    // the client got the response)
    bool destroyed_reaches(int count) {
        for (int i = 0; i < 200 && destroyed_values < count; ++i) std::this_thread::sleep_for(10ms);
        return destroyed_values == count;
    }
}

TEST_CASE("Middlewares store typed values the handlers read", "[server][context][integration]") {
    http::server server;
    configure(server);
    server_test::running_server running(server);

    // Each request, over the network and dispatched in memory, gives the same response
    auto both = [&](http::method method, const std::string& path, const http::headers_map& headers = {},
                    const std::string& body = "") {
        auto network = over_network(running.port, method, path, headers, body);
        auto memory = in_memory(server, method, path, headers, body);
        REQUIRE(network.status == memory.status);
        REQUIRE(network.body == memory.body);
        return network;
    };

    SECTION("Every kind of middleware stores the principal the handler reads") {
        struct expected { std::string auth; std::string user; };
        for (const auto& [auth, user] : {expected{"async", "alice"}, expected{"sync", "bob"}, expected{"coroutine", "carol"}}) {
            auto response = both(http::method::GET, "/me", {{"X-Auth", auth}});
            REQUIRE(response.status == 200);
            auto body = nlohmann::json::parse(response.body);
            REQUIRE(body["user"] == user);
            REQUIRE(body["scopes"] == nlohmann::json{user + ":read", user + ":write"});
        }
    }

    SECTION("Every kind of route callback taking the request reads it") {
        auto response = both(http::method::GET, "/me-coroutine", {{"X-Auth", "sync"}});
        REQUIRE(nlohmann::json::parse(response.body)["user"] == "bob");
        response = both(http::method::POST, "/me-json", {{"X-Auth", "async"}}, R"({"a":1})");
        REQUIRE(response.status == 200);
        REQUIRE(nlohmann::json::parse(response.body) == nlohmann::json{{"user", "alice"}, {"body", {{"a", 1}}}});
    }

    SECTION("Without a stored principal the handler reads it as absent") {
        auto response = both(http::method::GET, "/me");
        REQUIRE(response.status == 401);
        REQUIRE(response.body == "no principal");
    }

    SECTION("Two values of the same type live under two keys") {
        auto response = both(http::method::GET, "/values", {{"X-Trace", "trace-1"}, {"X-Tenant", "acme"}});
        REQUIRE(nlohmann::json::parse(response.body) == nlohmann::json{{"trace", "trace-1"}, {"tenant", "acme"}});
        response = both(http::method::GET, "/values", {{"X-Tenant", "acme"}});
        REQUIRE(nlohmann::json::parse(response.body) == nlohmann::json{{"trace", nullptr}, {"tenant", "acme"}});
    }

    SECTION("A value stored again replaces the previous one, which is destroyed") {
        destroyed_values = 0;
        auto network = over_network(running.port, http::method::GET, "/replaced");
        REQUIRE(nlohmann::json::parse(network.body) == nlohmann::json{{"label", "second"}, {"destroyed", 1}});
        REQUIRE(destroyed_reaches(2));

        destroyed_values = 0;
        auto memory = in_memory(server, http::method::GET, "/replaced");
        REQUIRE(memory.body == network.body);
        REQUIRE(destroyed_values == 2);
    }

    SECTION("Move-only values") {
        auto response = both(http::method::GET, "/owned");
        REQUIRE(response.status == 200);
        REQUIRE(nlohmann::json::parse(response.body)["user"] == "dave");
    }

    SECTION("A value is destroyed when a middleware stops the chain") {
        destroyed_values = 0;
        auto network = over_network(running.port, http::method::GET, "/blocked");
        REQUIRE(network.status == 401);
        REQUIRE(destroyed_reaches(1));

        destroyed_values = 0;
        auto memory = in_memory(server, http::method::GET, "/blocked");
        REQUIRE(memory.status == 401);
        REQUIRE(destroyed_values == 1);
    }

    SECTION("Requests matching no route have a context too") {
        auto response = both(http::method::GET, "/missing", {{"X-Trace", "trace-2"}});
        REQUIRE(response.status == 404);
        REQUIRE(response.body == "no route; trace trace-2");
    }
}

TEST_CASE("In-memory dispatch seeds the request context", "[server][context][dispatch][integration]") {
    http::server server;
    configure(server);

    auto seeded = [](std::string trace) {
        http::dispatch_options options;
        options.context.set(trace_key, std::move(trace));
        return options;
    };

    SECTION("Middlewares and handlers read the seeded values") {
        auto response = in_memory(server, http::method::GET, "/values", {}, "", seeded("seeded"));
        REQUIRE(nlohmann::json::parse(response.body)["trace"] == "seeded");

        http::dispatch_options options{.remote_ip = "10.0.0.7"};
        options.context.set(principal_key, principal_of("erin"));
        response = in_memory(server, http::method::GET, "/me", {}, "", options);
        REQUIRE(response.status == 200);
        REQUIRE(nlohmann::json::parse(response.body)["user"] == "erin");
        // the options are copied (values included), so they can be dispatched again
        response = in_memory(server, http::method::GET, "/me", {}, "", options);
        REQUIRE(nlohmann::json::parse(response.body)["user"] == "erin");
    }

    SECTION("A middleware replaces a seeded value") {
        auto response = in_memory(server, http::method::GET, "/values", {{"X-Trace", "middleware"}}, "", seeded("seeded"));
        REQUIRE(nlohmann::json::parse(response.body)["trace"] == "middleware");
    }

    SECTION("Values do not leak to the next dispatch") {
        auto response = in_memory(server, http::method::GET, "/values", {}, "", seeded("seeded"));
        REQUIRE(nlohmann::json::parse(response.body)["trace"] == "seeded");
        response = in_memory(server, http::method::GET, "/values");
        REQUIRE(nlohmann::json::parse(response.body)["trace"] == nullptr);
        response = in_memory(server, http::method::GET, "/me", {}, "", {});
        REQUIRE(response.status == 401);
    }

    SECTION("Values are destroyed when the dispatch times out") {
        destroyed_values = 0;
        http::dispatch_options options{.timeout = 100ms};
        options.context.emplace(counted_key, "seeded");
        // (the copy passed to the dispatch holds another instance)
        auto response = in_memory(server, http::method::GET, "/hangs", {}, "", options);
        REQUIRE(response.status == 504);
        REQUIRE(destroyed_values == 1);
    }
}

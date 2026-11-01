#include "server_test_helpers.hpp"
#include <thinger/http/client/client.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/pool_server.hpp>
#include <thinger/asio/ssl/certificate_manager.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include "../fixtures/running_workers.hpp"

// One server, several listeners (B1): an application listens on TCP, TLS and Unix sockets at
// once, every listener serving the same routes, middlewares and hooks, whenever they were added.
// And the connection options of each server (idle timeout, lingering close, logging), with no
// state shared between the servers of a process

using namespace thinger;
using namespace std::chrono_literals;
using server_test::count;
using thinger::http::test::listen_options;
using thinger::http::test::raw_connection;
using thinger::http::test::transport;
using thinger::http::test::transport_endpoint;
using thinger::http::test::transport_listener;

namespace {

    // A server listening on the transports asked for, one listener each (see listen()), served
    // on a thread of its own once run() is called, until destroyed
    struct multi_listener_server {
        http::server server;
        std::vector<std::unique_ptr<transport_listener>> listeners;
        std::thread thread;

        const transport_endpoint& listen(transport kind) {
            listeners.push_back(std::make_unique<transport_listener>(server, kind));
            return listeners.back()->endpoint();
        }

        void run() {
            thread = std::thread([this] { server.wait(); });
        }

        ~multi_listener_server() {
            server.stop();
            if (thread.joinable()) thread.join();
        }
    };

    // GET `path` on `to` with a client of its own
    http::client_response get(const transport_endpoint& to, const std::string& path) {
        http::client client;
        to.configure(client).timeout(10s);
        return client.get(to.url(path));
    }

    std::string url_of(const transport_endpoint& endpoint) {
        if (endpoint.kind == transport::unix) return "unix:" + endpoint.unix_path;
        return endpoint.scheme() + "://127.0.0.1:" + std::to_string(endpoint.port);
    }

    // The routes, a middleware and a response hook of the application, added after it listens
    struct late_application {
        std::atomic<int> responses{0};

        explicit late_application(http::server& server) {
            server.use([](http::request&, http::response& res, std::function<void()> next) {
                res.header("X-Middleware", "late");
                next();
            });
            server.get("/late", [](http::response& res) { res.send("late route"); });
            server.on_response([this](const http::request&, const http::response_info&) { ++responses; });
        }
    };

    // The route answered on `to`: the same response, middleware and hook on every listener
    void require_late_route(const transport_endpoint& to) {
        INFO("transport: " << to.kind);
        auto response = get(to, "/late");
        INFO("error: " << response.error());
        REQUIRE(response.status() == 200);
        REQUIRE(response.body() == "late route");
        REQUIRE(response.header("X-Middleware") == "late");
    }

    // Wait up to `timeout` for `done()`
    template<typename Condition>
    bool eventually(Condition done, std::chrono::milliseconds timeout = 3s) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(10ms);
        }
        return true;
    }

    const std::string ping = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";

    // A raw connection to `to` that sent a request and read its response (kept alive)
    std::unique_ptr<raw_connection> served_connection(const transport_endpoint& to) {
        auto connection = std::make_unique<raw_connection>(to);
        REQUIRE(connection->write(ping));
        std::string data;
        REQUIRE(connection->read_until(data, "pong", 5s));
        REQUIRE(data.starts_with("HTTP/1.1 200"));
        return connection;
    }

}

TEST_CASE("One server serves HTTP and HTTPS from two listeners", "[server][listeners][ssl][integration]") {
    multi_listener_server fixture;
    auto& http_endpoint = fixture.listen(transport::tcp);
    auto& https_endpoint = fixture.listen(transport::tls);

    // Routes, middleware and hook added after both listen()
    late_application application(fixture.server);
    fixture.run();

    SECTION("A route added after listen() answers on both, through the same middleware and hook") {
        require_late_route(http_endpoint);
        require_late_route(https_endpoint);
        REQUIRE(eventually([&] { return application.responses == 2; }));
    }

    SECTION("listening_on() names both, with the ports the system chose") {
        REQUIRE(http_endpoint.port != 0);
        REQUIRE(https_endpoint.port != 0);
        REQUIRE(http_endpoint.port != https_endpoint.port);
        REQUIRE(fixture.server.listening_on() ==
                std::vector<std::string>{"http://127.0.0.1:" + std::to_string(http_endpoint.port),
                                         "https://127.0.0.1:" + std::to_string(https_endpoint.port)});
        REQUIRE(fixture.server.is_listening());
    }
}

TEST_CASE("A third listener on a Unix socket serves the same application", "[server][listeners][unix][integration]") {
    multi_listener_server fixture;
    fixture.listen(transport::tcp);
    fixture.listen(transport::tls);
    fixture.listen(transport::unix);
    late_application application(fixture.server);
    fixture.run();

    std::vector<std::string> urls;
    for (const auto& listener : fixture.listeners) {
        require_late_route(listener->endpoint());
        urls.push_back(url_of(listener->endpoint()));
    }
    REQUIRE(urls.back().starts_with("unix:/"));
    REQUIRE(fixture.server.listening_on() == urls);
    REQUIRE(eventually([&] { return application.responses == 3; }));
}

TEST_CASE("A listen() that fails leaves the listeners open serving", "[server][listeners][integration]") {
    multi_listener_server fixture;
    fixture.listen(transport::tcp);
    fixture.listen(transport::unix);
    late_application application(fixture.server);
    fixture.run();
    auto before = fixture.server.listening_on();
    REQUIRE(before.size() == 2);

    SECTION("A port taken by another socket") {
        boost::asio::io_context ioc;
        boost::asio::ip::tcp::acceptor taken(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
        auto options = listen_options("127.0.0.1", taken.local_endpoint().port());
        options.max_listening_attempts = 1;
        auto ec = fixture.server.listen(std::move(options));
        INFO("listen: " << ec.message());
        REQUIRE(ec == boost::asio::error::address_in_use);
    }

    SECTION("TLS with a certificate manager without a default certificate") {
        auto manager = std::make_shared<asio::certificate_manager>();
        manager->remove_default_certificate();
        auto ec = fixture.server.listen(listen_options("127.0.0.1", 0, manager));
        INFO("listen: " << ec.message());
        REQUIRE(ec == boost::asio::error::invalid_argument);
    }

    SECTION("An endpoint the server listens on already") {
        auto ec = fixture.server.listen(http::test::unix_listen_options(fixture.listeners.back()->endpoint().unix_path));
        REQUIRE(ec == boost::asio::error::address_in_use);
    }

    // The failed one is not listed, and those open still serve
    REQUIRE(fixture.server.listening_on() == before);
    REQUIRE(fixture.server.is_listening());
    for (const auto& listener : fixture.listeners) require_late_route(listener->endpoint());
}

TEMPLATE_TEST_CASE("A server without listeners", "[server][listeners][integration]", http::server, http::pool_server) {
    http::test::running_workers workers;
    auto server = [&]() -> std::unique_ptr<http::server_base> {
        if constexpr (std::is_same_v<TestType, http::pool_server>) return std::make_unique<http::pool_server>(workers);
        else return std::make_unique<http::server>();
    }();

    SECTION("Never listened") {
        REQUIRE(server->listening_on().empty());
        REQUIRE_FALSE(server->is_listening());
    }

    SECTION("Its only listen() failed") {
        auto manager = std::make_shared<asio::certificate_manager>();
        manager->remove_default_certificate();
        auto ec = server->listen(listen_options("127.0.0.1", 0, manager));
        REQUIRE(ec == boost::asio::error::invalid_argument);
        REQUIRE(server->listening_on().empty());
        REQUIRE_FALSE(server->is_listening());
    }

    // stop() does nothing, and nothing was served
    server->stop();
    server->stop();
    REQUIRE(server->listening_on().empty());
    REQUIRE(server->stats().total_connections == 0);
}

TEST_CASE("stats() counts the connections of every listener", "[server][listeners][stats][integration]") {
    multi_listener_server fixture;
    fixture.server.get("/ping", [](http::response& res) { res.send("pong"); });
    std::vector<transport_endpoint> endpoints{fixture.listen(transport::tcp), fixture.listen(transport::tls),
                                              fixture.listen(transport::unix)};
    fixture.run();

    std::vector<std::unique_ptr<raw_connection>> connections;
    for (const auto& endpoint : endpoints) {
        INFO("transport: " << endpoint.kind);
        connections.push_back(served_connection(endpoint));
    }

    REQUIRE(eventually([&] { return fixture.server.stats().http_connections == 3; }));
    auto stats = fixture.server.stats();
    REQUIRE(stats.total_connections == 3);
    REQUIRE(stats.active_connections() == 3);

    // One closes: the sum follows
    connections.front()->close();
    REQUIRE(eventually([&] { return fixture.server.stats().http_connections == 2; }));
    REQUIRE(fixture.server.stats().total_connections == 3);
}

TEST_CASE("The connection options a server uses unless set", "[server][connection_options][unit]") {
    http::connection_options options;
    REQUIRE(options.idle_timeout == 120s);
    REQUIRE(options.linger_max_bytes == 8 * 1024 * 1024);
    REQUIRE(options.linger_timeout == 5000ms);
    REQUIRE_FALSE(options.logging.enabled);
}

TEST_CASE("Each server closes its idle connections after its own idle timeout", "[server][connection_options][timeout][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    // Two servers of one process: one closes after 1 s idle, the other has the default (120 s)
    multi_listener_server quick;
    http::connection_options options;
    options.idle_timeout = 1s;
    quick.server.set_connection_options(options);
    multi_listener_server patient;
    for (auto* fixture : {&quick, &patient}) {
        fixture->server.get("/ping", [](http::response& res) { res.send("pong"); });
        fixture->listen(kind);
        fixture->run();
    }

    SECTION("1 s: closed about 1 s after its last response") {
        auto quick_connection = served_connection(quick.listeners.front()->endpoint());
        auto start = std::chrono::steady_clock::now();
        auto result = quick_connection->read_until_closed(5s);
        auto took = std::chrono::steady_clock::now() - start;
        INFO("closed after " << std::chrono::duration_cast<std::chrono::milliseconds>(took).count() << " ms");
        REQUIRE(result.closed);
        REQUIRE(took >= 700ms);
        REQUIRE(took < 3s);
    }

    SECTION("The default: still open after 2 s, and serving") {
        auto patient_connection = served_connection(patient.listeners.front()->endpoint());
        auto result = patient_connection->read_until_closed(2s);
        REQUIRE_FALSE(result.closed);
        REQUIRE(patient_connection->write(ping));
        std::string data;
        REQUIRE(patient_connection->read_until(data, "pong", 5s));
        REQUIRE(count(data, "HTTP/1.1 200") == 1);
    }
}

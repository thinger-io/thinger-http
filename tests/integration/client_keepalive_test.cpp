#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <thinger/http_client.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/asio/workers.hpp>
#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include "../fixtures/test_ca.hpp"
#include "../fixtures/transport_matrix.hpp"
#include "../fixtures/running_workers.hpp"
#include "../fixtures/server_listen.hpp"

using namespace thinger;
using namespace std::chrono_literals;

// The keep-alive pool of the client (RFC 9112 section 9.3): sequential requests to a server
// share a connection, which is never shared by requests at the same time, nor kept when it
// cannot carry another request, and a connection the server closed while idle is replaced
// without the caller noticing (sending a request again only if that is safe)

namespace {

    using boost::asio::ip::tcp;
    using http::test::transport;
    using http::test::transport_endpoint;
    using unix_protocol = boost::asio::local::stream_protocol;
    using tls_stream = boost::asio::ssl::stream<tcp::socket>;

    // A server written by hand that answers requests on persistent connections as told, and
    // counts the connections accepted, the requests on each, and the connections the client
    // closed. It listens on any transport of the matrix: 127.0.0.1 over TCP and TLS (with a
    // certificate of the matrix CA for localhost), a socket in a temporary directory over Unix
    struct keepalive_server {
        // What to do with a request
        enum class then {
            keep,               // answer, and wait for the next request
            close,              // answer, and close the connection
            reset,              // answer, and reset the connection
            drop,               // close the connection without answering
        };
        struct reply {
            std::string response;
            then after = then::keep;
            std::chrono::milliseconds delay{0};     // before answering
        };
        struct received {
            int connection;     // 1, 2...
            int index;          // 1, 2... on its connection
            std::string request_line;
            std::string body;
        };

        boost::asio::io_context ioc;
        transport_endpoint endpoint;
        std::optional<tcp::acceptor> acceptor;
        std::optional<unix_protocol::acceptor> unix_acceptor;
        std::unique_ptr<http::test::temporary_directory> directory;
        std::unique_ptr<boost::asio::ssl::context> tls_context;
        std::atomic<int> accepts{0};
        std::atomic<int> closed_by_client{0};
        std::atomic<int> overlapped{0};     // requests received while one was being answered
        std::thread thread;

        // (shared with the thread of the server: TSan does not see the order sockets give)
        std::mutex mutex;
        std::vector<received> requests;
        std::function<reply(const received&)> respond_ = [](const received&) { return ok("hello"); };
        // Once idle for this long after an answer, the server closes (or resets) the connection
        std::chrono::milliseconds idle_close_{0};
        bool idle_reset_ = false;

        void respond(std::function<reply(const received&)> function) {
            std::lock_guard lock(mutex);
            respond_ = std::move(function);
        }

        void close_idle(std::chrono::milliseconds after, bool reset = false) {
            std::lock_guard lock(mutex);
            idle_close_ = after;
            idle_reset_ = reset;
        }

        explicit keepalive_server(transport kind = transport::tcp) {
            endpoint.kind = kind;
            if (kind == transport::unix) {
                directory = std::make_unique<http::test::temporary_directory>();
                endpoint.unix_path = (directory->path() / "s.sock").string();
                unix_acceptor.emplace(ioc, unix_protocol::endpoint(endpoint.unix_path));
                co_spawn(ioc, accept_loop(*unix_acceptor), boost::asio::detached);
            } else {
                if (kind == transport::tls) {
                    tls_context = std::make_unique<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
                    auto [certificate, key] = http::test::matrix_ca().issue("localhost");
                    tls_context->use_certificate_chain(boost::asio::buffer(certificate));
                    tls_context->use_private_key(boost::asio::buffer(key), boost::asio::ssl::context::pem);
                }
                acceptor.emplace(ioc, tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), 0});
                endpoint.port = acceptor->local_endpoint().port();
                co_spawn(ioc, accept_loop(*acceptor), boost::asio::detached);
            }
            thread = std::thread([this] { ioc.run(); });
        }

        ~keepalive_server() {
            ioc.stop();
            if (thread.joinable()) thread.join();
        }

        static reply ok(const std::string& body, const std::string& extra_headers = {}) {
            return {"HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra_headers +
                    "\r\n" + body};
        }

        uint16_t port() const { return endpoint.port; }

        std::string url(const std::string& path = "/") const { return endpoint.url(path); }

        // On another host name (TCP)
        std::string url(const std::string& path, const std::string& host) const {
            return "http://" + host + ":" + std::to_string(port()) + path;
        }

        // The origin the pool of a client keeps its connections to this server under
        std::string origin() const {
            return endpoint.kind == transport::unix ? "unix:" + endpoint.unix_path : endpoint.base_url();
        }

        // Point a client at this server (see transport_endpoint::configure())
        template<typename Client>
        Client& configure(Client& client) const { return endpoint.configure(client); }

        std::vector<received> received_requests() {
            std::lock_guard lock(mutex);
            return requests;
        }

        // Wait (a while) until `condition` holds
        static bool eventually(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 3s) {
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!condition()) {
                if (std::chrono::steady_clock::now() > deadline) return false;
                std::this_thread::sleep_for(5ms);
            }
            return true;
        }

        template<typename Acceptor>
        awaitable<void> accept_loop(Acceptor& listening) {
            while (true) {
                auto [ec, socket] = co_await listening.async_accept(use_nothrow_awaitable);
                if (ec) co_return;
                int connection = ++accepts;
                if constexpr (std::is_same_v<Acceptor, tcp::acceptor>) {
                    if (tls_context) {
                        co_spawn(ioc, [this, connection, stream = tls_stream(std::move(socket), *tls_context)]() mutable -> awaitable<void> {
                            auto [error] = co_await stream.async_handshake(boost::asio::ssl::stream_base::server,
                                                                           use_nothrow_awaitable);
                            if (!error) co_await session(std::move(stream), connection);
                        }, boost::asio::detached);
                        continue;
                    }
                }
                co_spawn(ioc, session(std::move(socket), connection), boost::asio::detached);
            }
        }

        // Close the connection (with a reset if `reset`), over TLS without a close_notify
        template<typename Stream>
        static void close(Stream& socket, bool reset = false) {
            boost::system::error_code ignored;
            auto& lowest = socket.lowest_layer();
            if (reset) {
                lowest.set_option(boost::asio::socket_base::linger(true, 0), ignored);
            } else {
                lowest.shutdown(boost::asio::socket_base::shutdown_both, ignored);
            }
            lowest.close(ignored);
        }

        template<typename Stream>
        awaitable<void> session(Stream socket, int connection) {
            std::string data;
            char buffer[4096];
            for (int index = 1;; ++index) {
                // The head of the next request, closing the connection if idle for too long
                size_t head_end;
                bool idle = true;
                std::chrono::milliseconds idle_close;
                bool idle_reset;
                {
                    std::lock_guard lock(mutex);
                    idle_close = idle_close_;
                    idle_reset = idle_reset_;
                }
                while ((head_end = data.find("\r\n\r\n")) == std::string::npos) {
                    boost::system::error_code ec;
                    size_t bytes = 0;
                    if (idle && index > 1 && idle_close.count() > 0) {
                        boost::asio::steady_timer timer(ioc, idle_close);
                        auto result = co_await (socket.async_read_some(boost::asio::buffer(buffer), use_nothrow_awaitable) ||
                                                timer.async_wait(use_nothrow_awaitable));
                        if (result.index() == 1) {
                            close(socket, idle_reset);
                            co_return;
                        }
                        std::tie(ec, bytes) = std::get<0>(result);
                    } else {
                        std::tie(ec, bytes) = co_await socket.async_read_some(boost::asio::buffer(buffer), use_nothrow_awaitable);
                    }
                    if (ec) {
                        if (data.empty()) closed_by_client++;
                        co_return;
                    }
                    idle = false;
                    data.append(buffer, bytes);
                }

                received request{connection, index, data.substr(0, data.find("\r\n")), {}};
                // A body with a Content-Length (all the client sends here)
                size_t length = 0;
                auto head = data.substr(0, head_end);
                for (auto& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (auto at = head.find("\r\ncontent-length:"); at != std::string::npos) {
                    length = std::stoul(head.substr(at + 17, head.find("\r\n", at + 2) - at - 17));
                }
                data.erase(0, head_end + 4);
                while (data.size() < length) {
                    auto [ec, bytes] = co_await socket.async_read_some(boost::asio::buffer(buffer), use_nothrow_awaitable);
                    if (ec) co_return;
                    data.append(buffer, bytes);
                }
                request.body = data.substr(0, length);
                data.erase(0, length);
                std::function<reply(const received&)> respond;
                {
                    std::lock_guard lock(mutex);
                    requests.push_back(request);
                    respond = respond_;
                }

                auto answer = respond(request);
                if (answer.delay.count() > 0) {
                    boost::asio::steady_timer pause(ioc, answer.delay);
                    co_await pause.async_wait(use_nothrow_awaitable);
                }
                // A client sending another request before this answer shares the connection
                boost::system::error_code ignored;
                if (!data.empty() || socket.lowest_layer().available(ignored) > 0) overlapped++;

                if (answer.after == then::drop) {
                    socket.lowest_layer().close(ignored);
                    co_return;
                }
                auto [ec, bytes] = co_await boost::asio::async_write(socket, boost::asio::buffer(answer.response),
                                                                     use_nothrow_awaitable);
                if (ec) co_return;
                if (answer.after == then::close || answer.after == then::reset) {
                    close(socket, answer.after == then::reset);
                    co_return;
                }
            }
        }
    };

    // An HTTP(S) server telling the port each request came from: one per connection. With TLS,
    // the certificates of its own manager
    struct port_server {
        std::shared_ptr<asio::certificate_manager> certificates = std::make_shared<asio::certificate_manager>();
        http::server server;
        uint16_t port = 0;
        std::thread thread;
        bool tls;

        explicit port_server(bool use_tls) : tls(use_tls) {
            server.get("/port", [](http::request& req, http::response& res) {
                res.send(req.connection().peer_port);
            });
            REQUIRE_FALSE(server.listen(http::test::listen_options("127.0.0.1", 0, tls ? certificates : nullptr)));
            port = http::test::listening_port(server);
            thread = std::thread([this] { server.wait(); });
        }

        ~port_server() {
            server.stop();
            if (thread.joinable()) thread.join();
        }

        std::string url(const std::string& host = "localhost") const {
            return std::string(tls ? "https://" : "http://") + host + ":" + std::to_string(port) + "/port";
        }
    };

    // Workers for the asynchronous clients, while it exists
    struct running_workers : http::test::running_workers {
        running_workers() : http::test::running_workers(4) {}
    };

    int connections_of(const std::vector<keepalive_server::received>& requests) {
        std::set<int> connections;
        for (const auto& request : requests) connections.insert(request.connection);
        return static_cast<int>(connections.size());
    }

}

TEST_CASE("HTTP client reuses a connection for sequential requests", "[http][client][pool][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    keepalive_server server(kind);

    SECTION("Synchronous client") {
        http::client client;
        server.configure(client).timeout(5s);
        for (int i = 0; i < 10; ++i) {
            auto res = client.get(server.url("/" + std::to_string(i)));
            REQUIRE(res.ok());
            REQUIRE(res.body() == "hello");
        }
        REQUIRE(server.accepts == 1);
        REQUIRE(server.received_requests().size() == 10);
        auto stats = client.pool_stats();
        REQUIRE(stats.idle == 1);
        REQUIRE(stats.active == 0);
        REQUIRE(stats.created == 1);
        REQUIRE(stats.reused == 9);
        REQUIRE(stats.hosts.size() == 1);
        REQUIRE(stats.hosts.begin()->first == server.origin());
    }

    SECTION("Asynchronous client, every method, with and without a body") {
        running_workers workers;
        http::async_client client(workers);
        server.configure(client).timeout(5s);
        bool all_ok = true;
        client.run([&]() -> awaitable<void> {
            for (int i = 0; i < 3; ++i) {
                all_ok &= (co_await client.get(server.url())).ok();
                all_ok &= (co_await client.post(server.url(), std::string("{\"a\":1}"))).ok();
                all_ok &= (co_await client.put(server.url(), std::string("data"), "text/plain")).ok();
                all_ok &= (co_await client.del(server.url())).ok();
                all_ok &= (co_await client.patch(server.url(), std::string("{}"))).ok();
                all_ok &= (co_await client.options(server.url())).ok();
            }
        });
        client.wait();
        REQUIRE(all_ok);
        REQUIRE(server.accepts == 1);
        auto requests = server.received_requests();
        REQUIRE(requests.size() == 18);
        REQUIRE(requests[1].body == "{\"a\":1}");
    }

    SECTION("Streaming responses, read whole") {
        http::client client;
        server.configure(client).timeout(5s);
        for (int i = 0; i < 3; ++i) {
            std::string streamed;
            auto result = client.request(server.url()).get([&](const http::stream_info& info) {
                streamed.append(info.data);
                return true;
            });
            REQUIRE(result.ok());
            REQUIRE(streamed == "hello");
        }
        REQUIRE(server.accepts == 1);
    }

    SECTION("Not after a streaming response aborted by its callback") {
        server.respond([](const keepalive_server::received&) { return keepalive_server::ok(std::string(100000, 'x')); });
        http::client client;
        server.configure(client).timeout(5s);
        auto result = client.request(server.url()).get([&](const http::stream_info&) { return false; });
        REQUIRE_FALSE(result.ok());
        REQUIRE(client.get(server.url()).ok());
        REQUIRE(server.accepts == 2);
    }

    SECTION("Redirects to the same server") {
        server.respond([&](const keepalive_server::received& request) {
            if (request.request_line.starts_with("GET /redirect")) {
                return keepalive_server::reply{"HTTP/1.1 302 Found\r\nLocation: " + server.url("/target") +
                                               "\r\nContent-Length: 0\r\n\r\n"};
            }
            return keepalive_server::ok("target");
        });
        http::client client;
        server.configure(client).timeout(5s);
        for (int i = 0; i < 3; ++i) {
            auto res = client.get(server.url("/redirect"));
            REQUIRE(res.ok());
            REQUIRE(res.body() == "target");
        }
        REQUIRE(server.accepts == 1);
        REQUIRE(server.received_requests().size() == 6);
    }
}

TEST_CASE("HTTP client reuses a TLS connection", "[http][client][pool][ssl][integration]") {
    http::test::test_ca ca;
    port_server server(true);
    http::test::server_certificate certificate(server.certificates, "localhost", ca.issue("localhost"));

    http::client client;
    client.timeout(5s).add_ca_pem(ca.pem());
    std::set<std::string> ports;
    for (int i = 0; i < 10; ++i) {
        auto res = client.get(server.url());
        REQUIRE(res.ok());
        ports.insert(res.body());
    }
    REQUIRE(ports.size() == 1);
    REQUIRE(client.pool_stats().reused == 9);

    SECTION("Different CAs trusted, different connections") {
        http::test::test_ca other;
        auto res = client.request(server.url()).ca_pem(other.pem()).get();
        REQUIRE(res.ok());
        REQUIRE_FALSE(ports.contains(res.body()));
        auto again = client.request(server.url()).ca_pem(other.pem()).get();
        REQUIRE(again.body() == res.body());
        REQUIRE(client.get(server.url()).body() == *ports.begin());
    }
}

TEST_CASE("HTTP client replaces connections the server closed while idle", "[http][client][pool][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    keepalive_server server(kind);
    bool reset = GENERATE(false, true);
    server.close_idle(100ms, reset);
    INFO((reset ? "reset" : "closed"));

    http::client client;
    server.configure(client).timeout(5s);
    REQUIRE(client.get(server.url("/first")).ok());

    // Closed by the server meanwhile: found so before reusing it, and the request goes on a
    // new connection, whatever its method (it was never sent on the closed one)
    std::this_thread::sleep_for(300ms);
    auto res = client.post(server.url("/second"), "payload");
    REQUIRE(res.ok());
    REQUIRE(server.accepts == 2);
    auto requests = server.received_requests();
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[1].connection == 2);
    REQUIRE(requests[1].body == "payload");

    std::this_thread::sleep_for(300ms);
    REQUIRE(client.get(server.url("/third")).ok());
    REQUIRE(server.accepts == 3);
}

TEST_CASE("HTTP client sends a request again only if it is safe", "[http][client][pool][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    // The server reads the second request on the connection and closes it without answering
    // (as when it closes an idle connection just as the request arrives): no byte of the
    // response was received
    keepalive_server server(kind);
    server.respond([](const keepalive_server::received& request) {
        if (request.connection == 1 && request.index == 2) return keepalive_server::reply{{}, keepalive_server::then::drop};
        return keepalive_server::ok("hello");
    });
    http::client client;
    server.configure(client).timeout(5s);
    REQUIRE(client.get(server.url("/first")).ok());

    SECTION("Idempotent: sent again on a new connection") {
        auto method = GENERATE(http::method::GET, http::method::HEAD, http::method::PUT, http::method::DELETE,
                               http::method::OPTIONS);
        auto request = client.create_request(method, server.url("/second"));
        if (method == http::method::PUT) request->set_content("data", "text/plain");
        auto res = client.send(request);
        REQUIRE(res.ok());
        auto requests = server.received_requests();
        REQUIRE(requests.size() == 3);
        REQUIRE(requests[1].connection == 1);
        REQUIRE(requests[2].connection == 2);
        REQUIRE(requests[2].request_line == requests[1].request_line);
    }

    SECTION("Not idempotent: the error is returned, the request is not sent twice") {
        auto method = GENERATE(http::method::POST, http::method::PATCH);
        auto request = client.create_request(method, server.url("/second"));
        request->set_content("{\"charge\":1}", "application/json");
        auto res = client.send(request);
        REQUIRE_FALSE(res.ok());
        REQUIRE(res.error_code());
        REQUIRE(server.received_requests().size() == 2);
        REQUIRE(server.accepts == 1);

        // The next request opens a new connection
        REQUIRE(client.get(server.url("/third")).ok());
        REQUIRE(server.accepts == 2);
    }

    SECTION("Not idempotent, with an idempotency key: sent again") {
        auto request = client.create_request(http::method::POST, server.url("/second"));
        request->set_content("{\"charge\":1}", "application/json");
        request->add_header("Idempotency-Key", "8e03978e");
        REQUIRE(client.send(request).ok());
        REQUIRE(server.received_requests().size() == 3);
    }

    SECTION("Streaming") {
        std::string streamed;
        auto result = client.request(server.url("/second")).get([&](const http::stream_info& info) {
            streamed.append(info.data);
            return true;
        });
        REQUIRE(result.ok());
        REQUIRE(streamed == "hello");
        REQUIRE(server.received_requests().size() == 3);
    }
}

TEST_CASE("HTTP client does not send a request again on a new connection, nor after a timeout", "[http][client][pool][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    keepalive_server server(kind);
    server.respond([](const keepalive_server::received& request) {
        if (request.index == 1) return keepalive_server::reply{{}, keepalive_server::then::drop};
        return keepalive_server::ok("hello");
    });
    http::client client;
    server.configure(client).timeout(5s);
    // Failed on a connection opened for it: the server got it and gave up, nothing to retry
    REQUIRE_FALSE(client.get(server.url()).ok());
    REQUIRE(server.received_requests().size() == 1);

    SECTION("Timeout on a reused connection") {
        server.respond([](const keepalive_server::received& request) {
            if (request.index == 1) return keepalive_server::ok("hello");
            return keepalive_server::reply{keepalive_server::ok("late").response, keepalive_server::then::keep, 2500ms};
        });
        REQUIRE(client.get(server.url()).ok());
        client.timeout(1s);
        REQUIRE_FALSE(client.get(server.url()).ok());
        REQUIRE(server.received_requests().size() == 3);
    }
}

TEST_CASE("HTTP client does not reuse connections that cannot carry another request", "[http][client][pool][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    struct reuse_case {
        std::string name;
        std::string response;
        bool reused;
    };
    auto test = GENERATE(
        reuse_case{"Connection: close", "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nok", false},
        reuse_case{"HTTP/1.0", "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok", false},
        reuse_case{"HTTP/1.0 keep-alive", "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 2\r\n\r\nok", true},
        reuse_case{"Truncated", "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nok", false},
        reuse_case{"Invalid", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nok", false},
        reuse_case{"Keep-Alive: timeout=1", "HTTP/1.1 200 OK\r\nKeep-Alive: timeout=1\r\nContent-Length: 2\r\n\r\nok", false},
        reuse_case{"Keep-Alive: timeout=5", "HTTP/1.1 200 OK\r\nKeep-Alive: timeout=5, max=100\r\nContent-Length: 2\r\n\r\nok", true});
    INFO(test.name);

    // The connection is left open by the server: the client decides
    keepalive_server server(kind);
    server.respond([&](const keepalive_server::received& request) {
        if (request.connection == 1 && request.index == 1) {
            // (truncated: the server closes it, as the body never completes otherwise)
            return keepalive_server::reply{test.response, test.name == "Truncated" ? keepalive_server::then::close
                                                                                  : keepalive_server::then::keep};
        }
        return keepalive_server::ok("ok");
    });
    http::client client;
    server.configure(client).timeout(5s);
    client.get(server.url());
    REQUIRE(client.get(server.url()).ok());
    REQUIRE(server.accepts == (test.reused ? 1 : 2));

    // A connection not kept was closed by the client
    if (!test.reused && test.name != "Truncated") {
        REQUIRE(keepalive_server::eventually([&] { return server.closed_by_client >= 1; }));
    }
}

TEST_CASE("HTTP client does not reuse a connection when the request asks to close it", "[http][client][pool][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    keepalive_server server(kind);
    http::client client;
    server.configure(client).timeout(5s);
    REQUIRE(client.get(server.url(), {{"Connection", "close"}}).ok());
    REQUIRE(client.get(server.url()).ok());
    REQUIRE(server.accepts == 2);
}

TEST_CASE("HTTP client honours the idle timeout the server announces", "[http][client][pool][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    // timeout=2: kept for 1 s (the margin is 1 s)
    keepalive_server server(kind);
    server.respond([](const keepalive_server::received&) {
        return keepalive_server::ok("ok", "Keep-Alive: timeout=2\r\n");
    });
    http::client client;
    server.configure(client).timeout(5s);
    REQUIRE(client.get(server.url()).ok());
    REQUIRE(client.get(server.url()).ok());
    REQUIRE(server.accepts == 1);
    std::this_thread::sleep_for(1300ms);
    REQUIRE(client.get(server.url()).ok());
    REQUIRE(server.accepts == 2);
}

TEST_CASE("HTTP client closes connections idle for too long", "[http][client][pool][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    keepalive_server server(kind);

    SECTION("Asynchronous client: closed by its timer") {
        running_workers workers;
        http::async_client client(workers);
        server.configure(client).timeout(5s).idle_timeout(200ms);
        bool ok = false;
        auto start = std::chrono::steady_clock::now();
        client.get(server.url(), [&](http::client_response& res) { ok = res.ok(); });
        client.wait();
        REQUIRE(ok);
        REQUIRE(client.pool_size() == 1);
        REQUIRE(keepalive_server::eventually([&] { return server.closed_by_client == 1; }));
        REQUIRE(std::chrono::steady_clock::now() - start >= 200ms);
        REQUIRE(client.pool_size() == 0);
    }

    SECTION("Synchronous client: not reused once expired") {
        http::client client;
        server.configure(client).timeout(5s).idle_timeout(200ms);
        REQUIRE(client.get(server.url()).ok());
        std::this_thread::sleep_for(300ms);
        REQUIRE(client.get(server.url()).ok());
        REQUIRE(server.accepts == 2);
        REQUIRE(keepalive_server::eventually([&] { return server.closed_by_client == 1; }));
    }

    SECTION("Synchronous client: closed as a later request ends, once expired") {
        // Expired while a request to another server is answered: closed as it ends, without
        // waiting for a next call
        keepalive_server slow(kind);
        slow.respond([](const keepalive_server::received&) {
            return keepalive_server::reply{keepalive_server::ok("slow").response, keepalive_server::then::keep, 400ms};
        });
        http::client client;
        server.configure(client).timeout(5s).idle_timeout(200ms);
        REQUIRE(client.get(server.url()).ok());
        // (one client reaches one Unix socket: the other one is the request's)
        auto to_slow = client.request(slow.url());
        if (kind == transport::unix) to_slow.unix_socket(slow.endpoint.unix_path);
        REQUIRE(to_slow.get().ok());
        REQUIRE(keepalive_server::eventually([&] { return server.closed_by_client == 1; }, 1s));
        auto stats = client.pool_stats();
        REQUIRE(stats.idle == 1);
        REQUIRE(stats.hosts.size() == 1);
        REQUIRE(slow.closed_by_client == 0);
    }
}

TEST_CASE("HTTP client closes its idle connections on demand", "[http][client][pool][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    // Between the calls of a synchronous client nothing runs: a long-lived one that makes
    // sporadic calls closes the connections it keeps
    keepalive_server server(kind);
    http::client client;
    server.configure(client).timeout(5s);
    REQUIRE(client.get(server.url()).ok());
    REQUIRE(client.pool_size() == 1);
    client.clear_connections();
    REQUIRE(client.pool_size() == 0);
    REQUIRE(keepalive_server::eventually([&] { return server.closed_by_client == 1; }, 1s));
    // Later calls connect again
    REQUIRE(client.get(server.url()).ok());
    REQUIRE(server.accepts == 2);
}

TEST_CASE("HTTP client never shares a busy connection, and bounds the idle ones", "[http][client][pool][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    running_workers workers;
    keepalive_server server(kind);
    server.respond([](const keepalive_server::received&) {
        return keepalive_server::reply{keepalive_server::ok("slow").response, keepalive_server::then::keep, 100ms};
    });
    http::async_client client(workers);
    server.configure(client).timeout(5s).max_idle_connections_per_host(4);

    const int concurrent = 16;
    std::atomic<int> ok{0};
    std::atomic<int> done{0};
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < concurrent; ++i) {
            // From the threads of the workers: requests, and their connections, on all of them
            boost::asio::post(workers.next_executor(), [&] {
                client.get(server.url(), [&](http::client_response& res) {
                    if (res.ok()) ok++;
                    done++;
                });
            });
        }
        REQUIRE(keepalive_server::eventually([&] { return done == (round + 1) * concurrent; }, 10s));
        client.wait();
        auto stats = client.pool_stats();
        REQUIRE(stats.idle <= 4);
        REQUIRE(stats.active == 0);
    }
    REQUIRE(ok == 3 * concurrent);
    REQUIRE(server.overlapped == 0);
    REQUIRE(server.received_requests().size() == 3 * concurrent);
    // The idle connections were reused by the next rounds
    REQUIRE(client.pool_stats().reused >= 8);
}

TEST_CASE("HTTP client with the pool disabled opens a connection per request", "[http][client][pool][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    keepalive_server server(kind);
    http::client client;
    server.configure(client).timeout(5s).max_idle_connections_per_host(0);
    for (int i = 0; i < 5; ++i) REQUIRE(client.get(server.url()).ok());
    REQUIRE(server.accepts == 5);
    REQUIRE(client.pool_size() == 0);
    REQUIRE(keepalive_server::eventually([&] { return server.closed_by_client == 5; }));
}

TEST_CASE("HTTP client closes its idle connections when destroyed", "[http][client][pool][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    keepalive_server server(kind);

    SECTION("Synchronous client") {
        {
            http::client client;
            server.configure(client).timeout(5s);
            REQUIRE(client.get(server.url()).ok());
            REQUIRE(client.pool_size() == 1);
        }
        REQUIRE(keepalive_server::eventually([&] { return server.closed_by_client == 1; }));
    }

    SECTION("Asynchronous client") {
        running_workers workers;
        {
            http::async_client client(workers);
            server.configure(client).timeout(5s);
            bool ok = false;
            client.get(server.url(), [&](http::client_response& res) { ok = res.ok(); });
            client.wait();
            REQUIRE(ok);
            REQUIRE(client.pool_size() == 1);
        }
        REQUIRE(keepalive_server::eventually([&] { return server.closed_by_client == 1; }));
    }

    SECTION("Asynchronous client, workers stopped first") {
        // The idle connections stay on the stopped workers (alive until destroyed), and close
        // with the client
        running_workers workers;
        {
            http::async_client client(workers);
            server.configure(client).timeout(5s);
            bool ok = false;
            client.get(server.url(), [&](http::client_response& res) { ok = res.ok(); });
            client.wait();
            REQUIRE(ok);
            workers.stop();
            REQUIRE(client.pool_size() == 1);
        }
        REQUIRE(keepalive_server::eventually([&] { return server.closed_by_client == 1; }));
    }
}

TEST_CASE("HTTP client keeps direct and proxied connections apart", "[http][client][pool][proxy][integration]") {
    // (Not in the matrix: the client reaches a forwarding proxy over TCP, and the requests it
    // sends through it name hosts that only TCP has.)
    // The server answers requests in absolute form too: it serves as the forwarding proxy.
    // Loopback addresses never use the proxy: the proxied requests name origin.test
    keepalive_server origin;
    keepalive_server proxy;
    http::client client;
    client.timeout(5s).proxy(proxy.url(""));
    auto proxied_url = origin.url("/", "origin.test");

    for (int i = 0; i < 2; ++i) {
        REQUIRE(client.get(origin.url()).ok());
        REQUIRE(client.get(proxied_url).ok());
    }
    REQUIRE(origin.accepts == 1);
    REQUIRE(origin.received_requests().size() == 2);
    REQUIRE(proxy.accepts == 1);
    REQUIRE(proxy.received_requests().size() == 2);
    REQUIRE(proxy.received_requests()[0].request_line == "GET " + proxied_url + " HTTP/1.1");

    // Through a forwarding proxy, a connection reaches any origin
    REQUIRE(client.get(origin.url("/", "other.test")).ok());
    REQUIRE(proxy.accepts == 1);
    REQUIRE(proxy.received_requests().back().request_line == "GET " + origin.url("/", "other.test") + " HTTP/1.1");
    auto stats = client.pool_stats();
    REQUIRE(stats.hosts.size() == 2);
    REQUIRE(stats.hosts.contains("proxy " + proxy.url("")));

    SECTION("Not after the proxy closed it") {
        proxy.close_idle(50ms);
        REQUIRE(client.get(proxied_url).ok());
        std::this_thread::sleep_for(200ms);
        REQUIRE(client.get(proxied_url).ok());
        REQUIRE(proxy.accepts == 2);
    }
}

TEST_CASE("HTTP client keep-alive benchmark", "[.][benchmark][http][client][pool]") {
    // Sequential GETs to a local server, with the pool and without it (a new connection, and
    // TLS handshake, per request: as before the pool)
    for (bool tls : {false, true}) {
        port_server server(tls);
        for (bool pool : {false, true}) {
            http::client client;
            client.timeout(5s).verify_ssl(false);
            if (!pool) client.max_idle_connections_per_host(0);
            const int requests = tls ? 2000 : 10000;
            std::set<std::string> ports;
            auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < requests; ++i) {
                auto res = client.get(server.url("127.0.0.1"));
                REQUIRE(res.ok());
                ports.insert(res.body());
            }
            std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
            std::cout << (tls ? "HTTPS" : "HTTP ") << (pool ? " pool    " : " no pool ") << requests << " requests: "
                      << static_cast<long>(requests / elapsed.count()) << " req/s, " << ports.size() << " connections"
                      << std::endl;
        }
    }
}

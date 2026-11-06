#include <catch2/catch_test_macros.hpp>

#include <thinger/asio/listener.hpp>
#include <thinger/asio/detail/listener_access.hpp>
#include <thinger/asio/sockets/tcp_socket.hpp>
#include <thinger/asio/ssl/certificate_manager.hpp>

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../fixtures/listener_helpers.hpp"

using namespace thinger::asio;
using namespace std::chrono_literals;
using thinger::http::test::client_hello;
using thinger::http::test::delivered;
using thinger::http::test::io_thread;
using thinger::http::test::log_capture;
using thinger::http::test::secure_options;
using thinger::http::test::stalled_client;
using thinger::http::test::tcp_options;
using thinger::http::test::tls_handshake;
using tcp = boost::asio::ip::tcp;
using local = boost::asio::local::stream_protocol;

// The listener: TCP, TLS and Unix domain sockets, accepting with executor providers or on a
// thread of its own (the mode of the servers on the workers), stop() against accepting,
// handshaking and delivering, and the deadline of TLS handshakes

namespace {

enum class accept_mode { provider, acceptor_thread };

std::unique_ptr<listener> make_listener(listener_options options, io_thread& io, accept_mode mode) {
    auto result = std::make_unique<listener>(std::move(options), io.provider(), io.provider());
    if (mode == accept_mode::acceptor_thread) detail::listener_access::use_acceptor_thread(*result);
    return result;
}

std::string unix_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() /
            ("thinger_listener_" + name + "_" + std::to_string(getpid()) + ".sock")).string();
}

listener_options unix_options(const std::string& path) {
    listener_options options;
    options.endpoint = unix_endpoint{path};
    options.max_listening_attempts = 1;
    return options;
}

tcp::endpoint loopback(uint16_t port) {
    return {boost::asio::ip::make_address("127.0.0.1"), port};
}

template<typename Protocol>
bool can_connect(const typename Protocol::endpoint& endpoint) {
    boost::asio::io_context io_context;
    typename Protocol::socket sock(io_context);
    boost::system::error_code ec;
    sock.connect(endpoint, ec);
    return !ec;
}

// Whether the server closes a connection to `endpoint` within `timeout` (EOF or reset)
template<typename Protocol>
bool closed_by_server(const typename Protocol::endpoint& endpoint, std::chrono::milliseconds timeout) {
    boost::asio::io_context io_context;
    typename Protocol::socket sock(io_context);
    sock.connect(endpoint);
    char byte;
    bool closed = false;
    sock.async_read_some(boost::asio::buffer(&byte, 1), [&](const boost::system::error_code& ec, size_t) {
        closed = static_cast<bool>(ec);
    });
    io_context.run_for(timeout);
    return closed;
}

const char* name(accept_mode mode) {
    return mode == accept_mode::provider ? "executor providers" : "acceptor thread";
}

} // namespace

TEST_CASE("listener accepts TCP connections", "[listener][integration]") {
    for (auto mode : {accept_mode::provider, accept_mode::acceptor_thread}) {
        DYNAMIC_SECTION(name(mode)) {
            io_thread io;
            auto server = make_listener(tcp_options(), io, mode);
            REQUIRE(server->local_port() == 0);
            REQUIRE_FALSE(server->open());
            auto port = server->local_port();
            REQUIRE(port != 0);
            REQUIRE(server->url() == "http://127.0.0.1:" + std::to_string(port));

            delivered sockets;
            server->start(sockets.handler());
            boost::asio::io_context client_context;
            tcp::socket client(client_context);
            client.connect(loopback(port));
            REQUIRE(sockets.wait(1));
            auto sock = sockets.sockets.front();
            REQUIRE_FALSE(sock->is_secure());
            REQUIRE(sock->get_remote_ip() == "127.0.0.1");

            // TCP_NODELAY is set by default
            int nodelay = 0;
            socklen_t size = sizeof(nodelay);
            auto* tcp_sock = dynamic_cast<tcp_socket*>(sock.get());
            REQUIRE(tcp_sock != nullptr);
            REQUIRE(::getsockopt(tcp_sock->get_socket().native_handle(), IPPROTO_TCP, TCP_NODELAY, &nodelay, &size) == 0);
            REQUIRE(nodelay != 0);

            server->stop();
            REQUIRE_NOTHROW(server->local_port());
            REQUIRE(server->local_port() == 0);
            REQUIRE_FALSE(can_connect<tcp>(loopback(port)));
            server->stop();   // harmless
        }
    }
}

TEST_CASE("listener accepts TLS connections once their handshake is done", "[listener][tls][integration]") {
    for (auto mode : {accept_mode::provider, accept_mode::acceptor_thread}) {
        DYNAMIC_SECTION(name(mode)) {
            io_thread io;
            auto server = make_listener(secure_options(), io, mode);
            REQUIRE_FALSE(server->open());
            auto port = server->local_port();
            REQUIRE(port != 0);
            REQUIRE(server->url() == "https://127.0.0.1:" + std::to_string(port));

            delivered sockets;
            server->start(sockets.handler());
            // A connection that does not handshake is not handed over
            stalled_client silent(port);
            REQUIRE(silent.connected());
            REQUIRE(tls_handshake(port, 5s, "listener.example.com"));
            REQUIRE(sockets.wait(1));
            std::this_thread::sleep_for(100ms);
            REQUIRE(sockets.size() == 1);

            auto sock = sockets.sockets.front();
            REQUIRE(sock->is_secure());
            auto session = sock->tls();
            REQUIRE(session);
            REQUIRE(session->version.starts_with("TLSv1."));
            REQUIRE_FALSE(session->cipher.empty());
            REQUIRE(session->server_name == "listener.example.com");

            int nodelay = 0;
            socklen_t size = sizeof(nodelay);
            auto* tcp_sock = dynamic_cast<tcp_socket*>(sock.get());
            REQUIRE(tcp_sock != nullptr);
            REQUIRE(::getsockopt(tcp_sock->get_socket().native_handle(), IPPROTO_TCP, TCP_NODELAY, &nodelay, &size) == 0);
            REQUIRE(nodelay != 0);

            server->stop();
            REQUIRE(server->local_port() == 0);
            REQUIRE_FALSE(can_connect<tcp>(loopback(port)));
        }
    }
}

TEST_CASE("listener keeps serving after a connection fails to be set up", "[listener][integration]") {
    // The handler throws for the first connection: only that one is dropped (its socket closed
    // while unwinding), never escaping the thread serving it, which would end the process
    auto keeps_serving = []<typename Protocol>(listener& server, const std::function<typename Protocol::endpoint()>& endpoint) {
        std::atomic<int> calls{0};
        std::atomic<int> served{0};
        REQUIRE_FALSE(server.open());
        server.start([&](std::shared_ptr<thinger::asio::socket> sock) {
            if (++calls == 1) throw std::runtime_error("cannot set the connection up");
            ++served;
            sock->close();
        });
        REQUIRE(closed_by_server<Protocol>(endpoint(), 5s));
        REQUIRE(calls == 1);
        REQUIRE(closed_by_server<Protocol>(endpoint(), 5s));
        REQUIRE(calls == 2);
        REQUIRE(served == 1);
        server.stop();
    };

    for (auto mode : {accept_mode::provider, accept_mode::acceptor_thread}) {
        DYNAMIC_SECTION("TCP, " << name(mode)) {
            io_thread io;
            auto server = make_listener(tcp_options(), io, mode);
            keeps_serving.operator()<tcp>(*server, [&] { return loopback(server->local_port()); });
        }
        DYNAMIC_SECTION("Unix, " << name(mode)) {
            auto path = unix_path("setup");
            io_thread io;
            auto server = make_listener(unix_options(path), io, mode);
            keeps_serving.operator()<local>(*server, [&] { return local::endpoint(path); });
        }
    }
}

TEST_CASE("listener without tcp_no_delay leaves Nagle on", "[listener][integration]") {
    io_thread io;
    auto options = tcp_options();
    options.tcp_no_delay = false;
    auto server = make_listener(options, io, accept_mode::provider);
    REQUIRE_FALSE(server->open());
    delivered sockets;
    server->start(sockets.handler());
    boost::asio::io_context client_context;
    tcp::socket client(client_context);
    client.connect(loopback(server->local_port()));
    REQUIRE(sockets.wait(1));
    int nodelay = 1;
    socklen_t size = sizeof(nodelay);
    auto* tcp_sock = dynamic_cast<tcp_socket*>(sockets.sockets.front().get());
    REQUIRE(::getsockopt(tcp_sock->get_socket().native_handle(), IPPROTO_TCP, TCP_NODELAY, &nodelay, &size) == 0);
    REQUIRE(nodelay == 0);
}

TEST_CASE("listener accepts sockets closed on exec", "[listener][integration]") {
    // A program a handler runs (fork and exec) does not inherit the connection
    for (auto mode : {accept_mode::provider, accept_mode::acceptor_thread}) {
        DYNAMIC_SECTION(name(mode)) {
            io_thread io;
            auto server = make_listener(tcp_options(), io, mode);
            REQUIRE_FALSE(server->open());
            delivered sockets;
            server->start(sockets.handler());
            boost::asio::io_context client_context;
            tcp::socket client(client_context);
            client.connect(loopback(server->local_port()));
            REQUIRE(sockets.wait(1));
            auto* tcp_sock = dynamic_cast<tcp_socket*>(sockets.sockets.front().get());
            REQUIRE(tcp_sock != nullptr);
            int flags = ::fcntl(tcp_sock->get_socket().native_handle(), F_GETFD);
            REQUIRE(flags >= 0);
            REQUIRE((flags & FD_CLOEXEC) != 0);
            server->stop();
        }
    }
}

TEST_CASE("listener calls its executor providers without holding its lock", "[listener][integration]") {
    // A provider calling the listener (here url()) would deadlock if it ran under its mutex,
    // as start() and every accept call it. Within 10 s: start() and two connections served
    io_thread io;
    std::atomic<listener*> self{nullptr};
    std::atomic<int> urls_read{0};
    executor_provider serve_on = [&] {
        if (auto* server = self.load(); server && !server->url().empty()) ++urls_read;
        return boost::asio::any_io_executor(io.io_context.get_executor());
    };
    listener server(tcp_options(), io.provider(), serve_on);
    REQUIRE_FALSE(server.open());
    self = &server;
    auto port = server.local_port();

    delivered sockets;
    auto served = std::async(std::launch::async, [&] {
        server.start(sockets.handler());
        boost::asio::io_context client_context;
        tcp::socket first(client_context);
        tcp::socket second(client_context);
        first.connect(loopback(port));
        second.connect(loopback(port));
        return sockets.wait(2, 5s);
    });
    REQUIRE(served.wait_for(10s) == std::future_status::ready);
    REQUIRE(served.get());
    // start(), then once per connection accepted
    REQUIRE(urls_read >= 3);
    server.stop();
    self = nullptr;
}

TEST_CASE("listener accepts Unix domain socket connections", "[listener][unix][integration]") {
    for (auto mode : {accept_mode::provider, accept_mode::acceptor_thread}) {
        DYNAMIC_SECTION(name(mode)) {
            auto path = unix_path("accept");
            io_thread io;
            auto server = make_listener(unix_options(path), io, mode);
            REQUIRE_FALSE(server->open());
            REQUIRE(server->local_port() == 0);
            REQUIRE(server->url() == "unix:" + path);
            REQUIRE(std::filesystem::exists(path));

            delivered sockets;
            server->start(sockets.handler());
            boost::asio::io_context client_context;
            local::socket client(client_context);
            client.connect(local::endpoint(path));
            REQUIRE(sockets.wait(1));
            REQUIRE_FALSE(sockets.sockets.front()->is_secure());

            server->stop();
            // The socket file goes with the listener
            REQUIRE_FALSE(std::filesystem::exists(path));
        }
    }
}

TEST_CASE("listener stops whatever happened to its Unix socket file", "[listener][unix][stop][integration]") {
    auto path = unix_path("file_gone");
    io_thread io;
    auto server = make_listener(unix_options(path), io, accept_mode::provider);
    REQUIRE_FALSE(server->open());
    // Never accepted: it sees the acceptor closing
    boost::asio::io_context client_context;
    local::socket client(client_context);
    client.connect(local::endpoint(path));
    REQUIRE(std::filesystem::remove(path));
    log_capture logs;

    SECTION("Another removed it") {
        server->stop();
        REQUIRE(logs.count("warning", "cannot remove Unix socket file") == 0);
    }

    SECTION("It cannot be removed") {
        // (a directory with something in it: not even root removes it)
        std::filesystem::create_directories(std::filesystem::path(path) / "child");
        server->stop();
        std::filesystem::remove_all(path);
        REQUIRE(logs.count("warning", "cannot remove Unix socket file") == 1);
    }

    pollfd closing{client.native_handle(), POLLIN, 0};
    REQUIRE(::poll(&closing, 1, 2000) == 1);
}

TEST_CASE("listener replaces a stale Unix socket file", "[listener][unix][integration]") {
    auto path = unix_path("stale");
    {
        boost::asio::io_context io_context;
        local::acceptor stale(io_context, local::endpoint(path));
    }
    REQUIRE(std::filesystem::exists(path));
    io_thread io;
    auto server = make_listener(unix_options(path), io, accept_mode::provider);
    REQUIRE_FALSE(server->open());
    REQUIRE(can_connect<local>(local::endpoint(path)));
}

TEST_CASE("listener open() reports a busy port at once with one attempt", "[listener][integration]") {
    boost::asio::io_context io_context;
    tcp::acceptor busy(io_context, loopback(0));
    auto port = busy.local_endpoint().port();

    io_thread io;
    auto server = make_listener(tcp_options(port), io, accept_mode::provider);
    auto started = std::chrono::steady_clock::now();
    auto ec = server->open();
    REQUIRE(ec);
    REQUIRE(std::chrono::steady_clock::now() - started < 2s);
    REQUIRE(server->local_port() == 0);
}

TEST_CASE("listener stop() ends an open() retrying to bind", "[listener][integration]") {
    boost::asio::io_context io_context;
    tcp::acceptor busy(io_context, loopback(0));
    io_thread io;
    auto options = tcp_options(busy.local_endpoint().port());
    options.max_listening_attempts = -1;
    listener server(options, io.provider(), io.provider());
    auto opening = std::async(std::launch::async, [&] { return server.open(); });
    std::this_thread::sleep_for(200ms);
    server.stop();
    REQUIRE(opening.wait_for(2s) == std::future_status::ready);
    REQUIRE(opening.get() == boost::asio::error::operation_aborted);
}

TEST_CASE("listener is used once", "[listener][integration]") {
    io_thread io;
    delivered sockets;

    SECTION("start() before open() does nothing") {
        auto server = make_listener(tcp_options(), io, accept_mode::provider);
        server->start(sockets.handler());
        REQUIRE_FALSE(server->open());
        server->start(sockets.handler());
        boost::asio::io_context client_context;
        tcp::socket client(client_context);
        client.connect(loopback(server->local_port()));
        REQUIRE(sockets.wait(1));
    }
    SECTION("open() twice") {
        auto server = make_listener(tcp_options(), io, accept_mode::provider);
        REQUIRE_FALSE(server->open());
        auto port = server->local_port();
        REQUIRE(server->open() == boost::asio::error::already_open);
        REQUIRE(server->local_port() == port);
    }
    SECTION("stop() before open(), then open()") {
        auto server = make_listener(tcp_options(), io, accept_mode::provider);
        server->stop();
        server->stop();
        REQUIRE(server->open() == boost::asio::error::already_open);
        server->start(sockets.handler());
    }
}

TEST_CASE("listener refuses executors not on an io_context", "[listener][integration]") {
    boost::asio::thread_pool pool(1);
    io_thread io;
    listener server(tcp_options(), io.provider(), [&] { return boost::asio::any_io_executor(pool.get_executor()); });
    REQUIRE(server.open() == boost::asio::error::invalid_argument);
    listener no_executor(tcp_options(), {}, io.provider());
    REQUIRE(no_executor.open() == boost::asio::error::invalid_argument);
}

TEST_CASE("listener reports an unresolvable host", "[listener][integration]") {
    io_thread io;
    auto options = tcp_options();
    options.endpoint = tcp_endpoint{"no-such-host.invalid", 0};
    listener server(options, io.provider(), io.provider());
    REQUIRE(server.open());
}

TEST_CASE("listener filters remote addresses", "[listener][integration]") {
    for (auto mode : {accept_mode::provider, accept_mode::acceptor_thread}) {
        DYNAMIC_SECTION(name(mode)) {
            io_thread io;
            delivered sockets;

            SECTION("forbidden") {
                auto options = tcp_options();
                options.forbidden_remotes = {"127.0.0.1"};
                auto server = make_listener(options, io, mode);
                REQUIRE_FALSE(server->open());
                server->start(sockets.handler());
                REQUIRE(closed_by_server<tcp>(loopback(server->local_port()), 2s));
                REQUIRE(sockets.size() == 0);
            }
            SECTION("not in the allowed list") {
                auto options = tcp_options();
                options.allowed_remotes = {"10.0.0.1"};
                auto server = make_listener(options, io, mode);
                REQUIRE_FALSE(server->open());
                server->start(sockets.handler());
                REQUIRE(closed_by_server<tcp>(loopback(server->local_port()), 2s));
                REQUIRE(sockets.size() == 0);
            }
            SECTION("allowed") {
                auto options = tcp_options();
                options.allowed_remotes = {"127.0.0.1"};
                auto server = make_listener(options, io, mode);
                REQUIRE_FALSE(server->open());
                server->start(sockets.handler());
                boost::asio::io_context client_context;
                tcp::socket client(client_context);
                client.connect(loopback(server->local_port()));
                REQUIRE(sockets.wait(1));
            }
        }
    }
}

TEST_CASE("listener stopped from its handler", "[listener][stop][integration]") {
    for (bool secure : {false, true}) {
        for (auto mode : {accept_mode::provider, accept_mode::acceptor_thread}) {
            DYNAMIC_SECTION((secure ? "TLS, " : "plain, ") << name(mode)) {
                io_thread io;
                auto server = make_listener(secure ? secure_options() : tcp_options(), io, mode);
                REQUIRE_FALSE(server->open());
                auto port = server->local_port();
                std::promise<void> stopped;
                std::atomic<int> calls{0};
                server->start([&](std::shared_ptr<thinger::asio::socket>) {
                    if (++calls > 1) return;
                    server->stop();
                    server->stop();
                    stopped.set_value();
                });
                // Two clients: the second one is never handed over
                auto connect = [&] {
                    if (secure) return std::async(std::launch::async, [port] { return tls_handshake(port, 2s); });
                    return std::async(std::launch::async, [port] { return can_connect<tcp>(loopback(port)); });
                };
                auto first = connect();
                REQUIRE(stopped.get_future().wait_for(5s) == std::future_status::ready);
                REQUIRE(first.get());
                auto second = connect();
                second.wait();
                REQUIRE(server->local_port() == 0);
                REQUIRE_FALSE(can_connect<tcp>(loopback(port)));
                server->stop();
                REQUIRE(calls == 1);
            }
        }
    }
}

TEST_CASE("listener destroyed from its handler", "[listener][stop][integration]") {
    io_thread io;
    auto server = make_listener(tcp_options(), io, accept_mode::provider);
    REQUIRE_FALSE(server->open());
    auto port = server->local_port();
    std::promise<void> destroyed;
    server->start([&](std::shared_ptr<thinger::asio::socket>) {
        server.reset();
        destroyed.set_value();
    });
    boost::asio::io_context client_context;
    tcp::socket client(client_context);
    client.connect(loopback(port));
    REQUIRE(destroyed.get_future().wait_for(5s) == std::future_status::ready);
    REQUIRE_FALSE(can_connect<tcp>(loopback(port)));
}

TEST_CASE("listener stop() waits for a handler running on another thread", "[listener][stop][integration]") {
    for (auto mode : {accept_mode::provider, accept_mode::acceptor_thread}) {
        DYNAMIC_SECTION(name(mode)) {
            io_thread io;
            auto server = make_listener(tcp_options(), io, mode);
            REQUIRE_FALSE(server->open());
            std::promise<void> entered;
            std::promise<void> release;
            auto released = release.get_future().share();
            std::atomic<bool> handler_done{false};
            server->start([&, released](std::shared_ptr<thinger::asio::socket>) {
                entered.set_value();
                released.wait();
                handler_done = true;
            });
            boost::asio::io_context client_context;
            tcp::socket client(client_context);
            client.connect(loopback(server->local_port()));
            REQUIRE(entered.get_future().wait_for(5s) == std::future_status::ready);

            auto stopping = std::async(std::launch::async, [&] { server->stop(); });
            REQUIRE(stopping.wait_for(200ms) == std::future_status::timeout);
            release.set_value();
            REQUIRE(stopping.wait_for(5s) == std::future_status::ready);
            REQUIRE(handler_done);
        }
    }
}

TEST_CASE("listener handlers on two threads stop it at once", "[listener][stop][integration]") {
    io_thread first;
    io_thread second;
    std::atomic<int> next{0};
    executor_provider serve_on = [&]() -> boost::asio::any_io_executor {
        return (next++ % 2 == 0) ? first.io_context.get_executor() : second.io_context.get_executor();
    };
    listener server(tcp_options(), first.provider(), serve_on);
    detail::listener_access::use_acceptor_thread(server);
    REQUIRE_FALSE(server.open());
    std::atomic<int> arrived{0};
    std::promise<void> both_stopped;
    std::atomic<int> stopped{0};
    server.start([&](std::shared_ptr<thinger::asio::socket>) {
        // Both handlers run before either stops the listener
        ++arrived;
        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (arrived < 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
        server.stop();
        if (++stopped == 2) both_stopped.set_value();
    });
    boost::asio::io_context client_context;
    tcp::socket a(client_context);
    tcp::socket b(client_context);
    a.connect(loopback(server.local_port()));
    b.connect(loopback(server.local_port()));
    REQUIRE(both_stopped.get_future().wait_for(10s) == std::future_status::ready);
}

namespace {

// Connects to an endpoint over and over, each attempt bounded, until destroyed
template<typename Protocol>
struct connecting_thread {
    std::atomic<bool> done{false};
    std::thread thread;

    explicit connecting_thread(typename Protocol::endpoint endpoint)
        : thread([this, endpoint] {
            boost::asio::io_context io_context;
            while (!done) {
                typename Protocol::socket sock(io_context);
                sock.async_connect(endpoint, [](const boost::system::error_code&) {});
                io_context.run_for(50ms);
                io_context.restart();
            }
        }) {}

    ~connecting_thread() {
        done = true;
        thread.join();
    }
};

// A new listener each round, stopped from another thread while accepting: once stop() returns
// no connection reaches the handler, and nothing listens
template<typename Protocol>
void stop_while_connecting(const std::function<listener_options()>& options, accept_mode mode,
                           const std::function<typename Protocol::endpoint(const listener&)>& endpoint) {
    std::atomic<int> served{0};
    std::atomic<bool> stopped{false};
    std::atomic<int> served_after_stop{0};
    io_thread io;

    for (int round = 0; round < 50; ++round) {
        stopped = false;
        auto server = make_listener(options(), io, mode);
        REQUIRE_FALSE(server->open());
        server->start([&](std::shared_ptr<thinger::asio::socket> sock) {
            if (stopped) ++served_after_stop;
            ++served;
            sock->close();
        });
        auto listening = endpoint(*server);
        int before = served;
        {
            connecting_thread<Protocol> connecting(listening);
            auto deadline = std::chrono::steady_clock::now() + 5s;
            while (served < before + 3 && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(100us);
            }
            REQUIRE(served >= before + 3);
            server->stop();
            stopped = true;
        }
        if constexpr (std::is_same_v<Protocol, local>) {
            REQUIRE_FALSE(can_connect<Protocol>(listening));
        }
    }
    REQUIRE(served_after_stop == 0);
}

} // namespace

TEST_CASE("TCP listener stops from another thread while accepting", "[listener][stop][integration]") {
    for (auto mode : {accept_mode::provider, accept_mode::acceptor_thread}) {
        DYNAMIC_SECTION(name(mode)) {
            stop_while_connecting<tcp>([] { return tcp_options(); }, mode,
                                       [](const listener& server) { return loopback(server.local_port()); });
        }
    }
}

TEST_CASE("Unix listener stops from another thread while accepting", "[listener][unix][stop][integration]") {
    auto path = unix_path("stop");
    for (auto mode : {accept_mode::provider, accept_mode::acceptor_thread}) {
        DYNAMIC_SECTION(name(mode)) {
            stop_while_connecting<local>([&] { return unix_options(path); }, mode,
                                         [&](const listener&) { return local::endpoint(path); });
        }
    }
}

// A client that left before its connection was accepted does not stop the listener accepting. On
// macOS accepting it fails: asio sets SO_NOSIGPIPE on its socket, EINVAL by then
TEST_CASE("A client gone before its accept holds up nobody", "[listener][unix][integration]") {
    for (auto mode : {accept_mode::provider, accept_mode::acceptor_thread}) {
        DYNAMIC_SECTION(name(mode)) {
            log_capture logs;
            auto path = unix_path("gone");
            io_thread io;
            auto server = make_listener(unix_options(path), io, mode);
            REQUIRE_FALSE(server->open());

            // In the backlog when the listener starts accepting
            boost::asio::io_context clients;
            local::socket gone(clients);
            gone.connect(local::endpoint(path));
            gone.close();

            server->start([](std::shared_ptr<thinger::asio::socket> sock) { sock->close(); });

            // The next client is served: the handler closes its connection
            local::socket next(clients);
            next.connect(local::endpoint(path));
            pollfd closed{next.native_handle(), POLLIN, 0};
            CHECK(::poll(&closed, 1, 5000) == 1);
            CHECK(logs.count("error", "cannot accept more connections") == 0);
            server->stop();
        }
    }
}

TEST_CASE("listener stop() against a TLS handshake ending", "[listener][stop][tls][integration]") {
    // Whichever wins, the connection is handed over before stop() returns, or closed: never
    // after it. Stopped at a different point of the handshake each round, from its start to
    // twice the time a whole one takes
    auto certificates = std::make_shared<certificate_manager>();
    for (auto mode : {accept_mode::provider, accept_mode::acceptor_thread}) {
        DYNAMIC_SECTION(name(mode)) {
            io_thread io;
            std::atomic<bool> stopped{false};
            std::atomic<int> handed{0};
            std::atomic<int> after_stop{0};
            auto handshake_round = [&](const std::function<void()>& meanwhile) {
                auto server = make_listener(secure_options(5s, certificates), io, mode);
                REQUIRE_FALSE(server->open());
                server->start([&](std::shared_ptr<thinger::asio::socket> sock) {
                    if (stopped) ++after_stop;
                    ++handed;
                    sock->close();
                });
                auto port = server->local_port();
                auto client = std::async(std::launch::async, [port] { return tls_handshake(port, 2s); });
                meanwhile();
                server->stop();
                stopped = true;
                client.wait();
            };

            // The longest of a few rounds from the client starting to the handler
            std::chrono::steady_clock::duration whole{};
            for (int round = 0; round < 3; ++round) {
                handshake_round([&] {
                    auto started = std::chrono::steady_clock::now();
                    int before = handed;
                    auto deadline = started + 5s;
                    while (handed == before && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
                    whole = std::max(whole, std::chrono::steady_clock::now() - started);
                });
                stopped = false;
            }
            REQUIRE(handed == 3);

            handed = 0;
            for (int round = 0; round < 200; ++round) {
                handshake_round([&] { std::this_thread::sleep_for(whole * (round % 20) / 10); });
                stopped = false;
            }
            INFO(handed << " of 200 handed over, a handshake took "
                 << std::chrono::duration_cast<std::chrono::microseconds>(whole).count() << " us");
            REQUIRE(after_stop == 0);
            // Both ends of the race were reached
            REQUIRE(handed > 0);
            REQUIRE(handed < 200);
        }
    }
}

TEST_CASE("listener closes a TLS client that does not handshake in time", "[listener][handshake-timeout][integration]") {
    for (auto mode : {accept_mode::provider, accept_mode::acceptor_thread}) {
        for (const auto& sent : {std::string(), client_hello()}) {
            DYNAMIC_SECTION(name(mode) << ", sending " << sent.size() << " bytes") {
                log_capture logs;
                io_thread io;
                auto server = make_listener(secure_options(200ms), io, mode);
                REQUIRE_FALSE(server->open());
                delivered sockets;
                server->start(sockets.handler());

                stalled_client client(server->local_port(), sent);
                REQUIRE(client.connected());
                REQUIRE(client.closed_within(3s));
                // Counted from the accept, a little after the client connected
                auto elapsed = std::chrono::steady_clock::now() - client.connected_at;
                INFO("closed after " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms");
                REQUIRE(elapsed >= 180ms);
                REQUIRE(elapsed <= 600ms);
                REQUIRE(sockets.size() == 0);

                // Logged at debug level: any client may cause it
                REQUIRE(logs.logged("debug", "TLS handshake not done in time"));
                server->stop();
                REQUIRE(logs.count("debug", "TLS handshake not done in time") == 1);
                REQUIRE(logs.count("warning", "handshake") == 0);
                REQUIRE(logs.count("error", "") == 0);
            }
        }
    }

    SECTION("A client that completed its handshake is not closed by the deadline") {
        io_thread io;
        auto server = make_listener(secure_options(200ms), io, accept_mode::provider);
        REQUIRE_FALSE(server->open());
        delivered sockets;
        server->start(sockets.handler());
        REQUIRE(tls_handshake(server->local_port()));
        REQUIRE(sockets.wait(1));
        std::this_thread::sleep_for(500ms);
        REQUIRE(sockets.sockets.front()->is_open());
    }
}

TEST_CASE("listener without a handshake deadline keeps a silent TLS client", "[listener][handshake-timeout][integration]") {
    io_thread io;
    auto server = make_listener(secure_options(0ms), io, accept_mode::provider);
    REQUIRE_FALSE(server->open());
    delivered sockets;
    server->start(sockets.handler());
    stalled_client silent(server->local_port());
    stalled_client hello(server->local_port(), client_hello());
    // Longer than the deadline of the tests above, and than the 500 ms of the former ones
    REQUIRE_FALSE(silent.closed_within(1500ms));
    REQUIRE_FALSE(hello.closed_within(100ms));
    REQUIRE(sockets.size() == 0);
    // Until the listener stops
    server->stop();
    REQUIRE(silent.closed_within(2s));
    REQUIRE(hello.closed_within(2s));
}

TEST_CASE("listener stop() closes the TLS handshakes pending", "[listener][handshake-timeout][stop][integration]") {
    for (auto mode : {accept_mode::provider, accept_mode::acceptor_thread}) {
        DYNAMIC_SECTION(name(mode)) {
            io_thread io;
            auto server = make_listener(secure_options(), io, mode);
            REQUIRE_FALSE(server->open());
            auto port = server->local_port();
            delivered sockets;
            server->start(sockets.handler());
            stalled_client silent(port);
            stalled_client hello(port, client_hello());
            // Both accepted, their handshakes pending
            REQUIRE(hello.wait(2s) == stalled_client::outcome::data);
            REQUIRE_FALSE(silent.closed_within(100ms));

            server->stop();
            // Closed on the thread handshaking, as stop() returns
            REQUIRE(silent.closed_within(1s));
            REQUIRE(hello.closed_within(1s));
            REQUIRE(sockets.size() == 0);
        }
    }
}

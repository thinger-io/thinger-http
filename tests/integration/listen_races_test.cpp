#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/pool_server.hpp>
#include <thinger/http/server/response.hpp>

#include "server_test_helpers.hpp"
#include "shutdown_test_helpers.hpp"
#include "../fixtures/running_workers.hpp"
#include "../fixtures/server_listen.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>
#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif

// listen() racing the calls that stop the listeners: stop_listening(), stop() and shutdown(),
// called on other threads. No listener outlives them unseen: once both calls returned, a client
// connects only to a listener listening_on() lists, and none after a shutdown. A listen() fails
// while a shutdown is in progress, and works again once it ended. Two listen() on the same
// endpoint open one listener at most.
//
// And the shutdown racing what stops under it: the workers of a pool server stopping
// (workers.stop()) never make it throw nor hang, and a subclass being destroyed as the shutdown
// ends on a worker never has its stop() override called

using namespace thinger;
using namespace std::chrono_literals;
using http::test::listen_options;
using http::test::temporary_directory;
using http::test::unix_listen_options;
using server_test::transport;
using server_test::transport_endpoint;
using shutdown_test::can_connect;
using shutdown_test::raw_client;

namespace {

    using shutdown_test::clock;   // here, as ::clock is the C function

    // Iterations of each race: their overlap shifts a little in each (see race())
    constexpr int race_iterations = 200;
    constexpr int same_endpoint_iterations = 100;

    // Run `first` and `second` on two threads released at once, `second` delayed by a few
    // microseconds that change with `iteration` (on either side), so that the races cover the
    // whole of the shorter call; and wait for both
    template<typename First, typename Second>
    void race(int iteration, First&& first, Second&& second) {
        std::atomic<int> ready{0};
        auto delay = std::chrono::microseconds((iteration % 20) * 10);
        auto start = [&ready](std::chrono::microseconds wait) {
            ready.fetch_add(1);
            while (ready.load() < 2) std::this_thread::yield();
            auto until = clock::now() + wait;
            while (clock::now() < until) {}
        };
        std::thread other([&] {
            start(iteration % 2 ? delay : 0us);
            second();
        });
        start(iteration % 2 ? 0us : delay);
        first();
        other.join();
    }

    // The first and last ports of the range the system picks from, for a listener on port 0
    // and for the local end of a connection
    std::pair<int, int> ephemeral_range() {
#if defined(__APPLE__)
        int first = 49152;
        int last = 65535;
        size_t size = sizeof(int);
        sysctlbyname("net.inet.ip.portrange.first", &first, &size, nullptr, 0);
        size = sizeof(int);
        sysctlbyname("net.inet.ip.portrange.last", &last, &size, nullptr, 0);
        return {first, last};
#else
        int first = 32768;
        int last = 60999;
        std::ifstream range("/proc/sys/net/ipv4/ip_local_port_range");
        range >> first >> last;
        return {first, last};
#endif
    }

    // Whether nothing on 127.0.0.1 holds `port` now
    bool can_bind(uint16_t port) {
        boost::asio::io_context ioc;
        boost::asio::ip::tcp::acceptor acceptor(ioc);
        boost::system::error_code ec;
        acceptor.open(boost::asio::ip::tcp::v4(), ec);
        if (!ec) acceptor.bind({boost::asio::ip::make_address("127.0.0.1"), port}, ec);
        return !ec;
    }

    // A port free on 127.0.0.1 now, and out of the system's ephemeral range: no other process is
    // given it while a test leaves it free between two listeners. One the system chose could be,
    // by the tests of a parallel run listening on port 0, and a client of the test would then
    // connect to that other process
    uint16_t unshared_port() {
        constexpr int lowest = 10000;
        auto [first, last] = ephemeral_range();
        // Below the range, or above it if it leaves too few ports below
        int low = first - lowest >= 1000 ? lowest : last + 1;
        int high = first - lowest >= 1000 ? first - 1 : 65535;
        REQUIRE(high - low >= 1000);
        std::mt19937 random{std::random_device{}()};
        std::uniform_int_distribution<int> ports(low, high);
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto port = static_cast<uint16_t>(ports(random));
            if (can_bind(port)) return port;
        }
        FAIL("no free port out of the ephemeral range " << first << "-" << last);
        return 0;
    }

    // The port of a TCP listener of listening_on(): "http://127.0.0.1:41234" gives 41234
    uint16_t port_of(const std::string& url) {
        return static_cast<uint16_t>(std::stoul(url.substr(url.rfind(':') + 1)));
    }

    transport_endpoint tcp_at(uint16_t port) {
        return transport_endpoint{transport::tcp, port, {}};
    }

    transport_endpoint unix_at(const std::string& path) {
        return transport_endpoint{transport::unix, 0, path};
    }

    // Whether the descriptor is a socket listening on TCP port `port`
    bool descriptor_listens_on(int descriptor, uint16_t port) {
#if defined(__APPLE__)
        // (its getsockopt() has no SO_ACCEPTCONN)
        socket_fdinfo info{};
        if (proc_pidfdinfo(getpid(), descriptor, PROC_PIDFDSOCKETINFO, &info, sizeof(info)) != sizeof(info)) return false;
        return info.psi.soi_kind == SOCKINFO_TCP && (info.psi.soi_options & SO_ACCEPTCONN) != 0 &&
               ntohs(static_cast<uint16_t>(info.psi.soi_proto.pri_tcp.tcpsi_ini.insi_lport)) == port;
#else
        int listening = 0;
        socklen_t size = sizeof(listening);
        if (::getsockopt(descriptor, SOL_SOCKET, SO_ACCEPTCONN, &listening, &size) != 0 || !listening) return false;
        sockaddr_storage local{};
        socklen_t length = sizeof(local);
        if (::getsockname(descriptor, reinterpret_cast<sockaddr*>(&local), &length) != 0) return false;
        if (local.ss_family == AF_INET) return ntohs(reinterpret_cast<const sockaddr_in&>(local).sin_port) == port;
        if (local.ss_family == AF_INET6) return ntohs(reinterpret_cast<const sockaddr_in6&>(local).sin6_port) == port;
        return false;
#endif
    }

    // Whether a socket of this process listens on TCP port `port` (/dev/fd lists its descriptors)
    bool listens_here(uint16_t port) {
        return std::ranges::any_of(std::filesystem::directory_iterator("/dev/fd"), [port](const auto& entry) {
            return descriptor_listens_on(std::stoi(entry.path().filename().string()), port);
        });
    }

    // Whether a client connects to a listener of this process on `to`. A TCP port the system
    // chose for a listener (port 0) may be given to another process once it stopped, by the tests
    // of a parallel run listening on port 0: a client connecting there does not count
    bool accepts_here(const transport_endpoint& to) {
        return can_connect(to) && (to.kind == transport::unix || listens_here(to.port));
    }

    // A pool server on workers of its own, answering GET /hello
    struct hello_server {
        http::test::running_workers workers;
        http::pool_server server{workers};

        hello_server() {
            server.get("/hello", [](http::response& res) { res.send("hi"); });
        }
    };

    // Whether `server` answers GET /hello on `to`
    bool answers(const transport_endpoint& to) {
        if (!can_connect(to)) return false;
        raw_client client(to);
        client.get("/hello");
        return client.read_until("hi");
    }

    // co_await server.shutdown(by) on an io_context of the caller's, out of the server's
    // workers: its result, or what it threw
    bool await_shutdown(http::server_base& server, clock::time_point by) {
        boost::asio::io_context ioc;
        std::optional<bool> result;
        std::exception_ptr error;
        boost::asio::co_spawn(ioc, server.shutdown(by), [&](std::exception_ptr e, bool clean) {
            error = e;
            result = clean;
        });
        // (until it ended: it waits on a timer up to `by`)
        ioc.run();
        if (error) std::rethrow_exception(error);
        return result.value();
    }

    // A pool server whose stop() override counts the calls made once its destructor started,
    // which works for `linger` (its own members going) before the pool_server's runs
    struct recording_server : http::pool_server {
        std::atomic<bool>& destroying;
        std::atomic<int>& late_calls;
        std::chrono::microseconds linger;

        recording_server(asio::workers& workers, std::atomic<bool>& destroying, std::atomic<int>& late_calls,
                         std::chrono::microseconds linger)
            : pool_server(workers), destroying(destroying), late_calls(late_calls), linger(linger) {}

        // (it does not call stop(): ~pool_server() does)
        ~recording_server() override {
            destroying = true;
            auto until = clock::now() + linger;
            while (clock::now() < until) {}
        }

        void stop() override {
            if (destroying) ++late_calls;
            pool_server::stop();
        }
    };

}

TEST_CASE("A listen() racing a shutdown() leaves nothing listening", "[server][listeners][shutdown][race][integration]") {
    hello_server app;
    auto& server = app.server;
    temporary_directory directory;
    auto path = (directory.path() / "s.sock").string();
    auto port = unshared_port();

    struct endpoint_case {
        const char* name;
        std::function<asio::listener_options()> options;
        std::optional<transport_endpoint> client;   // where a client tries to connect, if known
    };
    std::vector<endpoint_case> cases{
        {"TCP, port 0", [] { return listen_options("127.0.0.1", 0); }, std::nullopt},
        {"TCP, a fixed port", [port] { return listen_options("127.0.0.1", port); }, tcp_at(port)},
        {"Unix socket", [path] { return unix_listen_options(path); }, unix_at(path)},
    };

    // Of each iteration: what listen() opened (if it saw it listed before the shutdown took it),
    // and what listens while the shutdown is in progress, once listen() returned
    std::optional<transport_endpoint> opened;
    std::atomic<bool> returned{false};
    std::vector<std::string> listed_meanwhile;
    bool connected_meanwhile = false;
    // The shutdown goes on once listen() returned: it overlaps every listen(), which a shutdown
    // of an idle server, ending at once, might not (a listen() after it ended succeeds)
    server.on_shutdown([&] {
        while (!returned) std::this_thread::yield();
        listed_meanwhile = server.listening_on();
        connected_meanwhile = opened && accepts_here(*opened);
    });

    // (a listener of this server is told from one of another process: see accepts_here())
    REQUIRE_FALSE(server.listen(listen_options("127.0.0.1", 0)));
    REQUIRE(accepts_here(tcp_at(port_of(server.listening_on().front()))));
    server.stop_listening();

    for (const auto& endpoint : cases) {
        INFO(endpoint.name);
        int listened = 0;
        for (int i = 0; i < race_iterations; ++i) {
            INFO("iteration " << i);
            // A server listening (a shutdown() once one ended, and before the server listens
            // again, would share its end instead of starting)
            REQUIRE_FALSE(server.listen(listen_options("127.0.0.1", 0)));
            boost::system::error_code ec;
            opened = endpoint.client;
            returned = false;
            race(i,
                 [&] {
                     ec = server.listen(endpoint.options());
                     if (!ec && !endpoint.client) {
                         auto urls = server.listening_on();
                         if (urls.size() == 2) opened = tcp_at(port_of(urls.back()));
                     }
                     returned = true;
                 },
                 [&] { server.shutdown(200ms); });
            INFO("listen(): " << ec.message());
            if (!ec) ++listened;
            // Its listener is stopped as the shutdown starts, or it failed
            REQUIRE(listed_meanwhile.empty());
            REQUIRE_FALSE(connected_meanwhile);
            // And nothing listens once both returned
            REQUIRE(server.listening_on().empty());
            REQUIRE_FALSE(server.is_listening());
            if (opened) REQUIRE_FALSE(accepts_here(*opened));
        }
        // (both outcomes happen; how often depends on the machine)
        UNSCOPED_INFO(endpoint.name << ": listen() succeeded " << listened << " of " << race_iterations);
    }

    // The server serves again afterwards
    server.on_shutdown(nullptr);
    REQUIRE_FALSE(server.listen(listen_options("127.0.0.1", port)));
    REQUIRE(answers(tcp_at(port)));
}

TEST_CASE("A listen() racing stop_listening() or stop() is listed if a client can connect",
          "[server][listeners][race][integration]") {
    hello_server app;
    auto& server = app.server;
    temporary_directory directory;
    auto path = (directory.path() / "s.sock").string();
    auto port = unshared_port();

    for (bool full_stop : {false, true}) {
        for (bool on_unix : {false, true}) {
            INFO((full_stop ? "stop()" : "stop_listening()") << (on_unix ? " on a Unix socket" : " on TCP"));
            auto client = on_unix ? unix_at(path) : tcp_at(port);
            for (int i = 0; i < race_iterations; ++i) {
                INFO("iteration " << i);
                boost::system::error_code ec;
                race(i,
                     [&] { ec = server.listen(on_unix ? unix_listen_options(path) : listen_options("127.0.0.1", port)); },
                     [&] {
                         if (full_stop) server.stop();
                         else server.stop_listening();
                     });
                auto listed = server.listening_on();
                REQUIRE(listed.size() <= 1);
                // Listed only by a listen() that succeeded; and a client connects only if listed
                if (!listed.empty()) REQUIRE_FALSE(ec);
                REQUIRE(can_connect(client) == !listed.empty());
                server.stop_listening();
                REQUIRE(server.listening_on().empty());
                REQUIRE_FALSE(can_connect(client));
            }
        }
    }
}

TEST_CASE("Two listen() on the same endpoint at once open one listener at most", "[server][listeners][race][integration]") {
    hello_server app;
    auto& server = app.server;
    temporary_directory directory;
    auto path = (directory.path() / "s.sock").string();
    auto port = unshared_port();

    for (bool on_unix : {false, true}) {
        INFO((on_unix ? "Unix socket" : "TCP"));
        auto options = [&] {
            auto options = on_unix ? unix_listen_options(path) : listen_options("127.0.0.1", port);
            // (one attempt: a bind refused by the system fails at once, instead of retrying)
            options.max_listening_attempts = 1;
            return options;
        };
        auto client = on_unix ? unix_at(path) : tcp_at(port);
        for (int i = 0; i < same_endpoint_iterations; ++i) {
            INFO("iteration " << i);
            boost::system::error_code first;
            boost::system::error_code second;
            race(i, [&] { first = server.listen(options()); }, [&] { second = server.listen(options()); });
            INFO("first: " << first.message() << ", second: " << second.message());
            REQUIRE((!first || !second));
            if (on_unix) {
                // The system does not refuse a second bind on a path (a listen() replaces its
                // file): the server must
                REQUIRE((first == boost::asio::error::address_in_use || second == boost::asio::error::address_in_use));
            } else {
                REQUIRE((first || second));
            }
            REQUIRE(server.listening_on().size() <= 1);
            if (!first || !second) {
                REQUIRE(server.listening_on().size() == 1);
                REQUIRE(answers(client));
            }
            server.stop_listening();
            REQUIRE_FALSE(can_connect(client));
        }
    }
}

TEST_CASE("listen() fails while a shutdown is in progress, and works once it ended", "[server][listeners][shutdown][integration]") {
    hello_server app;
    auto& server = app.server;
    std::promise<void> started;
    std::promise<void> finish;
    auto finished = finish.get_future().share();
    server.get("/slow", [&started, finished](http::response& res) {
        started.set_value();
        // The shutdown waits for this request, which ends when the test tells it
        finished.wait();
        res.send("done");
    });
    auto port = unshared_port();
    auto other = unshared_port();
    REQUIRE_FALSE(server.listen(listen_options("127.0.0.1", port)));
    raw_client busy(tcp_at(port));
    busy.get("/slow");
    REQUIRE(started.get_future().wait_for(3s) == std::future_status::ready);

    auto shutdown = std::async(std::launch::async, [&] { return server.shutdown(5s); });
    REQUIRE(shutdown_test::eventually([&] { return !server.is_listening(); }));

    // In progress: listen() opens nothing
    auto ec = server.listen(listen_options("127.0.0.1", other));
    INFO("listen(): " << ec.message());
    REQUIRE(ec);
    REQUIRE(server.listening_on().empty());
    REQUIRE_FALSE(can_connect(other));
    REQUIRE(shutdown.wait_for(0ms) == std::future_status::timeout);

    finish.set_value();
    REQUIRE(shutdown.wait_for(3s) == std::future_status::ready);
    REQUIRE(shutdown.get());
    REQUIRE(busy.rest().data.ends_with("done"));

    // Ended: the server serves again, and a second shutdown ends as the first
    REQUIRE_FALSE(server.listen(listen_options("127.0.0.1", other)));
    REQUIRE(answers(tcp_at(other)));
    REQUIRE(server.shutdown(2s));
    REQUIRE(server.listening_on().empty());
    REQUIRE_FALSE(can_connect(other));
    REQUIRE_FALSE(server.listen(listen_options("127.0.0.1", other)));
    REQUIRE(answers(tcp_at(other)));
}

TEST_CASE("listen() from on_shutdown() fails, without a deadlock", "[server][listeners][shutdown][integration]") {
    hello_server app;
    auto& server = app.server;
    auto port = unshared_port();
    auto other = unshared_port();
    boost::system::error_code from_callback;
    server.on_shutdown([&] { from_callback = server.listen(listen_options("127.0.0.1", other)); });
    REQUIRE_FALSE(server.listen(listen_options("127.0.0.1", port)));

    auto shutdown = std::async(std::launch::async, [&] { return server.shutdown(2s); });
    REQUIRE(shutdown.wait_for(3s) == std::future_status::ready);
    REQUIRE(shutdown.get());
    INFO("listen() in on_shutdown(): " << from_callback.message());
    REQUIRE(from_callback);
    REQUIRE(server.listening_on().empty());
    REQUIRE_FALSE(can_connect(other));
    REQUIRE_FALSE(can_connect(port));
}

TEST_CASE("A listen() retrying its bind does not delay a shutdown, nor outlive it", "[server][listeners][shutdown][integration]") {
    hello_server app;
    auto& server = app.server;
    // The port is taken: the listen() waits to bind again (every 5 s)
    boost::asio::io_context ioc;
    auto blocker = std::make_unique<boost::asio::ip::tcp::acceptor>(
        ioc, boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), 0});
    auto port = blocker->local_endpoint().port();

    auto listening = std::async(std::launch::async, [&] {
        auto options = listen_options("127.0.0.1", port);
        options.max_listening_attempts = 2;
        return server.listen(std::move(options));
    });
    // Until it is retrying (its first bind fails at once)
    std::this_thread::sleep_for(100ms);
    REQUIRE(listening.wait_for(0ms) == std::future_status::timeout);

    auto begin = clock::now();
    REQUIRE(server.shutdown(200ms));
    REQUIRE(clock::now() - begin < 200ms);

    // The port is free now: a retry would bind it
    blocker.reset();
    REQUIRE(listening.wait_for(6s) == std::future_status::ready);
    auto ec = listening.get();
    INFO("listen(): " << ec.message());
    REQUIRE(ec);
    REQUIRE(server.listening_on().empty());
    REQUIRE_FALSE(can_connect(port));
}

TEST_CASE("A shutdown() racing workers.stop() returns in time, without throwing", "[server][shutdown][pool][race][integration]") {
    http::test::running_workers workers;
    int clean = 0;
    // Each way of shutting down in half the iterations, the workers stopping before or after
    for (int i = 0; i < race_iterations; ++i) {
        bool awaited = (i / 2) % 2;
        INFO("iteration " << i << ", " << (awaited ? "co_await shutdown(by), out of the workers" : "shutdown(grace)"));
        // (restarted, after the last iteration stopped them)
        if (!workers.running()) REQUIRE_FALSE(workers.start(2));
        auto server = std::make_unique<http::pool_server>(workers);
        server->get("/hello", [](http::response& res) { res.send("hi"); });
        REQUIRE_FALSE(server->listen(listen_options("127.0.0.1", 0)));
        // A connection open (idle, once answered)
        raw_client client(tcp_at(port_of(server->listening_on().front())));
        client.get("/hello");
        REQUIRE(client.read_until("hi"));

        std::optional<bool> result;
        std::string thrown;
        clock::duration took{};
        race(i,
             [&] {
                 auto begin = clock::now();
                 try {
                     result = awaited ? await_shutdown(*server, clock::now() + 200ms) : server->shutdown(200ms);
                 } catch (const std::exception& e) {
                     thrown = e.what();
                 }
                 took = clock::now() - begin;
             },
             [&] { workers.stop(); });
        INFO("thrown: " << thrown);
        REQUIRE(thrown.empty());
        REQUIRE(result.has_value());
        // Within its grace and the close margin
        REQUIRE(took < 700ms);
        // True only if nothing was left open
        if (*result) {
            ++clean;
            REQUIRE(server->stats().active_connections() == 0);
        }
        auto begin = clock::now();
        server.reset();
        REQUIRE(clock::now() - begin < 2s);
    }
    // (both outcomes happen; how often depends on the machine)
    UNSCOPED_INFO("clean shutdowns: " << clean << " of " << race_iterations);
}

TEST_CASE("The io_context a pool server's shutdown waits on is read without throwing as the workers stop",
          "[server][shutdown][pool][race][integration]") {
    // Its shutdown_context(), called the way the shutdown starts: from a thread out of the
    // workers, as they stop and start again
    struct exposed_server : http::pool_server {
        using pool_server::pool_server;
        using pool_server::shutdown_context;
    };
    http::test::running_workers workers{1};
    exposed_server server(workers);
    std::atomic<bool> finished{false};
    std::atomic<int> reads{0};
    std::atomic<int> found{0};
    std::string thrown;
    std::thread reader([&] {
        try {
            while (!finished) {
                if (server.shutdown_context()) ++found;
                ++reads;
            }
        } catch (const std::exception& e) {
            thrown = e.what();
        }
    });
    for (int i = 0; i < 1000 && thrown.empty(); ++i) {
        workers.stop();
        REQUIRE_FALSE(workers.start(1));
    }
    finished = true;
    reader.join();
    INFO("thrown: " << thrown << " (" << reads << " reads, " << found << " found an io_context)");
    REQUIRE(thrown.empty());
}

TEST_CASE("A shutdown() once the workers stopped ends at once", "[server][shutdown][pool][integration]") {
    http::test::running_workers workers;
    // (one shutdown each: a second one would share the end of the first)
    http::pool_server blocking(workers);
    http::pool_server awaited(workers);
    REQUIRE_FALSE(blocking.listen(listen_options("127.0.0.1", 0)));
    REQUIRE_FALSE(awaited.listen(listen_options("127.0.0.1", 0)));
    workers.stop();

    // As a server that is not running: true, as no connection is open
    auto begin = clock::now();
    REQUIRE(blocking.shutdown(5s));
    REQUIRE(await_shutdown(awaited, clock::now() + 5s));
    REQUIRE(clock::now() - begin < 1s);
    REQUIRE(blocking.listening_on().empty());
    REQUIRE(awaited.listening_on().empty());
}

TEST_CASE("workers.stop() on the worker awaiting a shutdown neither throws nor hangs", "[server][shutdown][pool][integration]") {
    http::test::running_workers workers;
    for (int i = 0; i < 20; ++i) {
        INFO("iteration " << i);
        if (!workers.running()) REQUIRE_FALSE(workers.start(2));
        auto server = std::make_unique<http::pool_server>(workers);
        server->get("/hello", [](http::response& res) { res.send("hi"); });
        REQUIRE_FALSE(server->listen(listen_options("127.0.0.1", 0)));
        raw_client client(tcp_at(port_of(server->listening_on().front())));
        client.get("/hello");
        REQUIRE(client.read_until("hi"));

        // The coroutine may never resume (its worker stops): what it throws, if it does
        auto thrown = std::make_shared<std::atomic<bool>>(false);
        boost::asio::co_spawn(
            workers.next_executor(),
            [&workers, &server]() -> thinger::awaitable<bool> {
                // On this worker, once this coroutine awaits the shutdown
                boost::asio::post(co_await boost::asio::this_coro::executor, [&workers] { workers.stop(); });
                co_return co_await server->shutdown(clock::now() + 200ms);
            },
            [thrown](std::exception_ptr error, bool) {
                if (error) *thrown = true;
            });
        REQUIRE(shutdown_test::eventually([&] { return !workers.running(); }));
        auto begin = clock::now();
        server.reset();
        REQUIRE(clock::now() - begin < 2s);
        REQUIRE_FALSE(*thrown);
    }
}

TEST_CASE("A shutdown ending on a worker never calls the stop() of a subclass being destroyed",
          "[server][shutdown][pool][race][integration]") {
    http::test::running_workers workers;
    for (int i = 0; i < race_iterations; ++i) {
        INFO("iteration " << i);
        std::atomic<bool> destroying{false};
        std::atomic<int> late_calls{0};
        // Its destructor works longer in each iteration, up to 190 µs: the shutdown ends on a
        // worker before it, during it, or once the pool_server's waits for it
        auto server = std::make_unique<recording_server>(workers, destroying, late_calls,
                                                         std::chrono::microseconds((i % 20) * 10));
        // Started on a worker, which it does not block (see shutdown(grace)): it ends there, a
        // few microseconds later, as no connection is open
        std::atomic<bool> started{false};
        boost::asio::post(workers.next_executor(), [&] {
            server->shutdown(200ms);
            started = true;
        });
        while (!started) std::this_thread::yield();
        server.reset();
        INFO("stop() calls once its destructor started: " << late_calls);
        REQUIRE(late_calls == 0);
    }
}

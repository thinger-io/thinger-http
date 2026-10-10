#include <catch2/catch_test_macros.hpp>

#include <thinger/asio/tcp_socket_server.hpp>
#include <thinger/asio/unix_socket_server.hpp>
#include <thinger/asio/worker_client.hpp>
#include <thinger/http/server/server_standalone.hpp>

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

using namespace thinger::asio;
using namespace std::chrono_literals;

// Setting an accepted connection up (or serving it) may fail: it must only drop that
// connection, and never escape the accept handler or the worker serving it, which would end the
// process. Servers accepting on their io_context providers and on a thread of their own (pool
// servers on the workers), over TCP and Unix domain sockets

namespace {

// An io_context run on a thread of its own, as the one a server accepts on
struct io_thread {
    boost::asio::io_context io_context;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{io_context.get_executor()};
    std::thread thread{[this] { io_context.run(); }};

    io_context_provider provider() {
        return [this]() -> boost::asio::io_context& { return io_context; };
    }

    ~io_thread() {
        work.reset();
        io_context.stop();
        thread.join();
    }
};

// Keeps the workers running, for the servers accepting on a thread of their own
struct workers_in_use : worker_client {
    workers_in_use() : worker_client("socket server setup test") {}
};

// Connects, and waits for the server to close the connection (true) or the timeout
template<typename Protocol>
bool closed_by_server(const typename Protocol::endpoint& endpoint) {
    boost::asio::io_context io_context;
    typename Protocol::socket sock(io_context);
    sock.connect(endpoint);
    bool closed = false;
    char byte;
    sock.async_read_some(boost::asio::buffer(&byte, 1),
                         [&closed](const boost::system::error_code& ec, size_t) { closed = bool(ec); });
    io_context.run_for(5s);
    return closed;
}

template<typename Protocol, typename Server>
void keeps_serving_after_a_setup_error(Server& server, const std::function<typename Protocol::endpoint()>& endpoint) {
    std::atomic<int> calls{0};
    std::atomic<int> served{0};
    server.set_max_listening_attempts(1);
    server.set_handler([&](std::shared_ptr<thinger::asio::socket> sock) {
        if (++calls == 1) throw std::runtime_error("cannot set the connection up");
        ++served;
        sock->close();
    });
    REQUIRE(server.start());

    // Dropped: its socket is closed while unwinding
    REQUIRE(closed_by_server<Protocol>(endpoint()));
    REQUIRE(calls == 1);

    // The next one is served
    REQUIRE(closed_by_server<Protocol>(endpoint()));
    REQUIRE(calls == 2);
    REQUIRE(served == 1);

    REQUIRE(server.stop());
}

boost::asio::ip::tcp::endpoint tcp_endpoint(const tcp_socket_server& server) {
    return {boost::asio::ip::make_address("127.0.0.1"), server.local_port()};
}

std::string socket_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() /
            ("thinger_setup_test_" + name + "_" + std::to_string(getpid()) + ".sock")).string();
}

} // namespace

TEST_CASE("TCP server keeps serving after a connection fails to be set up", "[tcp][server][integration]") {
    SECTION("Accepting on an io_context provider") {
        io_thread io;
        tcp_socket_server server("127.0.0.1", "0", io.provider(), io.provider());
        keeps_serving_after_a_setup_error<boost::asio::ip::tcp>(server, [&server] { return tcp_endpoint(server); });
    }
    SECTION("Accepting on a thread of its own") {
        workers_in_use workers;
        tcp_socket_server server("127.0.0.1", "0");
        keeps_serving_after_a_setup_error<boost::asio::ip::tcp>(server, [&server] { return tcp_endpoint(server); });
    }
}

TEST_CASE("Unix socket server keeps serving after a connection fails to be set up", "[unix][server][integration]") {
    SECTION("Accepting on an io_context provider") {
        auto path = socket_path("provider");
        io_thread io;
        unix_socket_server server(path, io.provider(), io.provider());
        keeps_serving_after_a_setup_error<boost::asio::local::stream_protocol>(server, [&path] {
            return boost::asio::local::stream_protocol::endpoint(path);
        });
    }
    SECTION("Accepting on a thread of its own") {
        auto path = socket_path("pool");
        workers_in_use workers;
        unix_socket_server server(path);
        keeps_serving_after_a_setup_error<boost::asio::local::stream_protocol>(server, [&path] {
            return boost::asio::local::stream_protocol::endpoint(path);
        });
    }
}

TEST_CASE("local_port() is 0 while not listening, and never throws", "[tcp][server][integration]") {
    SECTION("TCP socket server") {
        io_thread io;
        tcp_socket_server server("127.0.0.1", "0", io.provider(), io.provider());
        server.set_handler([](std::shared_ptr<thinger::asio::socket>) {});
        REQUIRE(server.local_port() == 0);
        REQUIRE(server.start());
        REQUIRE(server.local_port() != 0);
        REQUIRE(server.stop());
        REQUIRE_NOTHROW(server.local_port());
        REQUIRE(server.local_port() == 0);
    }
    SECTION("HTTP server") {
        thinger::http::server server;
        REQUIRE(server.local_port() == 0);
        REQUIRE(server.listen("127.0.0.1", 0));
        REQUIRE(server.local_port() != 0);
        REQUIRE(server.stop());
        REQUIRE_NOTHROW(server.local_port());
        REQUIRE(server.local_port() == 0);
    }
}

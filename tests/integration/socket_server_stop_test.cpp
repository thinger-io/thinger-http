#include <catch2/catch_test_macros.hpp>

#include <thinger/asio/tcp_socket_server.hpp>
#include <thinger/asio/unix_socket_server.hpp>

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <type_traits>
#include <unistd.h>

using namespace thinger::asio;
using namespace std::chrono_literals;

// Servers given io_context providers accept on the acceptor context, while they may be stopped
// from any other thread: stop() must not race with the accept handlers (which accept the next
// connection on the acceptor stop() closes), and once it returns no connection is handed to
// the handler anymore and nothing listens. Started again each round, as listen/stop cycles do.
// Without the guard of the accept handlers, TSan reports the race, and accepting on a closed
// acceptor crashes now and then

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

template<typename Protocol>
bool can_connect(const typename Protocol::endpoint& endpoint) {
    boost::asio::io_context io_context;
    typename Protocol::socket sock(io_context);
    boost::system::error_code ec;
    sock.connect(endpoint, ec);
    return !ec;
}

template<typename Protocol, typename Server>
void stop_while_connecting(Server& server, const std::function<typename Protocol::endpoint()>& endpoint) {
    std::atomic<int> served{0};
    std::atomic<bool> stopped{false};
    std::atomic<int> served_after_stop{0};

    server.set_max_listening_attempts(1);
    server.set_handler([&](std::shared_ptr<thinger::asio::socket> sock) {
        if (stopped) ++served_after_stop;
        ++served;
        sock->close();
    });

    for (int round = 0; round < 50; ++round) {
        stopped = false;
        REQUIRE(server.start());
        auto listening = endpoint();
        int before = served;
        {
            connecting_thread<Protocol> connecting(listening);
            // Stop once connections are being accepted
            auto deadline = std::chrono::steady_clock::now() + 5s;
            while (served < before + 3 && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(100us);
            }
            REQUIRE(served >= before + 3);
            REQUIRE(server.stop());
            stopped = true;
        }
        // Nothing listens on the socket file anymore. A TCP port may be taken by another
        // server meanwhile: connections still accepted on it would be counted below
        if constexpr (std::is_same_v<Protocol, boost::asio::local::stream_protocol>) {
            REQUIRE_FALSE(can_connect<Protocol>(listening));
        }
    }

    REQUIRE(served_after_stop == 0);
}

} // namespace

TEST_CASE("TCP server stops from another thread while accepting", "[tcp][server][stop][integration]") {
    io_thread io;
    tcp_socket_server server("127.0.0.1", "0", io.provider(), io.provider());
    stop_while_connecting<boost::asio::ip::tcp>(server, [&server] {
        return boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), server.local_port());
    });
}

TEST_CASE("Unix socket server stops from another thread while accepting", "[unix][server][stop][integration]") {
    auto path = (std::filesystem::temp_directory_path() /
                 ("thinger_stop_test_" + std::to_string(getpid()) + ".sock")).string();
    io_thread io;
    unix_socket_server server(path, io.provider(), io.provider());
    stop_while_connecting<boost::asio::local::stream_protocol>(server, [&path] {
        return boost::asio::local::stream_protocol::endpoint(path);
    });
}

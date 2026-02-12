#include <catch2/catch_test_macros.hpp>
#include <thinger/asio/socket_pipe.hpp>
#include <thinger/asio/sockets/tcp_socket.hpp>
#include <thinger/util/logger.hpp>

#include <boost/asio.hpp>
#include <thread>
#include <chrono>
#include <numeric>
#include <atomic>

using namespace thinger;
using namespace thinger::asio;
using namespace std::chrono_literals;
namespace net = boost::asio;
using net::ip::tcp;

namespace {

// Create an acceptor on an OS-assigned port (port 0) to avoid conflicts
tcp::acceptor make_acceptor(net::io_context& io) {
    tcp::acceptor acc(io, {tcp::v4(), 0});
    acc.set_option(net::socket_base::reuse_address(true));
    return acc;
}

uint16_t get_port(const tcp::acceptor& acc) {
    return acc.local_endpoint().port();
}

// Simple coroutine echo server: accepts one connection, echoes until EOF.
awaitable<void> echo_session(tcp::socket sock) {
    uint8_t buf[8192];
    for (;;) {
        auto [ec, n] = co_await sock.async_read_some(
            net::buffer(buf), use_nothrow_awaitable);
        if (ec || n == 0) break;
        auto [wec, wn] = co_await net::async_write(
            sock, net::buffer(buf, n), use_nothrow_awaitable);
        if (wec) break;
    }
}

awaitable<void> echo_server(tcp::acceptor& acceptor) {
    for (;;) {
        auto [ec, sock] = co_await acceptor.async_accept(use_nothrow_awaitable);
        if (ec) break;
        co_spawn(acceptor.get_executor(), echo_session(std::move(sock)), detached);
    }
}

// Proxy: accepts one client connection, connects to backend, pipes them.
awaitable<void> proxy_session(
    tcp::socket client_raw,
    uint16_t backend_port,
    std::shared_ptr<socket_pipe>& pipe_out)
{
    auto& io = static_cast<net::io_context&>(client_raw.get_executor().context());

    // Wrap client socket
    auto client_sock = std::make_shared<tcp_socket>("pipe-test-client", std::move(client_raw));

    // Connect to backend echo server
    auto backend_sock = std::make_shared<tcp_socket>("pipe-test-backend", io);
    co_await backend_sock->connect("127.0.0.1", std::to_string(backend_port), std::chrono::seconds(5));

    // Create and run the pipe
    auto pipe = std::make_shared<socket_pipe>(client_sock, backend_sock);
    pipe_out = pipe;
    co_await pipe->run();
}

} // anonymous namespace

TEST_CASE("Socket pipe bidirectional forwarding", "[socket-pipe]") {
    net::io_context io;

    // Start echo server (OS-assigned port)
    tcp::acceptor echo_acc = make_acceptor(io);
    uint16_t echo_port = get_port(echo_acc);
    co_spawn(io, echo_server(echo_acc), detached);

    // Start proxy acceptor (OS-assigned port)
    tcp::acceptor proxy_acc = make_acceptor(io);
    uint16_t proxy_port = get_port(proxy_acc);

    std::shared_ptr<socket_pipe> pipe;
    co_spawn(io, [&]() -> awaitable<void> {
        auto [ec, sock] = co_await proxy_acc.async_accept(use_nothrow_awaitable);
        if (!ec) {
            co_await proxy_session(std::move(sock), echo_port, pipe);
        }
    }, detached);

    // Run io_context in background
    std::thread io_thread([&io] { io.run(); });

    // Give servers time to start
    std::this_thread::sleep_for(50ms);

    SECTION("Small data round-trip") {
        // Connect a synchronous client to the proxy
        tcp::socket client(io);
        client.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), proxy_port));

        std::string msg = "Hello, socket_pipe!";
        net::write(client, net::buffer(msg));

        // Read the echoed response
        char buf[256]{};
        size_t n = client.read_some(net::buffer(buf, sizeof(buf)));
        std::string response(buf, n);

        REQUIRE(response == msg);

        client.shutdown(tcp::socket::shutdown_both);
        client.close();
    }

    SECTION("Large transfer with verification") {
        tcp::socket client(io);
        client.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), proxy_port));

        // Generate 1 MB of deterministic data
        constexpr size_t total = 1024 * 1024;
        std::vector<uint8_t> send_data(total);
        std::iota(send_data.begin(), send_data.end(), uint8_t(0));

        // Send in chunks
        size_t sent = 0;
        while (sent < total) {
            size_t chunk = std::min<size_t>(4096, total - sent);
            net::write(client, net::buffer(send_data.data() + sent, chunk));
            sent += chunk;
        }

        // Read all echoed data
        std::vector<uint8_t> recv_data(total);
        size_t received = 0;
        while (received < total) {
            size_t n = client.read_some(net::buffer(recv_data.data() + received, total - received));
            REQUIRE(n > 0);
            received += n;
        }

        REQUIRE(received == total);
        REQUIRE(send_data == recv_data);

        client.shutdown(tcp::socket::shutdown_both);
        client.close();
    }

    // Cleanup
    std::this_thread::sleep_for(50ms);
    echo_acc.close();
    proxy_acc.close();
    if (pipe) pipe->cancel();
    io.stop();
    io_thread.join();
}

TEST_CASE("Socket pipe cancel stops both directions", "[socket-pipe]") {
    net::io_context io;

    tcp::acceptor echo_acc = make_acceptor(io);
    uint16_t echo_port = get_port(echo_acc);
    co_spawn(io, echo_server(echo_acc), detached);

    tcp::acceptor proxy_acc = make_acceptor(io);
    uint16_t proxy_port = get_port(proxy_acc);

    std::shared_ptr<socket_pipe> pipe;
    std::atomic<bool> pipe_finished{false};

    co_spawn(io, [&]() -> awaitable<void> {
        auto [ec, sock] = co_await proxy_acc.async_accept(use_nothrow_awaitable);
        if (ec) co_return;

        auto& ctx = static_cast<net::io_context&>(sock.get_executor().context());
        auto client_sock = std::make_shared<tcp_socket>("cancel-test-client", std::move(sock));
        auto backend_sock = std::make_shared<tcp_socket>("cancel-test-backend", ctx);
        co_await backend_sock->connect("127.0.0.1", std::to_string(echo_port), std::chrono::seconds(5));

        pipe = std::make_shared<socket_pipe>(client_sock, backend_sock);
        co_await pipe->run();
        pipe_finished = true;
    }, detached);

    std::thread io_thread([&io] { io.run(); });
    std::this_thread::sleep_for(50ms);

    // Connect client
    tcp::socket client(io);
    client.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), proxy_port));

    // Verify pipe works
    std::string msg = "test";
    net::write(client, net::buffer(msg));
    char buf[64]{};
    size_t n = client.read_some(net::buffer(buf));
    REQUIRE(std::string(buf, n) == msg);

    // Cancel the pipe
    REQUIRE(pipe);
    pipe->cancel();

    // Wait for pipe to finish
    std::this_thread::sleep_for(100ms);
    REQUIRE(pipe_finished);

    // Sockets should no longer be usable
    REQUIRE_FALSE(pipe->get_source()->is_open());
    REQUIRE_FALSE(pipe->get_target()->is_open());

    echo_acc.close();
    proxy_acc.close();
    boost::system::error_code ec;
    client.shutdown(tcp::socket::shutdown_both, ec);
    client.close(ec);
    io.stop();
    io_thread.join();
}

TEST_CASE("Socket pipe on_end callback fires on destruction", "[socket-pipe]") {
    net::io_context io;
    bool on_end_called = false;

    // Create two tcp_socket instances (they don't need to be connected for this test)
    auto s1 = std::make_shared<tcp_socket>("onend-s1", io);
    auto s2 = std::make_shared<tcp_socket>("onend-s2", io);

    {
        auto pipe = std::make_shared<socket_pipe>(s1, s2);
        pipe->set_on_end([&on_end_called] { on_end_called = true; });
        REQUIRE_FALSE(on_end_called);
        // pipe goes out of scope here
    }

    REQUIRE(on_end_called);
}

TEST_CASE("Socket pipe on_end callback fires after run completes", "[socket-pipe]") {
    net::io_context io;

    tcp::acceptor echo_acc = make_acceptor(io);
    uint16_t echo_port = get_port(echo_acc);
    co_spawn(io, echo_server(echo_acc), detached);

    tcp::acceptor proxy_acc = make_acceptor(io);
    uint16_t proxy_port = get_port(proxy_acc);

    std::shared_ptr<socket_pipe> pipe;
    std::atomic<bool> on_end_called{false};
    std::atomic<bool> pipe_run_done{false};

    co_spawn(io, [&]() -> awaitable<void> {
        auto [ec, sock] = co_await proxy_acc.async_accept(use_nothrow_awaitable);
        if (ec) co_return;

        auto& ctx = static_cast<net::io_context&>(sock.get_executor().context());
        auto client_sock = std::make_shared<tcp_socket>("onend-run-client", std::move(sock));
        auto backend_sock = std::make_shared<tcp_socket>("onend-run-backend", ctx);
        co_await backend_sock->connect("127.0.0.1", std::to_string(echo_port), std::chrono::seconds(5));

        pipe = std::make_shared<socket_pipe>(client_sock, backend_sock);
        pipe->set_on_end([&on_end_called] { on_end_called = true; });
        co_await pipe->run();
        pipe_run_done = true;
    }, detached);

    std::thread io_thread([&io] { io.run(); });
    std::this_thread::sleep_for(50ms);

    // Connect and send data, then close
    tcp::socket client(io);
    client.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), proxy_port));

    std::string msg = "test on_end";
    net::write(client, net::buffer(msg));
    char buf[64]{};
    client.read_some(net::buffer(buf));

    client.shutdown(tcp::socket::shutdown_both);
    client.close();

    // Wait for pipe run to complete
    std::this_thread::sleep_for(300ms);
    REQUIRE(pipe_run_done);
    REQUIRE_FALSE(on_end_called);  // pipe still held by external shared_ptr

    pipe.reset();
    REQUIRE(on_end_called);

    echo_acc.close();
    proxy_acc.close();
    io.stop();
    io_thread.join();
}

TEST_CASE("Socket pipe transfer stats are correct", "[socket-pipe]") {
    net::io_context io;

    tcp::acceptor echo_acc = make_acceptor(io);
    uint16_t echo_port = get_port(echo_acc);
    co_spawn(io, echo_server(echo_acc), detached);

    tcp::acceptor proxy_acc = make_acceptor(io);
    uint16_t proxy_port = get_port(proxy_acc);

    std::shared_ptr<socket_pipe> pipe;

    co_spawn(io, [&]() -> awaitable<void> {
        auto [ec, sock] = co_await proxy_acc.async_accept(use_nothrow_awaitable);
        if (ec) co_return;

        auto& ctx = static_cast<net::io_context&>(sock.get_executor().context());
        auto client_sock = std::make_shared<tcp_socket>("stats-test-client", std::move(sock));
        auto backend_sock = std::make_shared<tcp_socket>("stats-test-backend", ctx);
        co_await backend_sock->connect("127.0.0.1", std::to_string(echo_port), std::chrono::seconds(5));

        pipe = std::make_shared<socket_pipe>(client_sock, backend_sock);
        co_await pipe->run();
    }, detached);

    std::thread io_thread([&io] { io.run(); });
    std::this_thread::sleep_for(50ms);

    tcp::socket client(io);
    client.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), proxy_port));

    // Send known amounts of data
    std::string msg1 = "Hello";      // 5 bytes
    std::string msg2 = "World!!!";   // 8 bytes
    net::write(client, net::buffer(msg1));
    char buf[64]{};
    client.read_some(net::buffer(buf));
    net::write(client, net::buffer(msg2));
    client.read_some(net::buffer(buf));

    std::this_thread::sleep_for(50ms);

    REQUIRE(pipe);
    // source->target = data from client going to echo server = 13 bytes
    REQUIRE(pipe->bytes_source_to_target() == 13);
    // target->source = echoed data coming back = 13 bytes
    REQUIRE(pipe->bytes_target_to_source() == 13);

    client.shutdown(tcp::socket::shutdown_both);
    client.close();
    std::this_thread::sleep_for(50ms);
    echo_acc.close();
    proxy_acc.close();
    if (pipe) pipe->cancel();
    io.stop();
    io_thread.join();
}

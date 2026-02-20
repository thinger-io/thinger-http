#include <catch2/catch_test_macros.hpp>

#include <thinger/asio/sockets/tcp_socket.hpp>
#include <thinger/asio/sockets/unix_socket.hpp>
#include <thinger/asio/tcp_socket_server.hpp>
#include <thinger/asio/unix_socket_server.hpp>

#include <boost/asio/steady_timer.hpp>

#include <thread>
#include <chrono>
#include <filesystem>
#include <cstring>

using namespace thinger;
using namespace thinger::asio;
using namespace std::chrono_literals;
using namespace std::string_view_literals;

// ============================================================================
// Helper: run a client coroutine on a fresh io_context
// ============================================================================

template<typename CoroFactory>
void run_client(CoroFactory&& factory) {
    boost::asio::io_context ctx;
    bool done = false;
    co_spawn(ctx, [&]() -> awaitable<void> {
        co_await factory(ctx);
        done = true;
    }, [](std::exception_ptr p) {
        if (p) std::rethrow_exception(p);
    });
    ctx.run();
    REQUIRE(done);
}

// ============================================================================
// TCP Echo Fixture
// ============================================================================

struct TcpEchoFixture {
    boost::asio::io_context io_ctx;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work;
    tcp_socket_server server;
    std::thread io_thread;

    TcpEchoFixture()
        : work(io_ctx.get_executor())
        , server("127.0.0.1", "0",
                 [this]() -> boost::asio::io_context& { return io_ctx; },
                 [this]() -> boost::asio::io_context& { return io_ctx; })
    {
        server.set_max_listening_attempts(1);
        server.set_handler([this](std::shared_ptr<asio::socket> sock) {
            co_spawn(io_ctx, [sock]() -> awaitable<void> {
                uint8_t buf[4096];
                while (sock->is_open()) {
                    auto n = co_await sock->read_some(buf, sizeof(buf));
                    if (n == 0) break;
                    co_await sock->write(buf, n);
                }
            }, detached);
        });
        REQUIRE(server.start());
        io_thread = std::thread([this]() { io_ctx.run(); });
    }

    ~TcpEchoFixture() {
        server.stop();
        work.reset();
        io_ctx.stop();
        if (io_thread.joinable()) io_thread.join();
    }

    std::string port_str() const {
        return std::to_string(server.local_port());
    }
};

// ============================================================================
// Unix Echo Fixture
// ============================================================================

struct UnixEchoFixture {
    boost::asio::io_context io_ctx;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work;
    std::string socket_path;
    unix_socket_server server;
    std::thread io_thread;

    UnixEchoFixture()
        : work(io_ctx.get_executor())
        , socket_path((std::filesystem::temp_directory_path() /
                       ("thinger_io_test_" + std::to_string(getpid()) + "_" +
                        std::to_string(reinterpret_cast<uintptr_t>(this)) + ".sock")).string())
        , server(socket_path,
                 [this]() -> boost::asio::io_context& { return io_ctx; },
                 [this]() -> boost::asio::io_context& { return io_ctx; })
    {
        std::filesystem::remove(socket_path);
        server.set_max_listening_attempts(1);
        server.set_handler([this](std::shared_ptr<asio::socket> sock) {
            co_spawn(io_ctx, [sock]() -> awaitable<void> {
                uint8_t buf[4096];
                while (sock->is_open()) {
                    auto n = co_await sock->read_some(buf, sizeof(buf));
                    if (n == 0) break;
                    co_await sock->write(buf, n);
                }
            }, detached);
        });
        REQUIRE(server.start());
        io_thread = std::thread([this]() { io_ctx.run(); });
    }

    ~UnixEchoFixture() {
        server.stop();
        work.reset();
        io_ctx.stop();
        if (io_thread.joinable()) io_thread.join();
        std::filesystem::remove(socket_path);
    }
};

// ============================================================================
// TCP Tests (#1 - #10)
// ============================================================================

// #1: connect + properties
TEST_CASE_METHOD(TcpEchoFixture, "TCP: connect and properties",
                 "[tcp][socket][io][integration]") {
    run_client([this](boost::asio::io_context& ctx) -> awaitable<void> {
        tcp_socket client("test", ctx);
        REQUIRE_FALSE(client.is_open());

        auto ec = co_await client.connect("127.0.0.1", port_str(), 5s);
        REQUIRE_FALSE(ec);
        REQUIRE(client.is_open());
        REQUIRE_FALSE(client.is_secure());
        REQUIRE(client.get_remote_ip() == "127.0.0.1");
        REQUIRE(client.get_remote_port() == port_str());
        REQUIRE_FALSE(client.get_local_port().empty());
        REQUIRE(client.get_local_port() != "0");
    });
}

// #2: write(string_view) + read_some
TEST_CASE_METHOD(TcpEchoFixture, "TCP: write string_view and read_some echo",
                 "[tcp][socket][io][integration]") {
    run_client([this](boost::asio::io_context& ctx) -> awaitable<void> {
        tcp_socket client("test", ctx);
        auto ec = co_await client.connect("127.0.0.1", port_str(), 5s);
        REQUIRE_FALSE(ec);

        auto written = co_await client.write("hello echo"sv);
        REQUIRE(written == 10);

        uint8_t buf[64];
        auto n = co_await client.read_some(buf, sizeof(buf));
        REQUIRE(n == 10);
        REQUIRE(std::string_view(reinterpret_cast<char*>(buf), n) == "hello echo");
    });
}

// #3: write(uint8_t*, size) + read(uint8_t*, size)
TEST_CASE_METHOD(TcpEchoFixture, "TCP: write and read exact bytes",
                 "[tcp][socket][io][integration]") {
    run_client([this](boost::asio::io_context& ctx) -> awaitable<void> {
        tcp_socket client("test", ctx);
        auto ec = co_await client.connect("127.0.0.1", port_str(), 5s);
        REQUIRE_FALSE(ec);

        const uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
        auto written = co_await client.write(data, 5);
        REQUIRE(written == 5);

        uint8_t buf[5];
        auto n = co_await client.read(buf, 5);
        REQUIRE(n == 5);
        REQUIRE(std::memcmp(buf, data, 5) == 0);
    });
}

// #4: write(vector<const_buffer>) + read(streambuf, size)
TEST_CASE_METHOD(TcpEchoFixture, "TCP: write scatter buffers and read into streambuf",
                 "[tcp][socket][io][integration]") {
    run_client([this](boost::asio::io_context& ctx) -> awaitable<void> {
        tcp_socket client("test", ctx);
        auto ec = co_await client.connect("127.0.0.1", port_str(), 5s);
        REQUIRE_FALSE(ec);

        std::string part1 = "hello ";
        std::string part2 = "world";
        std::vector<boost::asio::const_buffer> buffers = {
            boost::asio::buffer(part1),
            boost::asio::buffer(part2)
        };
        auto written = co_await client.write(buffers);
        REQUIRE(written == 11);

        boost::asio::streambuf sb;
        auto n = co_await client.read(sb, 11);
        REQUIRE(n == 11);

        auto begin = boost::asio::buffers_begin(sb.data());
        std::string result(begin, begin + static_cast<std::ptrdiff_t>(n));
        REQUIRE(result == "hello world");
    });
}

// #5: read_until(streambuf, delim)
TEST_CASE_METHOD(TcpEchoFixture, "TCP: read_until delimiter",
                 "[tcp][socket][io][integration]") {
    run_client([this](boost::asio::io_context& ctx) -> awaitable<void> {
        tcp_socket client("test", ctx);
        auto ec = co_await client.connect("127.0.0.1", port_str(), 5s);
        REQUIRE_FALSE(ec);

        co_await client.write("hello\nworld\n"sv);

        boost::asio::streambuf sb;
        auto n = co_await client.read_until(sb, "\n");
        REQUIRE(n > 0);

        auto begin = boost::asio::buffers_begin(sb.data());
        std::string line(begin, begin + static_cast<std::ptrdiff_t>(n));
        REQUIRE(line == "hello\n");
    });
}

// #6: available()
TEST_CASE_METHOD(TcpEchoFixture, "TCP: available bytes after echo",
                 "[tcp][socket][io][integration]") {
    run_client([this](boost::asio::io_context& ctx) -> awaitable<void> {
        tcp_socket client("test", ctx);
        auto ec = co_await client.connect("127.0.0.1", port_str(), 5s);
        REQUIRE_FALSE(ec);

        co_await client.write("ping"sv);

        // Wait until data is available to read
        auto wait_ec = co_await client.wait(boost::asio::socket_base::wait_read);
        REQUIRE_FALSE(wait_ec);

        auto avail = client.available();
        REQUIRE(avail > 0);

        // Consume echoed data
        uint8_t buf[64];
        co_await client.read_some(buf, sizeof(buf));
    });
}

// #7: cancel()
TEST_CASE_METHOD(TcpEchoFixture, "TCP: cancel pending read",
                 "[tcp][socket][io][integration]") {
    run_client([this](boost::asio::io_context& ctx) -> awaitable<void> {
        tcp_socket client("test", ctx);
        auto ec = co_await client.connect("127.0.0.1", port_str(), 5s);
        REQUIRE_FALSE(ec);

        // Schedule cancel after a short delay
        co_spawn(ctx, [&client]() -> awaitable<void> {
            boost::asio::steady_timer timer(client.get_io_context());
            timer.expires_after(100ms);
            auto [ec] = co_await timer.async_wait(use_nothrow_awaitable);
            if (!ec) client.cancel();
        }, detached);

        // read_some blocks because no data is sent; cancel will abort it
        uint8_t buf[64];
        auto n = co_await client.read_some(buf, sizeof(buf));
        REQUIRE(n == 0);
    });
}

// #8: wait(wait_write)
TEST_CASE_METHOD(TcpEchoFixture, "TCP: wait for write readiness",
                 "[tcp][socket][io][integration]") {
    run_client([this](boost::asio::io_context& ctx) -> awaitable<void> {
        tcp_socket client("test", ctx);
        auto ec = co_await client.connect("127.0.0.1", port_str(), 5s);
        REQUIRE_FALSE(ec);

        auto wait_ec = co_await client.wait(boost::asio::socket_base::wait_write);
        REQUIRE_FALSE(wait_ec);
    });
}

// #9: close()
TEST_CASE_METHOD(TcpEchoFixture, "TCP: close active connection",
                 "[tcp][socket][io][integration]") {
    run_client([this](boost::asio::io_context& ctx) -> awaitable<void> {
        tcp_socket client("test", ctx);
        auto ec = co_await client.connect("127.0.0.1", port_str(), 5s);
        REQUIRE_FALSE(ec);
        REQUIRE(client.is_open());

        client.close();
        REQUIRE_FALSE(client.is_open());
    });
}

// #10: enable/disable tcp_no_delay
TEST_CASE_METHOD(TcpEchoFixture, "TCP: enable and disable tcp_no_delay",
                 "[tcp][socket][io][integration]") {
    run_client([this](boost::asio::io_context& ctx) -> awaitable<void> {
        tcp_socket client("test", ctx);
        auto ec = co_await client.connect("127.0.0.1", port_str(), 5s);
        REQUIRE_FALSE(ec);

        REQUIRE_NOTHROW(client.enable_tcp_no_delay());
        REQUIRE_NOTHROW(client.disable_tcp_no_delay());
    });
}

// ============================================================================
// Unix Socket Tests (#11 - #16)
// ============================================================================

// #11: connect + properties
TEST_CASE_METHOD(UnixEchoFixture, "Unix: connect and properties",
                 "[unix][socket][io][integration]") {
    run_client([this](boost::asio::io_context& ctx) -> awaitable<void> {
        unix_socket client("test", ctx);
        REQUIRE_FALSE(client.is_open());

        auto ec = co_await client.connect(socket_path, 5s);
        REQUIRE_FALSE(ec);
        REQUIRE(client.is_open());
        REQUIRE_FALSE(client.is_secure());
        REQUIRE_FALSE(client.get_remote_ip().empty());
        REQUIRE(client.get_local_port() == "0");
        REQUIRE(client.get_remote_port() == "0");
    });
}

// #12: write(string_view) + read_some
TEST_CASE_METHOD(UnixEchoFixture, "Unix: write string_view and read_some echo",
                 "[unix][socket][io][integration]") {
    run_client([this](boost::asio::io_context& ctx) -> awaitable<void> {
        unix_socket client("test", ctx);
        auto ec = co_await client.connect(socket_path, 5s);
        REQUIRE_FALSE(ec);

        auto written = co_await client.write("unix echo"sv);
        REQUIRE(written == 9);

        uint8_t buf[64];
        auto n = co_await client.read_some(buf, sizeof(buf));
        REQUIRE(n == 9);
        REQUIRE(std::string_view(reinterpret_cast<char*>(buf), n) == "unix echo");
    });
}

// #13: write(uint8_t*, size) + read(uint8_t*, size)
TEST_CASE_METHOD(UnixEchoFixture, "Unix: write and read exact bytes",
                 "[unix][socket][io][integration]") {
    run_client([this](boost::asio::io_context& ctx) -> awaitable<void> {
        unix_socket client("test", ctx);
        auto ec = co_await client.connect(socket_path, 5s);
        REQUIRE_FALSE(ec);

        const uint8_t data[] = {0xAA, 0xBB, 0xCC, 0xDD};
        auto written = co_await client.write(data, 4);
        REQUIRE(written == 4);

        uint8_t buf[4];
        auto n = co_await client.read(buf, 4);
        REQUIRE(n == 4);
        REQUIRE(std::memcmp(buf, data, 4) == 0);
    });
}

// #14: read_until(streambuf, delim)
TEST_CASE_METHOD(UnixEchoFixture, "Unix: read_until delimiter",
                 "[unix][socket][io][integration]") {
    run_client([this](boost::asio::io_context& ctx) -> awaitable<void> {
        unix_socket client("test", ctx);
        auto ec = co_await client.connect(socket_path, 5s);
        REQUIRE_FALSE(ec);

        co_await client.write("line1\nline2\n"sv);

        boost::asio::streambuf sb;
        auto n = co_await client.read_until(sb, "\n");
        REQUIRE(n > 0);

        auto begin = boost::asio::buffers_begin(sb.data());
        std::string line(begin, begin + static_cast<std::ptrdiff_t>(n));
        REQUIRE(line == "line1\n");
    });
}

// #15: cancel()
TEST_CASE_METHOD(UnixEchoFixture, "Unix: cancel pending read",
                 "[unix][socket][io][integration]") {
    run_client([this](boost::asio::io_context& ctx) -> awaitable<void> {
        unix_socket client("test", ctx);
        auto ec = co_await client.connect(socket_path, 5s);
        REQUIRE_FALSE(ec);

        // Schedule cancel after a short delay
        co_spawn(ctx, [&client]() -> awaitable<void> {
            boost::asio::steady_timer timer(client.get_io_context());
            timer.expires_after(100ms);
            auto [ec] = co_await timer.async_wait(use_nothrow_awaitable);
            if (!ec) client.cancel();
        }, detached);

        // read_some blocks; cancel will abort it
        uint8_t buf[64];
        auto n = co_await client.read_some(buf, sizeof(buf));
        REQUIRE(n == 0);
    });
}

// #16: available()
TEST_CASE_METHOD(UnixEchoFixture, "Unix: available bytes after echo",
                 "[unix][socket][io][integration]") {
    run_client([this](boost::asio::io_context& ctx) -> awaitable<void> {
        unix_socket client("test", ctx);
        auto ec = co_await client.connect(socket_path, 5s);
        REQUIRE_FALSE(ec);

        co_await client.write("data"sv);

        // Wait until data is available to read
        auto wait_ec = co_await client.wait(boost::asio::socket_base::wait_read);
        REQUIRE_FALSE(wait_ec);

        auto avail = client.available();
        REQUIRE(avail > 0);

        // Consume echoed data
        uint8_t buf[64];
        co_await client.read_some(buf, sizeof(buf));
    });
}

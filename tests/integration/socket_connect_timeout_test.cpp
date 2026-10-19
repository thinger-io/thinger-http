#include <catch2/catch_test_macros.hpp>
#include <thinger/asio/sockets/tcp_socket.hpp>
#include <thinger/asio/sockets/unix_socket.hpp>
#include <boost/asio.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

// The connect timeout of the TCP and Unix sockets: a connect completing right at the timeout
// (a timeout of 0, so the timer and the connect complete together) must leave nothing behind
// that runs once connect() returned, such as a timer cancelling the next operation on the
// socket or writing to the frame of the connect (run under ASan and TSan); a connect that
// does not complete in time ends with timed_out

using namespace thinger;
using namespace std::chrono_literals;

namespace {

    constexpr int ROUNDS = 200;

    struct round_counts {
        int connected = 0;
        int timed_out = 0;
    };

    // Accept every connection and answer it with one byte, keeping it open
    template<typename Acceptor, typename Socket>
    void accept_and_greet(Acceptor& acceptor, std::vector<std::shared_ptr<Socket>>& accepted) {
        auto socket = std::make_shared<Socket>(acceptor.get_executor());
        acceptor.async_accept(*socket, [&acceptor, &accepted, socket](boost::system::error_code ec) {
            if (ec) return;
            accepted.push_back(socket);
            boost::asio::async_write(*socket, boost::asio::buffer("y", 1), [](boost::system::error_code, size_t) {});
            accept_and_greet<Acceptor, Socket>(acceptor, accepted);
        });
    }

    // Connect `rounds` times with `connect`, and use each socket connected right away: the
    // greeting must be read, not cancelled by a timeout left running
    template<typename Socket, typename Connect>
    awaitable<void> connect_rounds(boost::asio::io_context& ioc, round_counts& counts, Connect connect) {
        for (int i = 0; i < ROUNDS; ++i) {
            auto socket = std::make_shared<Socket>("test", ioc);
            auto ec = co_await connect(*socket);
            if (ec == boost::asio::error::timed_out) {
                CHECK_FALSE(socket->is_open());
                ++counts.timed_out;
                continue;
            }
            REQUIRE_FALSE(ec);
            ++counts.connected;
            // give a stale timer completion the chance to run
            for (int j = 0; j < 3; ++j) co_await boost::asio::post(ioc, use_awaitable);
            uint8_t byte = 0;
            auto [read_ec, bytes] = co_await socket->read(&byte, 1);
            REQUIRE_FALSE(read_ec);
            REQUIRE(byte == 'y');
            auto [write_ec, written] = co_await socket->write(std::string_view("x"));
            REQUIRE_FALSE(write_ec);
        }
    }

    // Run `ioc` until `done`, at most for `limit`
    void run_until(boost::asio::io_context& ioc, const bool& done, std::chrono::seconds limit) {
        auto until = std::chrono::steady_clock::now() + limit;
        while (!done && std::chrono::steady_clock::now() < until) {
            ioc.run_for(100ms);
            if (ioc.stopped()) ioc.restart();
        }
    }

}

TEST_CASE("TCP connect completing at the timeout leaves nothing behind", "[socket][connect][timeout][integration]") {
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    std::vector<std::shared_ptr<boost::asio::ip::tcp::socket>> accepted;
    accept_and_greet(acceptor, accepted);
    // (named: connect() is a coroutine keeping references to its arguments)
    const std::string host = "127.0.0.1";
    const auto port = std::to_string(acceptor.local_endpoint().port());

    round_counts counts;
    bool done = false;
    co_spawn(ioc, [&]() -> awaitable<void> {
        co_await connect_rounds<asio::tcp_socket>(ioc, counts, [&](asio::tcp_socket& socket) {
            return socket.connect(host, port, 0s);
        });
        done = true;
    }, [](std::exception_ptr e) { if (e) std::rethrow_exception(e); });
    run_until(ioc, done, 60s);

    REQUIRE(done);
    CHECK(counts.connected + counts.timed_out == ROUNDS);
    INFO("connected " << counts.connected << ", timed out " << counts.timed_out);
    acceptor.close();
}

TEST_CASE("Unix connect completing at the timeout leaves nothing behind", "[socket][connect][timeout][integration]") {
    auto path = (std::filesystem::temp_directory_path() /
                 ("thinger_connect_timeout_" + std::to_string(::getpid()) + ".sock")).string();
    std::filesystem::remove(path);

    boost::asio::io_context ioc;
    boost::asio::local::stream_protocol::acceptor acceptor(ioc, boost::asio::local::stream_protocol::endpoint(path));
    std::vector<std::shared_ptr<boost::asio::local::stream_protocol::socket>> accepted;
    accept_and_greet(acceptor, accepted);

    round_counts counts;
    bool done = false;
    co_spawn(ioc, [&]() -> awaitable<void> {
        co_await connect_rounds<asio::unix_socket>(ioc, counts, [&](asio::unix_socket& socket) {
            return socket.connect(path, 0s);
        });
        done = true;
    }, [](std::exception_ptr e) { if (e) std::rethrow_exception(e); });
    run_until(ioc, done, 60s);

    REQUIRE(done);
    CHECK(counts.connected + counts.timed_out == ROUNDS);
    acceptor.close();
    std::filesystem::remove(path);
}

TEST_CASE("TCP connect not completing in time ends with timed_out", "[socket][connect][timeout][integration]") {
    // A listener that never accepts, with the smallest backlog: once its queue is full, the
    // system drops the connection requests, so a connect waits until the client gives up
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::acceptor acceptor(ioc);
    boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::make_address("127.0.0.1"), 0);
    acceptor.open(endpoint.protocol());
    acceptor.bind(endpoint);
    acceptor.listen(0);
    auto port = std::to_string(acceptor.local_endpoint().port());

    std::vector<std::shared_ptr<asio::tcp_socket>> sockets;
    std::optional<boost::system::error_code> timeout_result;
    std::chrono::steady_clock::duration elapsed{};
    bool done = false;
    co_spawn(ioc, [&]() -> awaitable<void> {
        for (int i = 0; i < 16 && !timeout_result; ++i) {
            auto socket = std::make_shared<asio::tcp_socket>("test", ioc);
            auto start = std::chrono::steady_clock::now();
            auto ec = co_await socket->connect("127.0.0.1", port, 1s);
            if (ec == boost::asio::error::timed_out) {
                elapsed = std::chrono::steady_clock::now() - start;
                timeout_result = ec;
                CHECK_FALSE(socket->is_open());
            } else {
                REQUIRE_FALSE(ec);
                sockets.push_back(socket);
            }
        }
        done = true;
    }, [](std::exception_ptr e) { if (e) std::rethrow_exception(e); });
    run_until(ioc, done, 60s);

    REQUIRE(done);
    REQUIRE(timeout_result);
    CHECK(*timeout_result == boost::asio::error::timed_out);
    CHECK(elapsed >= 900ms);
    CHECK(elapsed < 5s);

    // the sockets connected before are still usable
    for (auto& socket : sockets) CHECK(socket->is_open());
}

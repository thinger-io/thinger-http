#include <catch2/catch_test_macros.hpp>

#include <thinger/asio/listener.hpp>
#include <thinger/asio/sockets/socket.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <string>

#include "../fixtures/listener_helpers.hpp"
#include "../fixtures/transport_matrix.hpp"

using namespace thinger;
using namespace std::chrono_literals;
using thinger::http::test::delivered;
using thinger::http::test::io_thread;
using thinger::http::test::raw_connection;
using thinger::http::test::transport;
using thinger::http::test::transport_endpoint;

// socket::peer_closed() once the socket is readable: no error with data (left for the next
// read), eof for the end of the stream (a FIN; over TLS a close_notify, or a FIN without one),
// the error of the connection for a reset. On every transport, on the socket a listener accepts

namespace {

// A listener on `kind` handing its sockets to `sockets`, with a raw client connected to it
struct accepted_pair {
    io_thread io;
    delivered sockets;
    std::unique_ptr<http::test::temporary_directory> directory;
    std::unique_ptr<asio::listener> server;
    transport_endpoint endpoint;
    std::unique_ptr<raw_connection> client;

    explicit accepted_pair(transport kind) {
        endpoint.kind = kind;
        asio::listener_options options;
        if (kind == transport::unix) {
            directory = std::make_unique<http::test::temporary_directory>();
            endpoint.unix_path = (directory->path() / "s.sock").string();
            options.endpoint = asio::unix_endpoint{endpoint.unix_path};
        } else {
            options = http::test::tcp_options();
            if (kind == transport::tls) options.tls = http::test::matrix_certificates();
        }
        server = std::make_unique<asio::listener>(std::move(options), io.provider(), io.provider());
        REQUIRE_FALSE(server->open());
        server->start(sockets.handler());
        if (kind != transport::unix) endpoint.port = server->local_port();
        client = std::make_unique<raw_connection>(endpoint);
        REQUIRE(sockets.wait(1));
    }

    ~accepted_pair() {
        server->stop();
        std::shared_ptr<asio::socket> socket = accepted();
        boost::asio::post(io.io_context, [socket] { socket->close(); });
    }

    std::shared_ptr<asio::socket> accepted() {
        std::scoped_lock lock(sockets.mutex);
        return sockets.sockets.front();
    }

    // Run `coroutine` on the io_context of the accepted socket, and wait for its result
    template<typename T, typename Coroutine>
    T run(Coroutine coroutine) {
        std::promise<T> promise;
        auto future = promise.get_future();
        co_spawn(io.io_context, std::move(coroutine), [&promise](std::exception_ptr error, T value) {
            if (error) {
                promise.set_exception(error);
            } else {
                promise.set_value(std::move(value));
            }
        });
        REQUIRE(future.wait_for(5s) == std::future_status::ready);
        return future.get();
    }

    // Once the accepted socket is readable: what peer_closed() says
    boost::system::error_code wait_peer_closed() {
        auto socket = accepted();
        return run<boost::system::error_code>([socket]() -> awaitable<boost::system::error_code> {
            if (auto ec = co_await socket->wait(boost::asio::socket_base::wait_read)) co_return ec;
            co_return co_await socket->peer_closed();
        });
    }

    // What the accepted socket reads next
    std::string read_some() {
        auto socket = accepted();
        return run<std::string>([socket]() -> awaitable<std::string> {
            uint8_t buffer[64];
            auto [ec, bytes] = co_await socket->read_some(buffer, sizeof(buffer));
            co_return ec ? "error: " + ec.message() : std::string(reinterpret_cast<char*>(buffer), bytes);
        });
    }
};

} // namespace

TEST_CASE("peer_closed: data is no error, and stays for the next read", "[peer_closed][matrix]") {
    MATRIX_TRANSPORT(kind);
    accepted_pair pair(kind);

    REQUIRE(pair.client->write("hello"));
    auto ec = pair.wait_peer_closed();
    INFO("peer_closed: " << ec.message());
    CHECK_FALSE(ec);
    CHECK(pair.read_some() == "hello");
}

TEST_CASE("peer_closed: nothing to read yet is no error", "[peer_closed][matrix]") {
    MATRIX_TRANSPORT(kind);
    // Over TLS it waits for a record: only once the socket is readable (see wait())
    if (kind == transport::tls) SKIP("peer_closed() reads a TLS record");
    accepted_pair pair(kind);

    auto socket = pair.accepted();
    auto ec = pair.run<boost::system::error_code>([socket]() -> awaitable<boost::system::error_code> {
        co_return co_await socket->peer_closed();
    });
    INFO("peer_closed: " << ec.message());
    CHECK_FALSE(ec);
}

TEST_CASE("peer_closed: a FIN is eof", "[peer_closed][matrix]") {
    MATRIX_TRANSPORT(kind);
    accepted_pair pair(kind);

    // Over TLS, a TCP half-close without a close_notify
    pair.client->shutdown_send();
    auto ec = pair.wait_peer_closed();
    INFO("peer_closed: " << ec.message());
    CHECK(ec == boost::asio::error::eof);
}

TEST_CASE("peer_closed: a TLS close_notify is eof", "[peer_closed][tls]") {
    accepted_pair pair(transport::tls);

    pair.client->close_notify();
    auto ec = pair.wait_peer_closed();
    INFO("peer_closed: " << ec.message());
    CHECK(ec == boost::asio::error::eof);
}

TEST_CASE("peer_closed: a reset is its error", "[peer_closed][matrix]") {
    MATRIX_TRANSPORT(kind);
    if (kind == transport::unix) SKIP("a Unix socket has no reset");
    accepted_pair pair(kind);

    pair.client->reset();
    auto ec = pair.wait_peer_closed();
    INFO("peer_closed: " << ec.message());
    CHECK(ec);
    CHECK(ec != boost::asio::error::eof);
    CHECK(ec == boost::asio::error::connection_reset);
}

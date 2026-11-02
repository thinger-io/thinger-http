#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <thinger/asio/sockets/ssl_socket.hpp>
#include <thinger/asio/sockets/tcp_socket.hpp>
#include <thinger/asio/sockets/unix_socket.hpp>
#include <thinger/asio/sockets/websocket.hpp>
#include <thinger/http/client/executor_client.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include "../fixtures/hostile_peer.hpp"

// Every client operation that waits on the peer ends within its deadline when the peer is
// hostile (see hostile_peer.hpp): one case per operation, run on the transports and with the
// behaviours that can hold it. Each case asserts the outcome and when it arrives: no earlier
// than the deadline minus 100 ms, no later than the deadline plus the tolerance (1 s for
// deadlines in seconds). A case still pending well past it is cancelled, failing without
// hanging the suite.

using namespace thinger;
using namespace thinger::http::test;
using namespace std::chrono_literals;

namespace {

    using clock_type = std::chrono::steady_clock;

    constexpr auto EARLY = 100ms;
    constexpr auto TOLERANCE = 1s;

    // What run_bounded() saw: the result of the operation (none if it threw), whether it
    // completed before the limit, and when
    template<typename T>
    struct bounded_result {
        std::optional<T> value;
        bool in_time = false;
        clock_type::duration elapsed{};
    };

    // Run `operation` on `ioc`, measuring it. One still pending at `limit` is cancelled, and
    // the io_context runs until it completes: the case fails then, it does not hang. With
    // `cancel_after`, its caller cancels it then
    template<typename T>
    bounded_result<T> run_bounded(boost::asio::io_context& ioc, awaitable<T> operation,
                                  clock_type::duration limit = 5s,
                                  std::optional<clock_type::duration> cancel_after = std::nullopt) {
        bounded_result<T> result;
        boost::asio::cancellation_signal cancel;
        bool done = false;
        auto start = clock_type::now();
        boost::asio::steady_timer canceller(ioc);
        if (cancel_after) {
            canceller.expires_after(*cancel_after);
            canceller.async_wait([&](boost::system::error_code ec) {
                if (!ec && !done) cancel.emit(boost::asio::cancellation_type::terminal);
            });
        }
        co_spawn(ioc, std::move(operation), boost::asio::bind_cancellation_slot(cancel.slot(),
            [&](std::exception_ptr error, T value) {
                done = true;
                result.elapsed = clock_type::now() - start;
                if (!error) result.value = std::move(value);
            }));
        ioc.restart();
        auto until = start + limit;
        while (!done && clock_type::now() < until) {
            if (ioc.run_one_until(until) == 0 && ioc.stopped()) break;
        }
        result.in_time = done;
        if (!done) {
            cancel.emit(boost::asio::cancellation_type::terminal);
            ioc.restart();
            while (!done && ioc.run_one() != 0) {}
        }
        canceller.cancel();
        ioc.restart();
        ioc.poll();
        return result;
    }

    // The outcome arrived at the deadline: not before it (minus 100 ms), within the tolerance
    template<typename T>
    void check_ends_at_deadline(const bounded_result<T>& result, clock_type::duration deadline,
                                clock_type::duration tolerance = TOLERANCE) {
        INFO("elapsed " << std::chrono::duration_cast<std::chrono::milliseconds>(result.elapsed).count() << " ms");
        CHECK(result.in_time);
        CHECK(result.elapsed >= deadline - EARLY);
        CHECK(result.elapsed <= deadline + tolerance);
    }

    // Wait until `condition` holds, at most `limit`
    template<typename Condition>
    bool eventually(Condition condition, clock_type::duration limit = 2s) {
        auto until = clock_type::now() + limit;
        while (!condition()) {
            if (clock_type::now() >= until) return false;
            std::this_thread::sleep_for(10ms);
        }
        return true;
    }

    std::shared_ptr<boost::asio::ssl::context> client_tls_context() {
        auto context = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_client);
        auto ca = matrix_ca().pem();
        context->add_certificate_authority(boost::asio::buffer(ca));
        context->set_verify_mode(boost::asio::ssl::verify_peer);
        return context;
    }

}

// The root cause of a client hung for weeks: the handshake after the connect had no deadline,
// so a peer that accepted TCP and never answered the ClientHello held connect() for ever
TEST_CASE("A TLS connect to a hostile peer ends at its timeout", "[hostile_peer][tls][connect][integration]") {
    const auto behaviour = GENERATE(hostile_behaviour::silent, hostile_behaviour::trickle);
    INFO("behaviour: " << to_string(behaviour));
    hostile_peer peer(transport::tls, behaviour);
    // (named: connect() is a coroutine keeping references to its arguments)
    const auto host = peer.endpoint().host();
    const auto port = std::to_string(peer.endpoint().port);
    boost::asio::io_context ioc;

    SECTION("ssl_socket::connect") {
        auto socket = std::make_shared<asio::ssl_socket>("test", ioc, client_tls_context());
        auto result = run_bounded(ioc, socket->connect(host, port, 1s));
        REQUIRE(result.value);
        CHECK(*result.value == boost::asio::error::timed_out);
        check_ends_at_deadline(result, 1s);
        CHECK_FALSE(socket->is_open());
    }

    SECTION("asio::websocket::connect over an ssl_socket") {
        auto socket = std::make_shared<asio::ssl_socket>("test", ioc, client_tls_context());
        auto websocket = std::make_shared<asio::websocket>(socket, true, false);
        auto result = run_bounded(ioc, websocket->connect(host, port, 1s));
        REQUIRE(result.value);
        CHECK(*result.value == boost::asio::error::timed_out);
        check_ends_at_deadline(result, 1s);
        CHECK_FALSE(socket->is_open());
    }

    CHECK(peer.accepted() >= 1);
    if (behaviour == hostile_behaviour::trickle) CHECK(peer.trickled() >= 3);
    // the client closed the connection it gave up on
    CHECK(eventually([&] { return peer.closed() >= 1; }));
}

TEST_CASE("Cancelling a TLS connect to a silent peer ends it with operation_aborted", "[hostile_peer][tls][connect][integration]") {
    hostile_peer peer(transport::tls, hostile_behaviour::silent);
    const auto host = peer.endpoint().host();
    const auto port = std::to_string(peer.endpoint().port);
    boost::asio::io_context ioc;
    auto socket = std::make_shared<asio::ssl_socket>("test", ioc, client_tls_context());

    // cancelled by its caller while in the handshake, well before its own deadline
    auto result = run_bounded(ioc, socket->connect(host, port, 10s), 5s, 300ms);
    REQUIRE(result.value);
    CHECK(*result.value == boost::asio::error::operation_aborted);
    check_ends_at_deadline(result, 300ms, 500ms);
    CHECK_FALSE(socket->is_open());
    CHECK(eventually([&] { return peer.closed() >= 1; }));
}

// The connect row: a peer that never accepts leaves the connect pending until the client gives
// up (over TLS before any handshake)
TEST_CASE("A connect to a peer that never accepts ends at its timeout", "[hostile_peer][connect][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    pending_connect_peer peer(kind);
    if (!peer.pending()) SKIP("no connect can be left pending over " << to_string(kind) << ": " << peer.reason());
    const auto host = peer.endpoint().host();
    const auto port = std::to_string(peer.endpoint().port);
    const auto path = peer.endpoint().unix_path;
    boost::asio::io_context ioc;

    std::shared_ptr<asio::socket> socket;
    bounded_result<boost::system::error_code> result;
    if (kind == transport::unix) {
        auto unix_socket = std::make_shared<asio::unix_socket>("test", ioc);
        socket = unix_socket;
        result = run_bounded(ioc, unix_socket->connect(path, 1s));
    } else {
        auto tcp_socket = kind == transport::tls
            ? std::make_shared<asio::ssl_socket>("test", ioc, client_tls_context())
            : std::make_shared<asio::tcp_socket>("test", ioc);
        socket = tcp_socket;
        result = run_bounded(ioc, tcp_socket->connect(host, port, 1s));
    }
    REQUIRE(result.value);
    CHECK(*result.value == boost::asio::error::timed_out);
    check_ends_at_deadline(result, 1s);
    CHECK_FALSE(socket->is_open());
}

namespace {

    // A client of `peer` on `ioc` with a request timeout of 1 s
    std::unique_ptr<http::executor_client> client_of(boost::asio::io_context& ioc, const hostile_peer& peer) {
        auto client = std::make_unique<http::executor_client>(ioc);
        client->timeout(1s);
        peer.endpoint().configure(*client);
        return client;
    }

    // The request ended with timed_out at the request timeout of 1 s
    void check_request_timed_out(const bounded_result<http::client_response>& result) {
        REQUIRE(result.value);
        INFO("error: " << result.value->error_message());
        CHECK(result.value->error_code() == boost::asio::error::timed_out);
        check_ends_at_deadline(result, 1s);
    }

    // A reading peer saw the client close the connection it gave up on
    void check_closed(const hostile_peer& peer, hostile_behaviour behaviour) {
        CHECK(peer.started() >= 1);
        if (behaviour == hostile_behaviour::trickle) CHECK(peer.trickled() >= 2);
        if (behaviour != hostile_behaviour::no_read) CHECK(eventually([&] { return peer.closed() >= 1; }));
    }

}

// The HTTP client rows: the request timeout bounds the whole exchange, whatever the phase the
// peer stops in
TEST_CASE("An HTTP request through a hostile TLS handshake ends at the request timeout", "[hostile_peer][tls][client][integration]") {
    const auto behaviour = GENERATE(hostile_behaviour::silent, hostile_behaviour::trickle);
    INFO("behaviour: " << to_string(behaviour));
    hostile_peer peer(transport::tls, behaviour);
    boost::asio::io_context ioc;
    auto client = client_of(ioc, peer);
    const auto url = peer.url("/get");

    auto result = run_bounded(ioc, client->get(url));
    check_request_timed_out(result);
    check_closed(peer, behaviour);
}

TEST_CASE("An HTTP request whose body the peer does not read ends at the request timeout", "[hostile_peer][client][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    hostile_peer peer(kind, hostile_behaviour::no_read, hostile_start::after_tls_handshake);
    boost::asio::io_context ioc;
    auto client = client_of(ioc, peer);
    const auto url = peer.url("/upload");
    // far more than the socket buffers hold, on every platform
    std::string body(32 * 1024 * 1024, 'x');

    auto result = run_bounded(ioc, client->post(url, std::move(body), "text/plain"));
    check_request_timed_out(result);
    check_closed(peer, hostile_behaviour::no_read);
}

TEST_CASE("An HTTP response whose head does not arrive ends at the request timeout", "[hostile_peer][client][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    const auto behaviour = GENERATE(hostile_behaviour::silent, hostile_behaviour::trickle);
    INFO("behaviour: " << to_string(behaviour));
    hostile_peer peer(kind, behaviour, hostile_start::after_request_head);
    boost::asio::io_context ioc;
    auto client = client_of(ioc, peer);
    const auto url = peer.url("/get");

    auto result = run_bounded(ioc, client->get(url));
    check_request_timed_out(result);
    check_closed(peer, behaviour);
}

TEST_CASE("An HTTP response whose body does not arrive ends at the request timeout", "[hostile_peer][client][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    const auto behaviour = GENERATE(hostile_behaviour::silent, hostile_behaviour::trickle);
    INFO("behaviour: " << to_string(behaviour));
    hostile_peer peer(kind, behaviour, hostile_start::after_response_headers);
    boost::asio::io_context ioc;
    auto client = client_of(ioc, peer);
    const auto url = peer.url("/get");

    auto result = run_bounded(ioc, client->get(url));
    check_request_timed_out(result);
    check_closed(peer, behaviour);
}

TEST_CASE("A WebSocket upgrade the peer does not answer ends at the request timeout", "[hostile_peer][client][websocket][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    const auto behaviour = GENERATE(hostile_behaviour::silent, hostile_behaviour::trickle);
    INFO("behaviour: " << to_string(behaviour));
    hostile_peer peer(kind, behaviour, hostile_start::after_request_head);
    boost::asio::io_context ioc;
    auto client = client_of(ioc, peer);
    const auto url = peer.websocket_url("/ws");

    // no error code: no WebSocket is the outcome
    auto result = run_bounded(ioc, client->websocket(url));
    REQUIRE(result.value);
    CHECK_FALSE(result.value->has_value());
    check_ends_at_deadline(result, 1s);
    check_closed(peer, behaviour);
}

// The start point of the WebSocket rows: the peer answers the upgrade with a valid 101 first
TEST_CASE("A hostile peer upgrades to WebSocket before it misbehaves", "[hostile_peer][client][websocket][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    hostile_peer peer(kind, hostile_behaviour::silent, hostile_start::after_101);
    boost::asio::io_context ioc;
    auto client = client_of(ioc, peer);
    const auto url = peer.websocket_url("/ws");

    auto result = run_bounded(ioc, client->websocket(url));
    REQUIRE(result.value);
    REQUIRE(result.value->has_value());
    CHECK((*result.value)->is_open());
    CHECK(peer.started() == 1);
}

namespace {

    // The keepalive of the WebSocket rows, set through the setter of the client WebSocket: the
    // deadline of its reads and writes is the idle timeout (with a tolerance of 500 ms)
    constexpr auto PING_INTERVAL = 300ms;
    constexpr auto IDLE_TIMEOUT = 1s;
    constexpr auto IDLE_TOLERANCE = 500ms;

    // A client WebSocket upgraded with `peer` (which answers with a valid 101), its keepalive
    // the one of the rows
    http::websocket_client open_websocket(boost::asio::io_context& ioc, http::executor_client& client,
                                          const hostile_peer& peer) {
        const auto url = peer.websocket_url("/ws");
        // the keepalive set on the io_context, which runs only within run_bounded()
        auto connect = [&]() -> awaitable<std::optional<http::websocket_client>> {
            auto websocket = co_await client.websocket(url);
            if (websocket) websocket->set_keepalive(PING_INTERVAL, IDLE_TIMEOUT);
            co_return std::move(websocket);
        };
        auto result = run_bounded(ioc, connect());
        REQUIRE(result.value);
        REQUIRE(result.value->has_value());
        return std::move(**result.value);
    }

    // The documented outcome of a connection closed for being idle
    void check_idle_closed(const http::websocket_client& websocket) {
        CHECK_FALSE(websocket.is_open());
        CHECK(websocket.get_close_code() == asio::websocket::CLOSE_GOING_AWAY);
    }

}

// The WebSocket rows: after the upgrade, the keepalive bounds the reads and writes
TEST_CASE("A client WebSocket read from a silent peer ends at the idle timeout", "[hostile_peer][client][websocket][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    hostile_peer peer(kind, hostile_behaviour::silent, hostile_start::after_101);
    boost::asio::io_context ioc;
    auto client = client_of(ioc, peer);
    auto websocket = open_websocket(ioc, *client, peer);

    auto result = run_bounded(ioc, websocket.receive_async());
    REQUIRE(result.value);
    CHECK(result.value->first.empty());
    check_ends_at_deadline(result, IDLE_TIMEOUT, IDLE_TOLERANCE);
    check_idle_closed(websocket);
    CHECK(peer.started() == 1);
}

// A peer sending a byte within each idle timeout is alive, even if no message completes: the
// read stays pending while the bytes arrive, and ends at the idle timeout once they stop
TEST_CASE("A client WebSocket read from a trickling peer ends at the idle timeout once the trickle stops", "[hostile_peer][client][websocket][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    hostile_peer peer(kind, hostile_behaviour::trickle, hostile_start::after_101);
    boost::asio::io_context ioc;
    auto client = client_of(ioc, peer);
    auto websocket = open_websocket(ioc, *client, peer);

    // two idle timeouts of trickle, then silence
    constexpr auto TRICKLE_FOR = 2 * IDLE_TIMEOUT;
    boost::asio::steady_timer stopper(ioc, TRICKLE_FOR);
    stopper.async_wait([&](boost::system::error_code ec) {
        if (!ec) peer.stop_trickle();
    });
    auto result = run_bounded(ioc, websocket.receive_async());
    REQUIRE(result.value);
    CHECK(result.value->first.empty());
    INFO("elapsed " << std::chrono::duration_cast<std::chrono::milliseconds>(result.elapsed).count() << " ms");
    CHECK(result.in_time);
    CHECK(result.elapsed >= TRICKLE_FOR);
    CHECK(result.elapsed <= TRICKLE_FOR + IDLE_TIMEOUT + IDLE_TOLERANCE);
    check_idle_closed(websocket);
    CHECK(peer.trickled() >= 8);
}

TEST_CASE("A client WebSocket write the peer does not read ends at the idle timeout", "[hostile_peer][client][websocket][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    hostile_peer peer(kind, hostile_behaviour::no_read, hostile_start::after_101);
    boost::asio::io_context ioc;
    auto client = client_of(ioc, peer);
    auto websocket = open_websocket(ioc, *client, peer);
    // far more than the socket buffers hold, on every platform
    std::string message(32 * 1024 * 1024, 'x');

    auto result = run_bounded(ioc, websocket.send_text_async(std::move(message)));
    REQUIRE(result.value);
    CHECK_FALSE(*result.value);
    check_ends_at_deadline(result, IDLE_TIMEOUT, IDLE_TOLERANCE);
    check_idle_closed(websocket);
}

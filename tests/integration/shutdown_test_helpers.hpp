#ifndef THINGER_TESTS_SHUTDOWN_TEST_HELPERS_HPP
#define THINGER_TESTS_SHUTDOWN_TEST_HELPERS_HPP

#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/http_application.hpp>
#include <thinger/http/server/response.hpp>
#include "server_test_helpers.hpp"

#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <thread>

// Helpers of the shutdown tests: raw clients on any transport whose reads never block longer
// than a timeout, whether a client can connect, and handlers answering late
namespace shutdown_test {

    using namespace std::chrono_literals;
    using clock = std::chrono::steady_clock;

    // Whether a client can connect (over TLS, without the handshake)
    inline bool can_connect(const server_test::transport_endpoint& to) {
        boost::asio::io_context ioc;
        boost::system::error_code ec;
        if (to.kind == server_test::transport::unix) {
            boost::asio::local::stream_protocol::socket sock(ioc);
            sock.connect(boost::asio::local::stream_protocol::endpoint(to.unix_path), ec);
            return !ec;
        }
        boost::asio::ip::tcp::socket sock(ioc);
        sock.connect({boost::asio::ip::make_address("127.0.0.1"), to.port}, ec);
        return !ec;
    }

    inline bool can_connect(uint16_t port) {
        return can_connect(server_test::transport_endpoint{server_test::transport::tcp, port, {}});
    }

    // Whether `condition` holds within the timeout
    inline bool eventually(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 3s) {
        auto deadline = clock::now() + timeout;
        while (clock::now() < deadline) {
            if (condition()) return true;
            std::this_thread::sleep_for(5ms);
        }
        return condition();
    }

    // A raw HTTP/1.1 client connection, on any transport: every read waits up to a timeout
    struct raw_client {
        thinger::http::test::raw_connection connection;
        std::string received;

        explicit raw_client(const server_test::transport_endpoint& to) : connection(to) {}

        explicit raw_client(uint16_t port)
            : raw_client(server_test::transport_endpoint{server_test::transport::tcp, port, {}}) {}

        void send(const std::string& data) {
            REQUIRE(connection.write(data));
        }

        void get(const std::string& path) {
            send("GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
        }

        // Read until `received` holds `needle`
        bool read_until(const std::string& needle, std::chrono::milliseconds timeout = 3s) {
            auto deadline = clock::now() + timeout;
            while (received.find(needle) == std::string::npos) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now());
                if (connection.eof() || left <= 0ms) return false;
                connection.read_some(received, left);
            }
            return true;
        }

        // What arrives until the server closes the connection (or the timeout expires)
        server_test::exchange_result rest(std::chrono::milliseconds timeout = 3s) {
            return connection.read_until_closed(timeout);
        }
    };

    // A handler answering after `delay`
    inline void slow_route(thinger::http::http_application& app, const std::string& path,
                           std::chrono::milliseconds delay, std::promise<void>* started = nullptr) {
        app.get(path, [delay, started](thinger::http::response& res) -> thinger::awaitable<void> {
            if (started) started->set_value();
            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, delay);
            co_await timer.async_wait(thinger::use_nothrow_awaitable);
            res.send("done");
        });
    }

}

#endif

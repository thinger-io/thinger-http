#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/pool_server.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>

#include "server_test_helpers.hpp"
#include "../fixtures/stream_test_helpers.hpp"

#include <atomic>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

// on_close() of the server WebSocket (websocket_connection) and SSE (sse_connection)
// connections: called once, on the thread of the connection, whatever closed it

using namespace thinger;
using namespace std::chrono_literals;
using namespace stream_test;
using server_test::running_server;

namespace {

    using ws_ptr = std::shared_ptr<http::websocket_connection>;
    using sse_ptr = std::shared_ptr<http::sse_connection>;

    template<typename F>
    bool eventually(F condition, std::chrono::milliseconds timeout = 5s) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!condition()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    // What on_close() told, and how many times, from the thread of the connection
    struct close_record {
        std::mutex mutex;
        std::atomic<int> calls{0};
        std::optional<http::websocket_close> status;
        std::thread::id connection_thread;
        std::thread::id close_thread;

        void opened() {
            std::scoped_lock lock(mutex);
            connection_thread = std::this_thread::get_id();
        }

        void closed(const http::websocket_close& close = {}) {
            std::scoped_lock lock(mutex);
            status = close;
            close_thread = std::this_thread::get_id();
            ++calls;
        }

        // Called once, on the thread of the connection, with what it returns
        http::websocket_close wait() {
            REQUIRE(eventually([&] { return calls.load() > 0; }));
            // a second call would come right after the first one
            std::this_thread::sleep_for(100ms);
            REQUIRE(calls.load() == 1);
            std::scoped_lock lock(mutex);
            REQUIRE(close_thread == connection_thread);
            return *status;
        }
    };

    // A WebSocket route recording on_close(), after `setup`
    void websocket_route(http::http_application& app, const std::shared_ptr<close_record>& record,
                         std::function<void(const ws_ptr&)> setup = {}) {
        app.get("/ws", [record, setup](http::response& res) {
            res.upgrade_websocket([record, setup](ws_ptr ws) {
                record->opened();
                ws->on_message([](std::string, bool) {});
                ws->on_close([record](const http::websocket_close& close) { record->closed(close); });
                if (setup) setup(ws);
            });
        });
    }

    // Read up to the close frame of the server, and answer it
    void answer_close(raw_ws_client& client, uint16_t expected) {
        std::optional<ws_frame> frame;
        while ((frame = client.read_frame(3s)) && frame->opcode != OP_CLOSE) {}
        REQUIRE(frame);
        REQUIRE(frame->close_code() == expected);
        client.send_frame(OP_CLOSE, close_payload(expected));
    }

    // Raw SSE client: sends the request and reads the response headers
    struct sse_client {
        net::io_context ioc;
        tcp::socket sock{ioc};

        explicit sse_client(uint16_t port, const std::string& path = "/events") {
            sock.connect({net::ip::make_address("127.0.0.1"), port});
            net::write(sock, net::buffer("GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n"));
        }

        // What arrives until `needle`, the server closes the connection, or the timeout expires
        server_test::exchange_result read_until(const std::string& needle, std::chrono::milliseconds timeout = 3s) {
            server_test::exchange_result result;
            std::array<char, 4096> buffer{};
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (result.data.find(needle) == std::string::npos && !result.closed) {
                bool done = false;
                sock.async_read_some(net::buffer(buffer), [&](boost::system::error_code ec, size_t bytes) {
                    result.data.append(buffer.data(), bytes);
                    if (ec) result.closed = true;
                    done = true;
                });
                ioc.restart();
                ioc.run_until(deadline);
                if (!done) {
                    sock.cancel();
                    ioc.restart();
                    ioc.run();
                    break;
                }
            }
            return result;
        }

        void close() {
            boost::system::error_code ec;
            sock.shutdown(tcp::socket::shutdown_both, ec);
            sock.close(ec);
        }
    };

    // An SSE route recording on_close(), after `setup`
    void sse_route(http::http_application& app, const std::shared_ptr<close_record>& record,
                   std::function<void(const sse_ptr&)> setup = {}) {
        app.get("/events", [record, setup](http::response& res) {
            res.start_sse([record, setup](sse_ptr sse) {
                record->opened();
                sse->on_close([record] { record->closed(); });
                sse->send_data("hello");
                if (setup) setup(sse);
            });
        });
    }

}

TEST_CASE("WebSocket on_close tells how it closed", "[websocket][close][integration]") {
    http::server server;
    auto record = std::make_shared<close_record>();
    std::function<void(const ws_ptr&)> setup;
    websocket_route(server, record, [&](const ws_ptr& ws) { if (setup) setup(ws); });
    running_server running(server);
    raw_ws_client client(running.port);

    SECTION("The peer closes, with a code and a reason") {
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        client.send_frame(OP_CLOSE, close_payload(4000, "bye"));
        auto status = record->wait();
        REQUIRE(status.code == 4000);
        REQUIRE(status.reason == "bye");
        REQUIRE(status.remote);
        // answered with the same code
        auto frame = client.read_frame(3s);
        REQUIRE(frame);
        REQUIRE(frame->close_code() == 4000);
    }

    SECTION("The peer closes without a status code") {
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        client.send_frame(OP_CLOSE, "");
        auto status = record->wait();
        REQUIRE(status.code == 1005);
        REQUIRE(status.reason.empty());
        REQUIRE(status.remote);
    }

    SECTION("The connection is lost") {
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        client.close();
        auto status = record->wait();
        REQUIRE(status.code == 1006);
        REQUIRE_FALSE(status.remote);
    }

    SECTION("stop()") {
        setup = [](const ws_ptr& ws) { ws->stop(); };
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        answer_close(client, 1000);
        auto status = record->wait();
        REQUIRE(status.code == 1000);
        REQUIRE_FALSE(status.remote);
    }

    SECTION("A message that is not valid") {
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        client.send_frame(OP_TEXT, "\xC0\xAF");
        auto status = record->wait();
        REQUIRE(status.code == 1007);
        REQUIRE_FALSE(status.remote);
    }

    SECTION("The overflow policy") {
        setup = [](const ws_ptr& ws) {
            ws->set_write_limits({.max_messages = 2, .policy = http::overflow_policy::close});
            for (int i = 0; i < 5; ++i) ws->send_binary(std::string(100, 'x'));
        };
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        auto status = record->wait();
        REQUIRE(status.code == 1013);
        REQUIRE_FALSE(status.remote);
    }
}

TEST_CASE("WebSocket on_close after on_message throws", "[websocket][close][integration]") {
    http::server server;
    auto record = std::make_shared<close_record>();
    websocket_route(server, record, [](const ws_ptr& ws) {
        ws->on_message([](std::string, bool) { throw std::runtime_error("bad message"); });
    });
    running_server running(server);
    raw_ws_client client(running.port);
    REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));

    client.send_frame(OP_TEXT, "hello");
    // closed with 1011, instead of left open without reading
    answer_close(client, 1011);
    auto status = record->wait();
    REQUIRE(status.code == 1011);
    REQUIRE_FALSE(status.remote);
}

TEST_CASE("WebSocket on_close is called once", "[websocket][close][integration]") {
    http::server server;
    auto record = std::make_shared<close_record>();
    std::promise<std::weak_ptr<http::websocket_connection>> created;
    websocket_route(server, record, [&](const ws_ptr& ws) { created.set_value(ws); });
    running_server running(server);
    raw_ws_client client(running.port);
    REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
    auto weak = created.get_future().get();

    // Several causes at once: the peer, stop() from another thread, and the connection lost
    client.send_frame(OP_CLOSE, close_payload(1000));
    if (auto ws = weak.lock()) {
        ws->stop();
        ws->stop();
    }
    client.close();
    record->wait();
    // and released afterwards
    REQUIRE(eventually([&] { return weak.expired(); }));
}

TEST_CASE("WebSocket on_close on a server shutdown", "[websocket][close][shutdown][integration]") {
    auto record = std::make_shared<close_record>();

    SECTION("Closed by default with 1001") {
        http::server server;
        websocket_route(server, record);
        running_server running(server);
        raw_ws_client client(running.port);
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        REQUIRE(eventually([&] { return server.stats().websocket_connections == 1; }));

        server.begin_shutdown(5s);
        answer_close(client, 1001);
        auto status = record->wait();
        REQUIRE(status.code == 1001);
        REQUIRE_FALSE(status.remote);
    }

    SECTION("Closed by the server at the end of the grace period") {
        // A pool server: its workers still run once it stopped
        http::pool_server server;
        websocket_route(server, record, [](const ws_ptr& ws) { ws->on_shutdown([] {}); });
        REQUIRE(server.listen("127.0.0.1", 0));
        auto waiting = std::async(std::launch::async, [&] { server.wait(); });
        raw_ws_client client(server.local_port());
        REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));
        REQUIRE(eventually([&] { return server.stats().websocket_connections == 1; }));

        REQUIRE_FALSE(server.shutdown(200ms));
        auto status = record->wait();
        REQUIRE(status.code == 1006);
        REQUIRE_FALSE(status.remote);
        REQUIRE(waiting.wait_for(3s) == std::future_status::ready);
    }
}

TEST_CASE("WebSocket on_close is not called once released", "[websocket][close][integration]") {
    http::server server;
    auto record = std::make_shared<close_record>();
    std::promise<std::shared_ptr<asio::socket>> released;
    websocket_route(server, record, [&](const ws_ptr& ws) {
        ws->on_message([&, weak = std::weak_ptr<http::websocket_connection>(ws)](std::string, bool) {
            if (auto self = weak.lock()) released.set_value(self->release_socket());
        });
    });
    running_server running(server);
    raw_ws_client client(running.port);
    REQUIRE(client.handshake("/ws").starts_with("HTTP/1.1 101"));

    client.send_frame(OP_TEXT, "release");
    auto future = released.get_future();
    REQUIRE(future.wait_for(3s) == std::future_status::ready);
    auto socket = future.get();
    REQUIRE(socket);
    client.close();
    std::this_thread::sleep_for(300ms);
    REQUIRE(record->calls.load() == 0);
    // released on its thread
    auto& io_context = socket->get_io_context();
    boost::asio::post(io_context, [socket = std::move(socket)] {});
}

TEST_CASE("SSE on_close tells the client left", "[sse][close][integration]") {
    http::server server;
    auto record = std::make_shared<close_record>();
    std::function<void(const sse_ptr&)> setup;
    sse_route(server, record, [&](const sse_ptr& sse) { if (setup) setup(sse); });
    running_server running(server);

    SECTION("The client leaves, while nothing is sent") {
        sse_client client(running.port);
        REQUIRE(client.read_until("data: hello").data.find("data: hello") != std::string::npos);
        client.close();
        // at once, not when the next event fails or the idle timer expires
        record->wait();
        REQUIRE(eventually([&] { return server.stats().sse_connections == 0; }));
    }

    SECTION("stop()") {
        setup = [](const sse_ptr& sse) { sse->stop(); };
        sse_client client(running.port);
        REQUIRE(client.read_until("never").closed);
        record->wait();
    }

    SECTION("The overflow policy") {
        setup = [](const sse_ptr& sse) {
            sse->set_write_limits({.max_messages = 2, .policy = http::overflow_policy::close});
            for (int i = 0; i < 10; ++i) sse->send_data(std::string(100, 'x'));
        };
        sse_client client(running.port);
        REQUIRE(client.read_until("never").closed);
        record->wait();
    }

    SECTION("Called once") {
        std::promise<std::weak_ptr<http::sse_connection>> created;
        setup = [&](const sse_ptr& sse) { created.set_value(sse); };
        sse_client client(running.port);
        auto weak = created.get_future().get();
        if (auto sse = weak.lock()) {
            sse->stop();
            sse->stop();
        }
        client.close();
        record->wait();
        REQUIRE(eventually([&] { return weak.expired(); }));
    }
}

TEST_CASE("SSE on_close on a server shutdown", "[sse][close][shutdown][integration]") {
    http::server server;
    auto record = std::make_shared<close_record>();
    sse_route(server, record);
    running_server running(server);
    sse_client client(running.port);
    REQUIRE(client.read_until("data: hello").data.find("data: hello") != std::string::npos);
    REQUIRE(eventually([&] { return server.stats().sse_connections == 1; }));

    server.begin_shutdown(5s);
    record->wait();
    REQUIRE(client.read_until("never").closed);
}

TEST_CASE("SSE on_close set once closed is called at once", "[sse][close][integration]") {
    http::server server;
    std::atomic<bool> left{false};
    std::promise<bool> called_at_once;
    server.get("/events", [&](http::response& res) {
        res.start_sse([&](sse_ptr sse) -> awaitable<void> {
            // a coroutine handler running late: the client left meanwhile
            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
            while (!left) {
                timer.expires_after(5ms);
                co_await timer.async_wait(use_nothrow_awaitable);
            }
            timer.expires_after(200ms);
            co_await timer.async_wait(use_nothrow_awaitable);
            bool called = false;
            sse->on_close([&called] { called = true; });
            called_at_once.set_value(called);
        });
    });
    running_server running(server);
    sse_client client(running.port);
    REQUIRE(eventually([&] { return server.stats().sse_connections == 1; }));
    client.close();
    left = true;
    auto future = called_at_once.get_future();
    REQUIRE(future.wait_for(3s) == std::future_status::ready);
    REQUIRE(future.get());
}

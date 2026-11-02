#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <thinger/http/client/websocket_client.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/client/async_client.hpp>
#include <thinger/http/client/executor_client.hpp>
#include <thinger/asio/io_context_of.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/websocket_connection.hpp>
#include <algorithm>
#include <thread>
#include <chrono>
#include <future>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thinger/util/base64.hpp>
#include <thinger/util/sha1.hpp>
#include "../fixtures/hostile_peer.hpp"
#include "../fixtures/running_workers.hpp"
#include "../fixtures/server_listen.hpp"

using namespace thinger;
using namespace std::chrono_literals;

namespace {

// Period of the messages of /ws/ticks: longer than the idle timeout of the keepalive tests
constexpr auto TICK_PERIOD = 1500ms;

// WebSocket test server fixture
struct WebSocketServerFixture {
    http::server server;
    uint16_t port = 0;
    std::string ws_url;
    std::thread server_thread;

    WebSocketServerFixture() {
        setup_websocket_endpoints();
        start_server();
    }

    ~WebSocketServerFixture() {
        server.stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

private:
    void setup_websocket_endpoints() {
        // Echo WebSocket endpoint - echoes back any message received
        // Note: ws->start() is called automatically by the server after the handler
        server.get("/ws/echo", [](http::request& req, http::response& res) {
            res.upgrade_websocket({}, [](std::shared_ptr<http::websocket_connection> ws) {
                ws->on_message([ws](std::string message, bool binary) {
                    // Echo back the message with same type
                    if (binary) {
                        ws->send_binary(std::move(message));
                    } else {
                        ws->send_text(std::move(message));
                    }
                });
            });
        });

        // Receives and drops the messages: answers only the pings (as any server reads)
        server.get("/ws/sink", [](http::request& req, http::response& res) {
            res.upgrade_websocket({}, [](std::shared_ptr<http::websocket_connection> ws) {
                ws->on_message([](std::string, bool) {});
            });
        });

        // Sends "tick" every TICK_PERIOD, a few times
        server.get("/ws/ticks", [](http::request& req, http::response& res) {
            res.upgrade_websocket({}, [](std::shared_ptr<http::websocket_connection> ws) {
                co_spawn(ws->get_executor(), [weak = std::weak_ptr(ws)]() -> awaitable<void> {
                    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
                    for (int i = 0; i < 10; ++i) {
                        timer.expires_after(TICK_PERIOD);
                        auto [ec] = co_await timer.async_wait(use_nothrow_awaitable);
                        auto ws = weak.lock();
                        if (ec || !ws) co_return;
                        ws->send_text("tick");
                    }
                }, boost::asio::detached);
            });
        });

        server.get("/hello", [](http::request& req, http::response& res) { res.send("hello"); });

        // WebSocket endpoint that sends a welcome message on connect
        server.get("/ws/welcome", [](http::request& req, http::response& res) {
            res.upgrade_websocket({}, [](std::shared_ptr<http::websocket_connection> ws) {
                ws->on_message([ws](std::string message, bool binary) {
                    ws->send_text("received: " + message);
                });
                // Send welcome message - this will be queued and sent after start()
                ws->send_text("welcome");
            });
        });
    }

    void start_server() {
        REQUIRE_FALSE(server.listen("127.0.0.1", 0));
        port = http::test::listening_port(server);
        ws_url = "ws://127.0.0.1:" + std::to_string(port);

        std::promise<void> ready;
        server_thread = std::thread([this, &ready]() {
            ready.set_value();
            server.wait();
        });
        ready.get_future().wait();
    }
};

} // anonymous namespace

TEST_CASE("HTTP Client WebSocket Integration", "[websocket][client][integration]") {
    WebSocketServerFixture fixture;

    SECTION("Connect via client.websocket() - sync") {
        http::client client;

        auto ws = client.websocket(fixture.ws_url + "/ws/echo");

        REQUIRE(ws.has_value());
        REQUIRE(ws->is_open());

        ws->close();
    }

    SECTION("Send and receive via client.websocket()") {
        http::client client;

        auto ws = client.websocket(fixture.ws_url + "/ws/echo");
        REQUIRE(ws.has_value());

        // Send a text message
        REQUIRE(ws->send_text("Hello from client.websocket()!"));

        // Receive echo
        auto [message, is_binary] = ws->receive();

        REQUIRE(message == "Hello from client.websocket()!");
        REQUIRE_FALSE(is_binary);

        ws->close();
    }

    SECTION("Send binary via client.websocket()") {
        http::client client;

        auto ws = client.websocket(fixture.ws_url + "/ws/echo");
        REQUIRE(ws.has_value());

        // Send binary data
        std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
        REQUIRE(ws->send_binary(data.data(), data.size()));

        // Receive echo
        auto [message, is_binary] = ws->receive();

        REQUIRE(message.size() == data.size());
        REQUIRE(is_binary);

        ws->close();
    }

    SECTION("Invalid URL returns nullopt") {
        http::client client;

        auto ws = client.websocket("ws://invalid.host.test:9999/ws");

        REQUIRE_FALSE(ws.has_value());
    }

    SECTION("Multiple messages via client.websocket()") {
        http::client client;

        auto ws = client.websocket(fixture.ws_url + "/ws/echo");
        REQUIRE(ws.has_value());

        for (int i = 0; i < 3; ++i) {
            std::string msg = "Msg" + std::to_string(i);
            REQUIRE(ws->send_text(msg));
            auto [response, _] = ws->receive();
            REQUIRE(response == msg);
        }

        ws->close();
    }

    SECTION("Server closes connection on buffer overflow") {
        http::client client;

        auto ws = client.websocket(fixture.ws_url + "/ws/echo");
        REQUIRE(ws.has_value());
        REQUIRE(ws->is_open());

        // Create message larger than websocket_connection::MAX_BUFFER_SIZE (16MB)
        std::string large_message(17 * 1024 * 1024, 'X');

        // Send oversized message - may fail mid-write when server detects overflow
        ws->send_text(large_message);

        // Server should have closed the connection after detecting buffer overflow
        // receive() returns empty when the server-side connection is gone
        auto [message, is_binary] = ws->receive();
        REQUIRE(message.empty());

        ws->close();
    }

    SECTION("Valid UTF-8 text is echoed back") {
        http::client client;

        auto ws = client.websocket(fixture.ws_url + "/ws/echo");
        REQUIRE(ws.has_value());

        // ASCII
        REQUIRE(ws->send_text("Hello World"));
        auto [msg1, _1] = ws->receive();
        REQUIRE(msg1 == "Hello World");

        // Multi-byte: 2-byte (Latin), 3-byte (CJK), 4-byte (emoji)
        std::string utf8_multi = "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xF0\x9F\x98\x80";
        REQUIRE(ws->send_text(utf8_multi));
        auto [msg2, _2] = ws->receive();
        REQUIRE(msg2 == utf8_multi);

        ws->close();
    }

    SECTION("Invalid UTF-8 text causes server to close connection") {
        http::client client;

        auto ws = client.websocket(fixture.ws_url + "/ws/echo");
        REQUIRE(ws.has_value());
        REQUIRE(ws->is_open());

        // Send invalid UTF-8: 0xC0 is never valid as a leading byte
        std::string invalid_utf8 = "hello\xC0\x80world";
        ws->send_text(invalid_utf8);

        // Server should reject the invalid UTF-8 and close the connection
        auto [message, is_binary] = ws->receive();
        REQUIRE(message.empty());

        ws->close();
    }

    SECTION("Binary frames skip UTF-8 validation") {
        http::client client;

        auto ws = client.websocket(fixture.ws_url + "/ws/echo");
        REQUIRE(ws.has_value());

        // Send bytes that would be invalid UTF-8, but as binary frame
        std::vector<uint8_t> data = {0xC0, 0x80, 0xFF, 0xFE};
        REQUIRE(ws->send_binary(data.data(), data.size()));

        // Should be echoed back without validation
        auto [message, is_binary] = ws->receive();
        REQUIRE(message.size() == data.size());
        REQUIRE(is_binary);

        ws->close();
    }

    SECTION("Same client can do HTTP and WebSocket") {
        http::client client;

        // First do a regular HTTP request (would need an HTTP endpoint)
        // For now, just test WebSocket works
        auto ws = client.websocket(fixture.ws_url + "/ws/welcome");
        REQUIRE(ws.has_value());

        // Receive welcome message
        auto [message, _] = ws->receive();
        REQUIRE(message == "welcome");

        ws->close();
    }
}

TEST_CASE("Async Client WebSocket Integration", "[websocket][client][integration]") {
    thinger::http::test::running_workers workers;
    WebSocketServerFixture fixture;

    SECTION("Connect via async_client.websocket() - async") {
        http::async_client client(workers);

        std::atomic<bool> connected{false};
        std::atomic<bool> callback_called{false};

        client.websocket(fixture.ws_url + "/ws/echo", [&](std::shared_ptr<http::websocket_client> ws) {
            callback_called = true;
            if (ws && ws->is_open()) {
                connected = true;
                ws->stop();   // the close of a callback, which cannot wait
            }
        });

        client.wait();

        REQUIRE(callback_called);
        REQUIRE(connected);
    }

    SECTION("Send/receive via async_client.websocket()") {
        http::async_client client(workers);

        std::string received_message;
        std::atomic<bool> done{false};

        // a coroutine callback: the coroutine API, as the synchronous one cannot wait there
        client.websocket(fixture.ws_url + "/ws/echo", [&](std::shared_ptr<http::websocket_client> ws) -> awaitable<void> {
            if (ws && ws->is_open()) {
                co_await ws->send_text_async("Hello async!");
                auto [msg, _] = co_await ws->receive_async();
                received_message = msg;
                co_await ws->close_async();
            }
            done = true;
        });

        client.wait();

        REQUIRE(done);
        REQUIRE(received_message == "Hello async!");
    }

    SECTION("Invalid URL calls callback with nullptr") {
        http::async_client client(workers);

        std::atomic<bool> callback_called{false};
        std::shared_ptr<http::websocket_client> received_ws;

        client.websocket("ws://invalid.host.test:9999/ws", [&](std::shared_ptr<http::websocket_client> ws) {
            callback_called = true;
            received_ws = ws;
        });

        client.wait();

        REQUIRE(callback_called);
        REQUIRE_FALSE(received_ws);  // Should be nullptr
    }
}

TEST_CASE("WebSocket client keeps the frames received along with the handshake response",
          "[websocket][client][integration]") {
    // A server that sends a frame right after its 101 response, in the same write: the
    // client reads both at once
    boost::asio::io_context server_context;
    boost::asio::ip::tcp::acceptor acceptor(server_context,
        {boost::asio::ip::make_address("127.0.0.1"), 0});
    auto port = acceptor.local_endpoint().port();

    std::thread server_thread([&acceptor, &server_context]() {
        boost::asio::ip::tcp::socket socket(server_context);
        acceptor.accept(socket);

        boost::asio::streambuf request;
        boost::asio::read_until(socket, request, "\r\n\r\n");
        std::string text(boost::asio::buffers_begin(request.data()), boost::asio::buffers_end(request.data()));
        const std::string key_header = "Sec-WebSocket-Key: ";
        auto key_start = text.find(key_header) + key_header.size();
        auto key = text.substr(key_start, text.find("\r\n", key_start) - key_start);
        auto accept = util::base64::encode(util::sha1::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));

        std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                               "Upgrade: websocket\r\n"
                               "Connection: Upgrade\r\n"
                               "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        response += std::string("\x81\x07" "welcome", 9);   // final text frame, unmasked
        boost::asio::write(socket, boost::asio::buffer(response));

        // Wait for the close frame of the client, then close
        uint8_t data[64];
        boost::system::error_code ec;
        socket.read_some(boost::asio::buffer(data), ec);
        socket.close();
    });

    {
        http::client client;
        auto ws = client.websocket("ws://127.0.0.1:" + std::to_string(port) + "/");
        REQUIRE(ws.has_value());

        auto [message, binary] = ws->receive();
        REQUIRE(message == "welcome");
        REQUIRE_FALSE(binary);

        ws->close();
    }
    server_thread.join();
}

namespace {

    using clock_type = std::chrono::steady_clock;

    // The keepalive of the timing cases: a ping after 300 ms without receiving, closed after 1 s
    constexpr auto PING_INTERVAL = 300ms;
    constexpr auto IDLE_TIMEOUT = 1s;
    // At least 3 idle timeouts
    constexpr auto USE_FOR = 3500ms;
    // A blocking call returns once its own operation completes
    constexpr auto PROMPT = 500ms;

    // A client WebSocket of an executor_client running on `ioc`, on this thread. Awaited inside a
    // coroutine: the awaitable of websocket() refers to its arguments (the default subprotocol is a
    // temporary), so it is not handed to co_spawn by itself
    http::websocket_client open_on(boost::asio::io_context& ioc, http::executor_client& client, const std::string& url) {
        auto connect = [&]() -> awaitable<std::optional<http::websocket_client>> {
            co_return co_await client.websocket(url);
        };
        auto future = co_spawn(ioc, connect, boost::asio::use_future);
        asio::run_until(ioc, [&] { return future.wait_for(0s) == std::future_status::ready; });
        auto websocket = future.get();
        REQUIRE(websocket.has_value());
        return std::move(*websocket);
    }

    std::chrono::milliseconds elapsed_since(clock_type::time_point start) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - start);
    }

}

// Every client WebSocket has a deadline on its reads and writes by default: the keepalive of
// the server (asio::websocket::start_timeout())
TEST_CASE("Client WebSocket keepalive: the default and the setter", "[websocket][client][keepalive][integration]") {
    WebSocketServerFixture fixture;
    const auto url = fixture.ws_url + "/ws/echo";
    constexpr auto PING = std::chrono::milliseconds(asio::websocket::CONNECTION_TIMEOUT_SECONDS);
    constexpr auto IDLE = 2 * PING;
    STATIC_REQUIRE(PING == 60s);

    SECTION("60 s and 120 s from the synchronous client") {
        http::client client;
        auto ws = client.websocket(url);
        REQUIRE(ws.has_value());
        CHECK(ws->get_ping_interval() == PING);
        CHECK(ws->get_idle_timeout() == IDLE);
        ws->close();
    }

    SECTION("60 s and 120 s from a coroutine client") {
        boost::asio::io_context ioc;
        http::executor_client client(ioc);
        auto ws = open_on(ioc, client, url);
        CHECK(ws.get_ping_interval() == PING);
        CHECK(ws.get_idle_timeout() == IDLE);
    }

    SECTION("60 s and 120 s from async_client") {
        thinger::http::test::running_workers workers;
        http::async_client client(workers);
        std::atomic<bool> checked{false};
        client.websocket(url, [&](std::shared_ptr<http::websocket_client> ws) {
            if (ws && ws->get_ping_interval() == PING && ws->get_idle_timeout() == IDLE) checked = true;
            if (ws) ws->stop();
        });
        client.wait();
        CHECK(checked);
    }

    SECTION("The setter changes them, (0, 0) turns the keepalive off") {
        http::client client;
        auto ws = client.websocket(url);
        REQUIRE(ws.has_value());
        ws->set_keepalive(PING_INTERVAL, IDLE_TIMEOUT);
        CHECK(ws->get_ping_interval() == PING_INTERVAL);
        CHECK(ws->get_idle_timeout() == IDLE_TIMEOUT);
        ws->set_keepalive(0ms, 0ms);
        CHECK(ws->get_ping_interval() == 0ms);
        CHECK(ws->get_idle_timeout() == 0ms);
        ws->close();
    }

    SECTION("The keepalive stays on the WebSocket released") {
        http::client client;
        auto ws = client.websocket(url);
        REQUIRE(ws.has_value());
        ws->set_keepalive(PING_INTERVAL, IDLE_TIMEOUT);
        auto released = ws->release_socket();
        REQUIRE(released);
        CHECK(released->get_ping_interval() == PING_INTERVAL);
        CHECK(released->get_idle_timeout() == IDLE_TIMEOUT);
        CHECK(ws->get_ping_interval() == 0ms);
        released->close();
    }
}

// The keepalive closes a dead peer: off, it does not
TEST_CASE("Client WebSocket keepalive: set and turned off against a silent peer", "[websocket][client][keepalive][integration]") {
    using namespace thinger::http::test;
    const bool on = GENERATE(true, false);
    INFO("keepalive " << (on ? "300 ms / 1 s" : "(0, 0) after 300 ms / 1 s"));
    hostile_peer peer(transport::tcp, hostile_behaviour::silent, hostile_start::after_101);
    boost::asio::io_context ioc;
    http::executor_client client(ioc);
    std::atomic<bool> closed{false};
    auto ws = open_on(ioc, client, peer.websocket_url("/ws"));
    ws.on_close([&] { closed = true; });

    // on the io_context, which runs only below
    boost::asio::post(ioc, [&] {
        ws.set_keepalive(PING_INTERVAL, IDLE_TIMEOUT);
        if (!on) ws.set_keepalive(0ms, 0ms);
    });
    ws.run();
    ioc.run_for(IDLE_TIMEOUT + 500ms);
    CHECK(ws.is_open() == !on);
    CHECK(closed == on);
    ws.stop();
    ioc.run_for(100ms);
}

// A peer answering the pings is never closed by the keepalive, however the client is used
TEST_CASE("Client WebSocket keepalive: a responsive peer stays connected", "[websocket][client][keepalive][integration]") {
    WebSocketServerFixture fixture;

    SECTION("In the message loop of run()") {
        boost::asio::io_context ioc;
        http::executor_client client(ioc);
        std::atomic<bool> closed{false};
        auto ws = open_on(ioc, client, fixture.ws_url + "/ws/echo");
        // on the io_context, which runs only below
        boost::asio::post(ioc, [&] { ws.set_keepalive(PING_INTERVAL, IDLE_TIMEOUT); });
        ws.on_close([&] { closed = true; });
        ws.run();
        // nothing sent nor received but the pings and pongs
        ioc.run_for(USE_FOR);
        CHECK(ws.is_open());
        CHECK_FALSE(closed);
        ws.stop();
        ioc.run_for(100ms);
    }

    SECTION("In a loop of blocking receive() calls") {
        // a message every 1.5 s: each receive() waits longer than the idle timeout
        http::client client;
        auto ws = client.websocket(fixture.ws_url + "/ws/ticks");
        REQUIRE(ws.has_value());
        ws->set_keepalive(PING_INTERVAL, IDLE_TIMEOUT);
        auto start = clock_type::now();
        int ticks = 0;
        while (clock_type::now() - start < USE_FOR) {
            auto [message, binary] = ws->receive();
            REQUIRE(ws->is_open());
            REQUIRE(message == "tick");
            ++ticks;
            std::this_thread::sleep_for(200ms);
        }
        CHECK(ticks >= 3);
        ws->close();
    }

    SECTION("Sending periodically, never calling receive()") {
        http::client client;
        auto ws = client.websocket(fixture.ws_url + "/ws/sink");
        REQUIRE(ws.has_value());
        ws->set_keepalive(PING_INTERVAL, IDLE_TIMEOUT);
        auto start = clock_type::now();
        while (clock_type::now() - start < USE_FOR) {
            REQUIRE(ws->send_text("data"));
            std::this_thread::sleep_for(200ms);
        }
        CHECK(ws->is_open());
        ws->close();
    }

    SECTION("Sending less often than the idle timeout, never calling receive()") {
        // nothing runs between the calls: the first check of the next one, late, pings
        // before it closes, and the pong that arrived meanwhile counts as received
        http::client client;
        auto ws = client.websocket(fixture.ws_url + "/ws/sink");
        REQUIRE(ws.has_value());
        ws->set_keepalive(PING_INTERVAL, IDLE_TIMEOUT);
        for (int i = 0; i < 3; ++i) {
            INFO("send " << i);
            REQUIRE(ws->send_text("data"));
            REQUIRE(ws->is_open());
            std::this_thread::sleep_for(IDLE_TIMEOUT + 500ms);
        }
        CHECK(ws->send_text("data"));
        CHECK(ws->is_open());
        ws->close();
    }
}

// The keepalive keeps the io_context busy: a blocking call waits for its own operation only
TEST_CASE("Client WebSocket keepalive: the blocking calls return once their operation completes", "[websocket][client][keepalive][integration]") {
    WebSocketServerFixture fixture;
    const bool set = GENERATE(false, true);
    INFO("keepalive " << (set ? "2 s / 4 s" : "the default"));
    http::client client;

    auto start = clock_type::now();
    auto ws = client.websocket(fixture.ws_url + "/ws/echo");
    REQUIRE(ws.has_value());
    CHECK(elapsed_since(start) < PROMPT);
    if (set) ws->set_keepalive(2s, 4s);

    start = clock_type::now();
    REQUIRE(ws->send_text("text"));
    CHECK(elapsed_since(start) < PROMPT);

    start = clock_type::now();
    auto [text, text_binary] = ws->receive();
    CHECK(text == "text");
    CHECK(elapsed_since(start) < PROMPT);

    start = clock_type::now();
    REQUIRE(ws->send_binary(std::string_view("binary")));
    CHECK(elapsed_since(start) < PROMPT);
    auto [binary, is_binary] = ws->receive();
    CHECK(binary == "binary");
    CHECK(is_binary);

    // an HTTP request of the same client, the WebSocket open
    start = clock_type::now();
    auto response = client.get("http://127.0.0.1:" + std::to_string(fixture.port) + "/hello");
    CHECK(response.ok());
    CHECK(response.body() == "hello");
    CHECK(elapsed_since(start) < PROMPT);

    start = clock_type::now();
    ws->close();
    CHECK_FALSE(ws->is_open());
    CHECK(elapsed_since(start) < PROMPT);
}

// A synchronous call cannot wait on a thread running the io_context of its WebSocket: it would
// hold up the handler it waits for, or run the handlers of other connections within its caller.
// It throws there at once, before starting anything, and the connection stays usable
TEST_CASE("The synchronous calls of a WebSocket fail on a thread running its io_context", "[websocket][client][integration]") {
    WebSocketServerFixture fixture;
    const auto url = fixture.ws_url + "/ws/echo";
    constexpr auto AT_ONCE = 100ms;

    SECTION("In a plain callback of async_client, on its worker") {
        thinger::http::test::running_workers workers;
        http::async_client client(workers);
        bool opened = false;
        int thrown = 0;
        auto slowest = 0ms;
        std::string message;
        std::string echoed;
        client.websocket(url, [&](std::shared_ptr<http::websocket_client> ws) {
            if (!ws) return;
            opened = true;
            auto refused = [&](auto&& call) {
                auto start = clock_type::now();
                try {
                    call();
                } catch (const std::logic_error& error) {
                    ++thrown;
                    if (message.empty()) message = error.what();
                }
                slowest = std::max(slowest, elapsed_since(start));
            };
            refused([&] { ws->send_text("x"); });
            refused([&] { ws->send_binary(std::string_view("x")); });
            refused([&] { ws->receive(); });
            refused([&] { ws->close(); });

            // the coroutine API, on the connection still open
            client.run([ws, &echoed]() -> awaitable<void> {
                if (co_await ws->send_text_async("hello")) echoed = (co_await ws->receive_async()).first;
                co_await ws->close_async();
            });
        });
        REQUIRE(client.wait_for(10s));

        REQUIRE(opened);
        CHECK(thrown == 4);
        CHECK(slowest < AT_ONCE);
        INFO(message);
        CHECK(message.find("send_text()") != std::string::npos);
        CHECK(message.find("send_text_async()") != std::string::npos);
        CHECK(message.find("stop()") != std::string::npos);
        CHECK(echoed == "hello");
    }

    SECTION("In a coroutine of executor_client on the io_context, without polling it") {
        boost::asio::io_context context;
        bool opened = false;
        bool thrown = false;
        bool probe_ran = false;
        bool polled = true;
        auto took = 0ms;
        std::string echoed;
        co_spawn(context, [&]() -> awaitable<void> {
            auto client = co_await http::executor_client::for_current_coroutine();
            auto ws = co_await client->websocket(url);
            if (!ws) co_return;
            opened = true;
            // a handler ready to run: a call polling the io_context would run it
            boost::asio::post(context, [&] { probe_ran = true; });
            auto start = clock_type::now();
            try { ws->send_text("lost"); } catch (const std::logic_error&) { thrown = true; }
            took = elapsed_since(start);
            polled = probe_ran;

            if (co_await ws->send_text_async("hello")) echoed = (co_await ws->receive_async()).first;
            co_await ws->close_async();
        }, detached);
        context.run_for(10s);

        REQUIRE(opened);
        CHECK(thrown);
        CHECK(took < AT_ONCE);
        CHECK_FALSE(polled);
        CHECK(probe_ran);
        CHECK(echoed == "hello");
    }
}

// From a thread that does not run the io_context of the WebSocket, a synchronous call waits for
// its operation, which runs on the worker: the caller runs nothing of that io_context meanwhile
TEST_CASE("The synchronous calls of an async_client WebSocket wait on another thread without running its io_context",
          "[websocket][client][integration]") {
    thinger::http::test::running_workers workers(1);
    WebSocketServerFixture fixture;
    http::async_client client(workers);

    struct handed_websocket {
        std::shared_ptr<http::websocket_client> ws;
        boost::asio::io_context* context = nullptr;
        std::thread::id worker;
    };
    std::promise<handed_websocket> handed;
    client.websocket(fixture.ws_url + "/ws/echo", [&](std::shared_ptr<http::websocket_client> ws) {
        handed.set_value({std::move(ws), &workers.get_thread_io_context(), std::this_thread::get_id()});
    });
    auto future = handed.get_future();
    REQUIRE(future.wait_for(5s) == std::future_status::ready);
    auto websocket = future.get();
    auto& ws = websocket.ws;
    auto* context = websocket.context;
    const auto worker = websocket.worker;
    REQUIRE(ws);
    REQUIRE(ws->is_open());
    REQUIRE(worker != std::this_thread::get_id());

    // The thread of every handler of the io_context run during the calls: before each call, the
    // worker is held up a while with probe handlers queued behind it, which a second thread
    // running the io_context would run meanwhile. They run before the operation of the call,
    // queued after them
    std::mutex mutex;
    std::set<std::thread::id> ran_on;
    int probes = 0;
    auto probed = [&](auto&& call) {
        boost::asio::post(*context, [] { std::this_thread::sleep_for(50ms); });
        for (int i = 0; i < 5; ++i) {
            boost::asio::post(*context, [&] {
                std::lock_guard lock(mutex);
                ran_on.insert(std::this_thread::get_id());
                ++probes;
            });
        }
        auto start = clock_type::now();
        call();
        CHECK(elapsed_since(start) < 2s);
    };

    probed([&] { CHECK(ws->send_text("hello")); });
    probed([&] {
        auto [message, binary] = ws->receive();
        CHECK(message == "hello");
        CHECK_FALSE(binary);
    });
    probed([&] { ws->set_keepalive(PING_INTERVAL, IDLE_TIMEOUT); });
    CHECK(ws->get_ping_interval() == PING_INTERVAL);
    CHECK(ws->get_idle_timeout() == IDLE_TIMEOUT);
    probed([&] { ws->close(); });
    CHECK_FALSE(ws->is_open());

    std::lock_guard lock(mutex);
    CHECK(probes == 20);
    CHECK(ran_on == std::set<std::thread::id>{worker});
    ws.reset();
}

// http::client runs its io_context only within its synchronous calls: from whichever thread
// makes them, one at a time, but not from within one of its own callbacks
TEST_CASE("The synchronous calls of http::client within its own callbacks fail", "[websocket][client][integration]") {
    WebSocketServerFixture fixture;
    const auto hello = "http://127.0.0.1:" + std::to_string(fixture.port) + "/hello";
    http::client client;
    auto ws = client.websocket(fixture.ws_url + "/ws/echo");
    REQUIRE(ws.has_value());

    int calls = 0;
    int thrown = 0;
    auto result = client.send_streaming(http::http_request::create_http_request(http::method::GET, hello),
        [&](const http::stream_info&) {
            if (calls++ > 0) return true;
            try { client.get(hello); } catch (const std::logic_error&) { ++thrown; }
            try { ws->send_text("lost"); } catch (const std::logic_error&) { ++thrown; }
            return true;
        });
    CHECK(result.ok());
    CHECK(calls > 0);
    CHECK(thrown == 2);

    // Both still work, the WebSocket also from another thread (while no other uses the client)
    CHECK(client.get(hello).body() == "hello");
    REQUIRE(ws->send_text("from another thread"));
    auto received = std::async(std::launch::async, [&] { return ws->receive(); });
    REQUIRE(received.wait_for(2s) == std::future_status::ready);
    CHECK(received.get().first == "from another thread");
    ws->close();
    CHECK_FALSE(ws->is_open());
}

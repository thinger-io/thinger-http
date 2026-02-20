#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/sse_connection.hpp>
#include <boost/asio.hpp>
#include <chrono>
#include <thread>
#include <future>

using namespace thinger;
using namespace std::chrono_literals;

// Fixture for SSE tests
struct SSETestFixture {
    http::server server;
    uint16_t port = 0;
    std::thread server_thread;
    bool server_started = false;

    SSETestFixture() = default;

    void start_server() {
        if (server_started) return;
        REQUIRE(server.listen("0.0.0.0", 0));
        port = server.local_port();
        server_started = true;

        std::promise<void> ready;
        server_thread = std::thread([this, &ready]() {
            ready.set_value();
            server.wait();
        });
        ready.get_future().wait();
    }

    ~SSETestFixture() {
        if (server_started) {
            server.stop();
            if (server_thread.joinable()) {
                server_thread.join();
            }
        }
    }
};

// Helper: connect a raw TCP socket
static boost::asio::ip::tcp::socket raw_connect(boost::asio::io_context& ioc, uint16_t port) {
    boost::asio::ip::tcp::socket sock(ioc);
    boost::asio::ip::tcp::resolver resolver(ioc);
    auto results = resolver.resolve("127.0.0.1", std::to_string(port));
    boost::asio::connect(sock, results);
    return sock;
}

// Helper: send an HTTP GET request for SSE
static void send_sse_request(boost::asio::ip::tcp::socket& sock, const std::string& path) {
    std::string request =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Accept: text/event-stream\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    boost::asio::write(sock, boost::asio::buffer(request));
}

// Helper: read HTTP response headers (up to \r\n\r\n)
static std::string read_response_headers(boost::asio::ip::tcp::socket& sock,
                                          boost::asio::streambuf& buf) {
    boost::system::error_code ec;
    boost::asio::read_until(sock, buf, "\r\n\r\n", ec);
    if (ec) return "";
    std::string data(boost::asio::buffers_begin(buf.data()),
                     boost::asio::buffers_end(buf.data()));
    // Consume the headers from the buffer
    auto header_end = data.find("\r\n\r\n");
    if (header_end != std::string::npos) {
        buf.consume(header_end + 4);
    }
    return data.substr(0, header_end + 4);
}

// Helper: read SSE data until double newline (\n\n) which marks end of a data event
static std::string read_sse_event(boost::asio::ip::tcp::socket& sock,
                                   boost::asio::streambuf& buf) {
    boost::system::error_code ec;
    boost::asio::read_until(sock, buf, "\n\n", ec);
    if (ec) return "";
    std::string data(boost::asio::buffers_begin(buf.data()),
                     boost::asio::buffers_end(buf.data()));
    // Find the \n\n boundary and consume it
    auto end = data.find("\n\n");
    if (end != std::string::npos) {
        std::string event = data.substr(0, end + 2);
        buf.consume(end + 2);
        return event;
    }
    return data;
}

// Helper: read a single SSE line (ending with \n)
static std::string read_sse_line(boost::asio::ip::tcp::socket& sock,
                                  boost::asio::streambuf& buf) {
    boost::system::error_code ec;
    boost::asio::read_until(sock, buf, "\n", ec);
    if (ec) return "";
    std::string data(boost::asio::buffers_begin(buf.data()),
                     boost::asio::buffers_end(buf.data()));
    auto end = data.find("\n");
    if (end != std::string::npos) {
        std::string line = data.substr(0, end + 1);
        buf.consume(end + 1);
        return line;
    }
    return data;
}

// ============================================================================
// SSE Response Headers Tests
// ============================================================================

TEST_CASE("SSE response includes correct headers", "[server][sse][integration]") {
    SSETestFixture fixture;

    std::promise<void> sse_ready;
    std::shared_ptr<http::sse_connection> sse_conn;

    fixture.server.get("/events", [&](http::request& req, http::response& res) {
        res.start_sse([&](std::shared_ptr<http::sse_connection> conn) {
            sse_conn = conn;
            sse_ready.set_value();
        });
    });

    fixture.start_server();

    boost::asio::io_context ioc;
    auto sock = raw_connect(ioc, fixture.port);
    boost::asio::streambuf buf;

    send_sse_request(sock, "/events");

    std::string headers = read_response_headers(sock, buf);

    REQUIRE(headers.find("HTTP/1.1 200") != std::string::npos);
    REQUIRE(headers.find("Content-Type: text/event-stream") != std::string::npos);
    REQUIRE(headers.find("Cache-Control: no-cache") != std::string::npos);
    REQUIRE(headers.find("X-Accel-Buffering: no") != std::string::npos);

    // Wait for SSE connection to be established
    REQUIRE(sse_ready.get_future().wait_for(5s) == std::future_status::ready);

    sse_conn->stop();
    sock.close();
}

// ============================================================================
// SSE send_data Tests
// ============================================================================

TEST_CASE("SSE send_data delivers data event to client", "[server][sse][integration]") {
    SSETestFixture fixture;

    std::promise<void> sse_ready;
    std::shared_ptr<http::sse_connection> sse_conn;

    fixture.server.get("/events", [&](http::request& req, http::response& res) {
        res.start_sse([&](std::shared_ptr<http::sse_connection> conn) {
            sse_conn = conn;
            sse_ready.set_value();
        });
    });

    fixture.start_server();

    boost::asio::io_context ioc;
    auto sock = raw_connect(ioc, fixture.port);
    boost::asio::streambuf buf;

    send_sse_request(sock, "/events");
    read_response_headers(sock, buf);

    REQUIRE(sse_ready.get_future().wait_for(5s) == std::future_status::ready);

    // Send a data event
    sse_conn->send_data("hello world");

    // Read the SSE event — format: "data: hello world\n\n"
    std::string event = read_sse_event(sock, buf);
    REQUIRE(event == "data: hello world\n\n");

    sse_conn->stop();
    sock.close();
}

// ============================================================================
// SSE send_event Tests
// ============================================================================

TEST_CASE("SSE send_event followed by send_data", "[server][sse][integration]") {
    SSETestFixture fixture;

    std::promise<void> sse_ready;
    std::shared_ptr<http::sse_connection> sse_conn;

    fixture.server.get("/events", [&](http::request& req, http::response& res) {
        res.start_sse([&](std::shared_ptr<http::sse_connection> conn) {
            sse_conn = conn;
            sse_ready.set_value();
        });
    });

    fixture.start_server();

    boost::asio::io_context ioc;
    auto sock = raw_connect(ioc, fixture.port);
    boost::asio::streambuf buf;

    send_sse_request(sock, "/events");
    read_response_headers(sock, buf);

    REQUIRE(sse_ready.get_future().wait_for(5s) == std::future_status::ready);

    // Send an event name followed by data
    sse_conn->send_event("ping");
    sse_conn->send_data("pong");

    // Read the event line — format: "event: ping\n"
    std::string event_line = read_sse_line(sock, buf);
    REQUIRE(event_line == "event: ping\n");

    // Read the data event — format: "data: pong\n\n"
    std::string data_event = read_sse_event(sock, buf);
    REQUIRE(data_event == "data: pong\n\n");

    sse_conn->stop();
    sock.close();
}

// ============================================================================
// SSE send_retry Tests
// ============================================================================

TEST_CASE("SSE send_retry delivers retry directive to client", "[server][sse][integration]") {
    SSETestFixture fixture;

    std::promise<void> sse_ready;
    std::shared_ptr<http::sse_connection> sse_conn;

    fixture.server.get("/events", [&](http::request& req, http::response& res) {
        res.start_sse([&](std::shared_ptr<http::sse_connection> conn) {
            sse_conn = conn;
            sse_ready.set_value();
        });
    });

    fixture.start_server();

    boost::asio::io_context ioc;
    auto sock = raw_connect(ioc, fixture.port);
    boost::asio::streambuf buf;

    send_sse_request(sock, "/events");
    read_response_headers(sock, buf);

    REQUIRE(sse_ready.get_future().wait_for(5s) == std::future_status::ready);

    // Send a retry directive
    sse_conn->send_retry(3000);

    // Read the retry line — format: "retry: 3000\n"
    std::string retry_line = read_sse_line(sock, buf);
    REQUIRE(retry_line == "retry: 3000\n");

    sse_conn->stop();
    sock.close();
}

// ============================================================================
// SSE Multiple Messages Tests
// ============================================================================

TEST_CASE("SSE multiple data messages arrive in order", "[server][sse][integration]") {
    SSETestFixture fixture;

    std::promise<void> sse_ready;
    std::shared_ptr<http::sse_connection> sse_conn;

    fixture.server.get("/events", [&](http::request& req, http::response& res) {
        res.start_sse([&](std::shared_ptr<http::sse_connection> conn) {
            sse_conn = conn;
            sse_ready.set_value();
        });
    });

    fixture.start_server();

    boost::asio::io_context ioc;
    auto sock = raw_connect(ioc, fixture.port);
    boost::asio::streambuf buf;

    send_sse_request(sock, "/events");
    read_response_headers(sock, buf);

    REQUIRE(sse_ready.get_future().wait_for(5s) == std::future_status::ready);

    // Send multiple data messages
    sse_conn->send_data("message 1");
    sse_conn->send_data("message 2");
    sse_conn->send_data("message 3");

    // Read all three events in order
    std::string event1 = read_sse_event(sock, buf);
    REQUIRE(event1 == "data: message 1\n\n");

    std::string event2 = read_sse_event(sock, buf);
    REQUIRE(event2 == "data: message 2\n\n");

    std::string event3 = read_sse_event(sock, buf);
    REQUIRE(event3 == "data: message 3\n\n");

    sse_conn->stop();
    sock.close();
}

// ============================================================================
// SSE Connection Counter Tests
// ============================================================================

TEST_CASE("SSE connection counter increments and decrements", "[server][sse][integration]") {
    SSETestFixture fixture;

    std::promise<void> sse_ready;
    std::shared_ptr<http::sse_connection> sse_conn;

    fixture.server.get("/events", [&](http::request& req, http::response& res) {
        res.start_sse([&](std::shared_ptr<http::sse_connection> conn) {
            sse_conn = conn;
            sse_ready.set_value();
        });
    });

    fixture.start_server();

    auto initial_count = http::sse_connection::connections.load();

    boost::asio::io_context ioc;
    auto sock = raw_connect(ioc, fixture.port);
    boost::asio::streambuf buf;

    send_sse_request(sock, "/events");
    read_response_headers(sock, buf);

    REQUIRE(sse_ready.get_future().wait_for(5s) == std::future_status::ready);

    // Connection counter should have incremented
    REQUIRE(http::sse_connection::connections.load() == initial_count + 1);

    // Stop and release the SSE connection
    sse_conn->stop();
    sse_conn.reset();
    sock.close();

    // Give time for cleanup
    std::this_thread::sleep_for(200ms);

    // Connection counter should be back to initial
    REQUIRE(http::sse_connection::connections.load() == initial_count);
}

// ============================================================================
// SSE Handler Sends Data Immediately Tests
// ============================================================================

TEST_CASE("SSE handler sends initial data on connect", "[server][sse][integration]") {
    SSETestFixture fixture;

    // Handler that sends data immediately when SSE is established
    fixture.server.get("/events", [](http::request& req, http::response& res) {
        res.start_sse([](std::shared_ptr<http::sse_connection> conn) {
            conn->send_retry(5000);
            conn->send_event("welcome");
            conn->send_data("{\"connected\":true}");
        });
    });

    fixture.start_server();

    boost::asio::io_context ioc;
    auto sock = raw_connect(ioc, fixture.port);
    boost::asio::streambuf buf;

    send_sse_request(sock, "/events");

    std::string headers = read_response_headers(sock, buf);
    REQUIRE(headers.find("HTTP/1.1 200") != std::string::npos);

    // Read the retry line
    std::string retry_line = read_sse_line(sock, buf);
    REQUIRE(retry_line == "retry: 5000\n");

    // Read the event name
    std::string event_line = read_sse_line(sock, buf);
    REQUIRE(event_line == "event: welcome\n");

    // Read the data event
    std::string data_event = read_sse_event(sock, buf);
    REQUIRE(data_event == "data: {\"connected\":true}\n\n");

    sock.close();
}

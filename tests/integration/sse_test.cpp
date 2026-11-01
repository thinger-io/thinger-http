#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/sse_connection.hpp>
#include "server_test_helpers.hpp"
#include <chrono>
#include <thread>
#include <future>
#include <optional>
#include <string>

using namespace thinger;
using namespace std::chrono_literals;

using server_test::running_server;
using http::test::transport;

// Fixture for SSE tests: the server, run on the transport of the case once its routes are set
struct SSETestFixture {
    http::server server;
    std::optional<running_server> running;

    void start_server(transport kind) {
        if (!running) running.emplace(server, kind);
    }
};

// A raw client of an SSE stream over any transport; every read waits up to a timeout
struct sse_reader {
    http::test::raw_connection connection;
    std::string buffer;

    explicit sse_reader(const running_server& running) : connection(running.endpoint()) {}

    // What arrived up to and including `delimiter` (consumed), or "" if it did not in time
    std::string read_until(const std::string& delimiter, std::chrono::milliseconds timeout = 5s) {
        if (!connection.read_until(buffer, delimiter, timeout)) return "";
        auto end = buffer.find(delimiter) + delimiter.size();
        std::string data = buffer.substr(0, end);
        buffer.erase(0, end);
        return data;
    }

    void close() { connection.close(); }
};

// Helper: send an HTTP GET request for SSE
static void send_sse_request(sse_reader& sock, const std::string& path) {
    std::string request =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Accept: text/event-stream\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    REQUIRE(sock.connection.write(request));
}

// Helper: read HTTP response headers (up to \r\n\r\n)
static std::string read_response_headers(sse_reader& sock) {
    return sock.read_until("\r\n\r\n");
}

// Helper: read SSE data until double newline (\n\n) which marks end of a data event
static std::string read_sse_event(sse_reader& sock) {
    return sock.read_until("\n\n");
}

// Helper: read a single SSE line (ending with \n)
static std::string read_sse_line(sse_reader& sock) {
    return sock.read_until("\n");
}

// ============================================================================
// SSE Response Headers Tests
// ============================================================================

TEST_CASE("SSE response includes correct headers", "[server][sse][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    SSETestFixture fixture;

    std::promise<void> sse_ready;
    std::shared_ptr<http::sse_connection> sse_conn;

    fixture.server.get("/events", [&](http::request& req, http::response& res) {
        res.start_sse([&](std::shared_ptr<http::sse_connection> conn) {
            sse_conn = conn;
            sse_ready.set_value();
        });
    });

    fixture.start_server(kind);

    sse_reader sock(*fixture.running);

    send_sse_request(sock, "/events");

    std::string headers = read_response_headers(sock);

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

TEST_CASE("SSE send_data delivers data event to client", "[server][sse][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    SSETestFixture fixture;

    std::promise<void> sse_ready;
    std::shared_ptr<http::sse_connection> sse_conn;

    fixture.server.get("/events", [&](http::request& req, http::response& res) {
        res.start_sse([&](std::shared_ptr<http::sse_connection> conn) {
            sse_conn = conn;
            sse_ready.set_value();
        });
    });

    fixture.start_server(kind);

    sse_reader sock(*fixture.running);

    send_sse_request(sock, "/events");
    read_response_headers(sock);

    REQUIRE(sse_ready.get_future().wait_for(5s) == std::future_status::ready);

    // Send a data event
    sse_conn->send_data("hello world");

    // Read the SSE event — format: "data: hello world\n\n"
    std::string event = read_sse_event(sock);
    REQUIRE(event == "data: hello world\n\n");

    sse_conn->stop();
    sock.close();
}

// ============================================================================
// SSE send_event Tests
// ============================================================================

TEST_CASE("SSE send_event followed by send_data", "[server][sse][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    SSETestFixture fixture;

    std::promise<void> sse_ready;
    std::shared_ptr<http::sse_connection> sse_conn;

    fixture.server.get("/events", [&](http::request& req, http::response& res) {
        res.start_sse([&](std::shared_ptr<http::sse_connection> conn) {
            sse_conn = conn;
            sse_ready.set_value();
        });
    });

    fixture.start_server(kind);

    sse_reader sock(*fixture.running);

    send_sse_request(sock, "/events");
    read_response_headers(sock);

    REQUIRE(sse_ready.get_future().wait_for(5s) == std::future_status::ready);

    // Send an event name followed by data
    sse_conn->send_event("ping");
    sse_conn->send_data("pong");

    // Read the event line — format: "event: ping\n"
    std::string event_line = read_sse_line(sock);
    REQUIRE(event_line == "event: ping\n");

    // Read the data event — format: "data: pong\n\n"
    std::string data_event = read_sse_event(sock);
    REQUIRE(data_event == "data: pong\n\n");

    sse_conn->stop();
    sock.close();
}

// ============================================================================
// SSE send_retry Tests
// ============================================================================

TEST_CASE("SSE send_retry delivers retry directive to client", "[server][sse][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    SSETestFixture fixture;

    std::promise<void> sse_ready;
    std::shared_ptr<http::sse_connection> sse_conn;

    fixture.server.get("/events", [&](http::request& req, http::response& res) {
        res.start_sse([&](std::shared_ptr<http::sse_connection> conn) {
            sse_conn = conn;
            sse_ready.set_value();
        });
    });

    fixture.start_server(kind);

    sse_reader sock(*fixture.running);

    send_sse_request(sock, "/events");
    read_response_headers(sock);

    REQUIRE(sse_ready.get_future().wait_for(5s) == std::future_status::ready);

    // Send a retry directive
    sse_conn->send_retry(3000);

    // Read the retry line — format: "retry: 3000\n"
    std::string retry_line = read_sse_line(sock);
    REQUIRE(retry_line == "retry: 3000\n");

    sse_conn->stop();
    sock.close();
}

// ============================================================================
// SSE Multiple Messages Tests
// ============================================================================

TEST_CASE("SSE multiple data messages arrive in order", "[server][sse][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    SSETestFixture fixture;

    std::promise<void> sse_ready;
    std::shared_ptr<http::sse_connection> sse_conn;

    fixture.server.get("/events", [&](http::request& req, http::response& res) {
        res.start_sse([&](std::shared_ptr<http::sse_connection> conn) {
            sse_conn = conn;
            sse_ready.set_value();
        });
    });

    fixture.start_server(kind);

    sse_reader sock(*fixture.running);

    send_sse_request(sock, "/events");
    read_response_headers(sock);

    REQUIRE(sse_ready.get_future().wait_for(5s) == std::future_status::ready);

    // Send multiple data messages
    sse_conn->send_data("message 1");
    sse_conn->send_data("message 2");
    sse_conn->send_data("message 3");

    // Read all three events in order
    std::string event1 = read_sse_event(sock);
    REQUIRE(event1 == "data: message 1\n\n");

    std::string event2 = read_sse_event(sock);
    REQUIRE(event2 == "data: message 2\n\n");

    std::string event3 = read_sse_event(sock);
    REQUIRE(event3 == "data: message 3\n\n");

    sse_conn->stop();
    sock.close();
}

// ============================================================================
// SSE Connection Counter Tests
// ============================================================================

TEST_CASE("SSE connections are counted in the server statistics", "[server][sse][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    SSETestFixture fixture;

    std::promise<void> sse_ready;
    std::shared_ptr<http::sse_connection> sse_conn;

    fixture.server.get("/events", [&](http::request& req, http::response& res) {
        res.start_sse([&](std::shared_ptr<http::sse_connection> conn) {
            sse_conn = conn;
            sse_ready.set_value();
        });
    });

    fixture.start_server(kind);

    REQUIRE(fixture.server.stats().sse_connections == 0);

    sse_reader sock(*fixture.running);

    send_sse_request(sock, "/events");
    read_response_headers(sock);

    REQUIRE(sse_ready.get_future().wait_for(5s) == std::future_status::ready);

    // Counted as an event stream, no longer as an HTTP connection
    auto stats = fixture.server.stats();
    REQUIRE(stats.sse_connections == 1);
    REQUIRE(stats.http_connections == 0);
    REQUIRE(stats.total_connections == 1);

    // Stop and release the SSE connection
    sse_conn->stop();
    sse_conn.reset();
    sock.close();

    // Give time for cleanup
    std::this_thread::sleep_for(200ms);

    // Not counted once stopped
    REQUIRE(fixture.server.stats().sse_connections == 0);
    REQUIRE(fixture.server.stats().active_connections() == 0);
}

// ============================================================================
// SSE Handler Sends Data Immediately Tests
// ============================================================================

TEST_CASE("SSE handler sends initial data on connect", "[server][sse][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    SSETestFixture fixture;

    // Handler that sends data immediately when SSE is established
    fixture.server.get("/events", [](http::request& req, http::response& res) {
        res.start_sse([](std::shared_ptr<http::sse_connection> conn) {
            conn->send_retry(5000);
            conn->send_event("welcome");
            conn->send_data("{\"connected\":true}");
        });
    });

    fixture.start_server(kind);

    sse_reader sock(*fixture.running);

    send_sse_request(sock, "/events");

    std::string headers = read_response_headers(sock);
    REQUIRE(headers.find("HTTP/1.1 200") != std::string::npos);

    // Read the retry line
    std::string retry_line = read_sse_line(sock);
    REQUIRE(retry_line == "retry: 5000\n");

    // Read the event name
    std::string event_line = read_sse_line(sock);
    REQUIRE(event_line == "event: welcome\n");

    // Read the data event
    std::string data_event = read_sse_event(sock);
    REQUIRE(data_event == "data: {\"connected\":true}\n\n");

    sock.close();
}

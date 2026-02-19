#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/client/client.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <future>
#include <filesystem>
#include <cstdlib>

using namespace thinger;
using namespace std::chrono_literals;

// Test fixture for Unix socket server functionality
// Note: Routes and middleware must be added BEFORE calling start_server()
struct UnixSocketTestFixture {
    http::server server;
    std::string socket_path;
    std::thread server_thread;
    bool server_started = false;

    UnixSocketTestFixture() {
        // Generate a unique temporary socket path per test
        socket_path = (std::filesystem::temp_directory_path() /
                       ("thinger_test_" + std::to_string(getpid()) + "_" +
                        std::to_string(reinterpret_cast<uintptr_t>(this)) + ".sock")).string();
        // Remove any leftover socket file
        std::filesystem::remove(socket_path);
    }

    // Call this after setting up routes and middleware
    void start_server() {
        if (server_started) return;

        REQUIRE(server.listen_unix(socket_path));
        server_started = true;

        std::promise<void> ready;
        server_thread = std::thread([this, &ready]() {
            ready.set_value();
            server.wait();
        });
        ready.get_future().wait();
    }

    // Helper: build URL for client (host is ignored for Unix sockets)
    std::string url(const std::string& path) {
        return "http://localhost" + path;
    }

    ~UnixSocketTestFixture() {
        if (server_started) {
            server.stop();
            if (server_thread.joinable()) {
                server_thread.join();
            }
        }
        // Ensure socket file is cleaned up
        std::filesystem::remove(socket_path);
    }
};

// ============================================================================
// 1. Server Lifecycle
// ============================================================================

TEST_CASE("Unix Socket Server lifecycle", "[unix][server][lifecycle][integration]") {
    std::string socket_path = (std::filesystem::temp_directory_path() /
                               ("thinger_lifecycle_test_" + std::to_string(getpid()) + ".sock")).string();
    std::filesystem::remove(socket_path);

    SECTION("listen_unix creates socket file, stop removes it") {
        http::server server;
        REQUIRE(server.listen_unix(socket_path));
        REQUIRE(std::filesystem::exists(socket_path));
        REQUIRE(server.is_listening());

        std::promise<void> ready;
        std::thread t([&server, &ready]() {
            ready.set_value();
            server.wait();
        });
        ready.get_future().wait();

        REQUIRE(server.stop());
        t.join();

        REQUIRE_FALSE(server.is_listening());
        REQUIRE_FALSE(std::filesystem::exists(socket_path));
    }

    // Cleanup in case of failure
    std::filesystem::remove(socket_path);
}

// ============================================================================
// 2. GET Request/Response
// ============================================================================

TEST_CASE("Unix Socket GET request/response", "[unix][server][get][integration]") {
    UnixSocketTestFixture fixture;
    auto& server = fixture.server;

    server.get("/hello", [](http::response& res) {
        res.json({{"message", "hello from unix socket"}});
    });

    server.get("/greet/:name", [](http::request& req, http::response& res) {
        res.json({{"greeting", "hello " + std::string(req["name"])}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Simple GET returns 200 with JSON body") {
        auto response = client.get(fixture.url("/hello"), fixture.socket_path);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["message"] == "hello from unix socket");
    }

    SECTION("GET with path parameter") {
        auto response = client.get(fixture.url("/greet/world"), fixture.socket_path);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["greeting"] == "hello world");
    }
}

// ============================================================================
// 3. POST with JSON Body
// ============================================================================

TEST_CASE("Unix Socket POST with JSON body", "[unix][server][post][integration]") {
    UnixSocketTestFixture fixture;
    auto& server = fixture.server;

    server.post("/echo-json", [](nlohmann::json& json, http::response& res) {
        json["echoed"] = true;
        res.json(json);
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("POST JSON body is echoed back") {
        std::string body = R"({"name": "unix_test", "value": 42})";
        auto response = client.post(fixture.url("/echo-json"), fixture.socket_path,
                                    body, "application/json");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["name"] == "unix_test");
        REQUIRE(json["value"] == 42);
        REQUIRE(json["echoed"] == true);
    }
}

// ============================================================================
// 4. Multiple HTTP Methods
// ============================================================================

TEST_CASE("Unix Socket multiple HTTP methods", "[unix][server][methods][integration]") {
    UnixSocketTestFixture fixture;
    auto& server = fixture.server;

    server.get("/resource", [](http::response& res) {
        res.json({{"method", "GET"}});
    });

    server.post("/resource", [](http::response& res) {
        res.json({{"method", "POST"}});
    });

    server.put("/resource", [](http::request& req, http::response& res) {
        res.json({{"method", "PUT"}, {"body", req.body()}});
    });

    server.del("/resource", [](http::response& res) {
        res.json({{"method", "DELETE"}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("GET on /resource") {
        auto response = client.get(fixture.url("/resource"), fixture.socket_path);
        REQUIRE(response.ok());
        REQUIRE(response.json()["method"] == "GET");
    }

    SECTION("POST on /resource") {
        auto response = client.post(fixture.url("/resource"), fixture.socket_path,
                                    "", "text/plain");
        REQUIRE(response.ok());
        REQUIRE(response.json()["method"] == "POST");
    }

    SECTION("PUT on /resource via generic send") {
        auto request = std::make_shared<http::http_request>();
        request->set_method(http::method::PUT);
        request->set_url(fixture.url("/resource"));
        request->set_unix_socket(fixture.socket_path);
        request->set_content("test body", "text/plain");
        auto response = client.send(request);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["method"] == "PUT");
        REQUIRE(json["body"] == "test body");
    }

    SECTION("DELETE on /resource via generic send") {
        auto request = std::make_shared<http::http_request>();
        request->set_method(http::method::DELETE);
        request->set_url(fixture.url("/resource"));
        request->set_unix_socket(fixture.socket_path);
        auto response = client.send(request);
        REQUIRE(response.ok());
        REQUIRE(response.json()["method"] == "DELETE");
    }
}

// ============================================================================
// 5. Multiple Sequential Requests (keep-alive)
// ============================================================================

TEST_CASE("Unix Socket multiple sequential requests", "[unix][server][sequential][integration]") {
    UnixSocketTestFixture fixture;
    auto& server = fixture.server;

    std::atomic<int> counter{0};
    server.get("/count", [&counter](http::response& res) {
        int val = ++counter;
        res.json({{"count", val}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Multiple sequential requests succeed") {
        for (int i = 1; i <= 5; ++i) {
            auto response = client.get(fixture.url("/count"), fixture.socket_path);
            REQUIRE(response.ok());
            REQUIRE(response.json()["count"] == i);
        }
    }
}

// ============================================================================
// 6. Custom Headers
// ============================================================================

TEST_CASE("Unix Socket custom headers", "[unix][server][headers][integration]") {
    UnixSocketTestFixture fixture;
    auto& server = fixture.server;

    server.get("/headers", [](http::request& req, http::response& res) {
        // Echo back the custom header from request
        auto custom = req.header("X-Custom-Input");
        res.header("X-Custom-Output", "pong");
        res.json({{"received_header", std::string(custom)}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Send and receive custom headers") {
        http::headers_map headers = {{"X-Custom-Input", "ping"}};
        auto response = client.get(fixture.url("/headers"), fixture.socket_path, headers);
        REQUIRE(response.ok());
        REQUIRE(response.json()["received_header"] == "ping");
        REQUIRE(response.header("X-Custom-Output") == "pong");
    }
}

// ============================================================================
// 7. Not Found Handler
// ============================================================================

TEST_CASE("Unix Socket not found handler", "[unix][server][notfound][integration]") {
    UnixSocketTestFixture fixture;
    auto& server = fixture.server;

    server.get("/exists", [](http::response& res) {
        res.json({{"found", true}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Request to existing route returns 200") {
        auto response = client.get(fixture.url("/exists"), fixture.socket_path);
        REQUIRE(response.ok());
        REQUIRE(response.json()["found"] == true);
    }

    SECTION("Request to non-existent route returns 404") {
        auto response = client.get(fixture.url("/does-not-exist"), fixture.socket_path);
        REQUIRE(response.status() == 404);
    }
}

// ============================================================================
// 8. Large Response Body
// ============================================================================

TEST_CASE("Unix Socket large response body", "[unix][server][large][integration]") {
    UnixSocketTestFixture fixture;
    auto& server = fixture.server;

    // Generate a large response (256 KB)
    const size_t large_size = 256 * 1024;
    std::string large_body(large_size, 'X');

    server.get("/large", [large_body](http::response& res) {
        res.send(large_body, "text/plain");
    });

    fixture.start_server();
    http::client client;
    client.timeout(30s);

    SECTION("Large response body arrives intact") {
        auto response = client.get(fixture.url("/large"), fixture.socket_path);
        REQUIRE(response.ok());
        REQUIRE(response.body().size() == large_size);
        REQUIRE(response.body() == large_body);
    }
}

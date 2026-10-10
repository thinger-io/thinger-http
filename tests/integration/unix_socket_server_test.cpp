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
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string_view>
#include <vector>
#include <thinger/http/server/pool_server.hpp>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

using namespace thinger;
using namespace std::chrono_literals;

namespace {

// A blocking client connection to a Unix socket, never waiting longer than 10 seconds to read
class unix_client {
public:
    explicit unix_client(const std::string& path) {
        descriptor_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (descriptor_ < 0) return;
        timeval timeout{10, 0};
        ::setsockopt(descriptor_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#if defined(SO_NOSIGPIPE)
        int enabled = 1;
        ::setsockopt(descriptor_, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
        if (::connect(descriptor_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
    }
    ~unix_client() { if (descriptor_ >= 0) ::close(descriptor_); }
    unix_client(const unix_client&) = delete;
    unix_client& operator=(const unix_client&) = delete;

    bool connected() const { return descriptor_ >= 0; }

    bool send(std::string_view data) {
#if defined(MSG_NOSIGNAL)
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        while (!data.empty()) {
            auto sent = ::send(descriptor_, data.data(), data.size(), flags);
            if (sent <= 0) return false;
            data.remove_prefix(static_cast<size_t>(sent));
        }
        return true;
    }

    // Read until the server closes the connection
    std::string read_all() {
        std::string data;
        while (read_more(data)) {}
        return data;
    }

    // Read a response framed by its Content-Length (empty if it does not arrive)
    std::string read_response() {
        while (true) {
            auto head_end = buffer_.find("\r\n\r\n");
            if (head_end != std::string::npos) {
                auto length = content_length(std::string_view(buffer_).substr(0, head_end));
                auto size = head_end + 4 + length;
                while (buffer_.size() < size) {
                    if (!read_more(buffer_)) return {};
                }
                auto response = buffer_.substr(0, size);
                buffer_.erase(0, size);
                return response;
            }
            if (!read_more(buffer_)) return {};
        }
    }

private:
    bool read_more(std::string& data) {
        char chunk[4096];
        auto received = ::recv(descriptor_, chunk, sizeof(chunk), 0);
        if (received <= 0) return false;
        data.append(chunk, static_cast<size_t>(received));
        return true;
    }

    static size_t content_length(std::string_view head) {
        for (size_t line = head.find("\r\n"); line != std::string_view::npos; line = head.find("\r\n", line + 2)) {
            auto header = head.substr(line + 2, head.find("\r\n", line + 2) - (line + 2));
            constexpr std::string_view name = "content-length:";
            if (header.size() > name.size() &&
                std::equal(name.begin(), name.end(), header.begin(),
                           [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); })) {
                return std::strtoul(std::string(header.substr(name.size())).c_str(), nullptr, 10);
            }
        }
        return 0;
    }

    int descriptor_ = -1;
    std::string buffer_;
};

std::string temporary_socket_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() /
            ("thinger_" + name + "_" + std::to_string(getpid()) + ".sock")).string();
}

} // namespace

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
    client.timeout(10s).unix_socket(fixture.socket_path);

    SECTION("Simple GET returns 200 with JSON body") {
        auto response = client.get(fixture.url("/hello"));
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["message"] == "hello from unix socket");
    }

    SECTION("GET with path parameter") {
        auto response = client.get(fixture.url("/greet/world"));
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
    client.timeout(10s).unix_socket(fixture.socket_path);

    SECTION("POST JSON body is echoed back") {
        std::string body = R"({"name": "unix_test", "value": 42})";
        auto response = client.post(fixture.url("/echo-json"),
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

    server.patch("/resource", [](http::request& req, http::response& res) {
        res.json({{"method", "PATCH"}, {"body", req.body()}});
    });

    server.head("/resource", [](http::response& res) {
        res.status(http::http_response::status::ok);
        res.header("X-Method", "HEAD");
        res.send("");
    });

    server.options("/resource", [](http::response& res) {
        res.header("Allow", "GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS");
        res.send("");
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s).unix_socket(fixture.socket_path);

    SECTION("GET on /resource") {
        auto response = client.get(fixture.url("/resource"));
        REQUIRE(response.ok());
        REQUIRE(response.json()["method"] == "GET");
    }

    SECTION("POST on /resource") {
        auto response = client.post(fixture.url("/resource"),
                                    "", "text/plain");
        REQUIRE(response.ok());
        REQUIRE(response.json()["method"] == "POST");
    }

    SECTION("PUT on /resource") {
        auto response = client.put(fixture.url("/resource"),
                                   "test body", "text/plain");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["method"] == "PUT");
        REQUIRE(json["body"] == "test body");
    }

    SECTION("DELETE on /resource") {
        auto response = client.del(fixture.url("/resource"));
        REQUIRE(response.ok());
        REQUIRE(response.json()["method"] == "DELETE");
    }

    SECTION("PATCH on /resource") {
        auto response = client.patch(fixture.url("/resource"),
                                     "patch body", "text/plain");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["method"] == "PATCH");
        REQUIRE(json["body"] == "patch body");
    }

    SECTION("HEAD on /resource") {
        auto response = client.head(fixture.url("/resource"));
        REQUIRE(response.ok());
        REQUIRE(response.header("X-Method") == "HEAD");
    }

    SECTION("OPTIONS on /resource") {
        auto response = client.options(fixture.url("/resource"));
        REQUIRE(response.ok());
        REQUIRE(response.header("Allow").find("GET") != std::string::npos);
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
    client.timeout(10s).unix_socket(fixture.socket_path);

    SECTION("Multiple sequential requests succeed") {
        for (int i = 1; i <= 5; ++i) {
            auto response = client.get(fixture.url("/count"));
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
    client.timeout(10s).unix_socket(fixture.socket_path);

    SECTION("Send and receive custom headers") {
        http::headers_map headers = {{"X-Custom-Input", "ping"}};
        auto response = client.get(fixture.url("/headers"), headers);
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
    client.timeout(10s).unix_socket(fixture.socket_path);

    SECTION("Request to existing route returns 200") {
        auto response = client.get(fixture.url("/exists"));
        REQUIRE(response.ok());
        REQUIRE(response.json()["found"] == true);
    }

    SECTION("Request to non-existent route returns 404") {
        auto response = client.get(fixture.url("/does-not-exist"));
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
    client.timeout(30s).unix_socket(fixture.socket_path);

    SECTION("Large response body arrives intact") {
        auto response = client.get(fixture.url("/large"));
        REQUIRE(response.ok());
        REQUIRE(response.body().size() == large_size);
        REQUIRE(response.body() == large_body);
    }
}

// ============================================================================
// 9. Pool Server (connections accepted on a thread of the server, served on the workers)
// ============================================================================

TEST_CASE("Unix Socket pool server serves concurrent clients", "[unix][server][pool][integration]") {
    auto socket_path = temporary_socket_path("pool_test");
    std::filesystem::remove(socket_path);

    std::mutex mutex;
    std::set<std::thread::id> serving_threads;
    std::atomic<int> served{0};

    http::pool_server server;
    server.get("/hello", [&](http::response& res) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            serving_threads.insert(std::this_thread::get_id());
        }
        ++served;
        res.send("Hello World!");
    });
    REQUIRE(server.listen_unix(socket_path));
    REQUIRE(std::filesystem::exists(socket_path));

    constexpr int clients = 8;
    std::atomic<int> succeeded{0};

    SECTION("One connection per request") {
        constexpr int requests = 25;
        std::vector<std::thread> threads;
        for (int i = 0; i < clients; ++i) {
            threads.emplace_back([&] {
                for (int j = 0; j < requests; ++j) {
                    unix_client client(socket_path);
                    if (!client.connected()) continue;
                    if (!client.send("GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")) continue;
                    auto response = client.read_all();
                    if (response.starts_with("HTTP/1.1 200") && response.ends_with("Hello World!")) ++succeeded;
                }
            });
        }
        for (auto& thread : threads) thread.join();
        REQUIRE(succeeded == clients * requests);
        REQUIRE(served == clients * requests);
    }

    SECTION("Keep-alive connections") {
        constexpr int requests = 50;
        std::vector<std::thread> threads;
        for (int i = 0; i < clients; ++i) {
            threads.emplace_back([&] {
                unix_client client(socket_path);
                if (!client.connected()) return;
                for (int j = 0; j < requests; ++j) {
                    if (!client.send("GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n")) return;
                    auto response = client.read_response();
                    if (!response.starts_with("HTTP/1.1 200") || !response.ends_with("Hello World!")) return;
                    ++succeeded;
                }
            });
        }
        for (auto& thread : threads) thread.join();
        REQUIRE(succeeded == clients * requests);
        REQUIRE(served == clients * requests);

        // The connections are served on the workers, in turn
        if (std::thread::hardware_concurrency() > 1) {
            std::lock_guard<std::mutex> lock(mutex);
            REQUIRE(serving_threads.size() > 1);
        }
    }

    SECTION("Stop removes the socket and the server listens again") {
        REQUIRE(server.stop());
        REQUIRE_FALSE(server.is_listening());
        REQUIRE_FALSE(std::filesystem::exists(socket_path));
        {
            unix_client client(socket_path);
            REQUIRE_FALSE(client.connected());
        }

        REQUIRE(server.listen_unix(socket_path));
        unix_client client(socket_path);
        REQUIRE(client.connected());
        REQUIRE(client.send("GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
        auto response = client.read_all();
        REQUIRE(response.starts_with("HTTP/1.1 200"));
        REQUIRE(response.ends_with("Hello World!"));
    }

    server.stop();
    std::filesystem::remove(socket_path);
}

#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/util/types.hpp>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <chrono>
#include <thread>
#include <future>
#include <fstream>
#include <filesystem>

using namespace thinger;
using namespace std::chrono_literals;

// Test fixture for HTTP server base functionality
// Note: Routes and middleware must be added BEFORE calling start_server()
struct ServerBaseTestFixture {
    http::server server;
    uint16_t port = 0;
    std::string base_url;
    std::thread server_thread;
    bool server_started = false;

    ServerBaseTestFixture() = default;

    // Call this after setting up routes and middleware
    void start_server() {
        if (server_started) return;

        REQUIRE(server.listen("0.0.0.0", 0));
        port = server.local_port();
        base_url = "http://localhost:" + std::to_string(port);
        server_started = true;

        std::promise<void> ready;
        server_thread = std::thread([this, &ready]() {
            ready.set_value();
            server.wait();
        });
        ready.get_future().wait();
    }

    virtual ~ServerBaseTestFixture() {
        if (server_started) {
            server.stop();
            if (server_thread.joinable()) {
                server_thread.join();
            }
        }
    }
};

// ============================================================================
// Route Registration Tests - Different Callback Types
// ============================================================================

TEST_CASE("Server Route Callbacks - Response Only", "[server][routes][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // Register GET with response-only callback
    server.get("/response-only", [](http::response& res) {
        res.json({{"type", "response_only"}});
    });

    // Register POST with response-only callback
    server.post("/response-only-post", [](http::response& res) {
        res.json({{"type", "response_only_post"}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("GET with response-only callback") {
        auto response = client.get(base_url + "/response-only");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["type"] == "response_only");
    }

    SECTION("POST with response-only callback") {
        http::headers_map headers;
        auto response = client.post(base_url + "/response-only-post", "", "text/plain", headers);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["type"] == "response_only_post");
    }
}

TEST_CASE("Server Route Callbacks - JSON Body Parsing", "[server][routes][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // json_response_callback: json parameter contains parsed request body
    // Handler must call res.json() or res.send() to send response
    server.post("/echo-json", [](nlohmann::json& json, http::response& res) {
        // json contains the parsed request body
        json["echoed"] = true;  // Modify it
        res.json(json);         // Send it back
    });

    // Test with empty body - json will be empty
    server.post("/json-empty", [](nlohmann::json& json, http::response& res) {
        res.json({{"received_empty", json.empty()}, {"callback_type", "json_response"}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("POST with JSON body - json callback parses body") {
        http::headers_map headers;
        std::string body = R"({"name": "test", "value": 42})";
        auto response = client.post(base_url + "/echo-json", body, "application/json", headers);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["name"] == "test");
        REQUIRE(json["value"] == 42);
        REQUIRE(json["echoed"] == true);
    }

    SECTION("POST with empty body - json is empty object") {
        http::headers_map headers;
        auto response = client.post(base_url + "/json-empty", "", "application/json", headers);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["received_empty"] == true);
        REQUIRE(json["callback_type"] == "json_response");
    }
}

TEST_CASE("Server Route Callbacks - Request Response", "[server][routes][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // PUT with request/response callback
    server.put("/request-response", [](http::request& req, http::response& res) {
        auto body = req.body();
        res.json({{"callback_type", "request_response"}, {"received_body", body}});
    });

    // PATCH with request/response callback
    server.patch("/request-response-patch", [](http::request& req, http::response& res) {
        res.json({{"callback_type", "patch_request_response"}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("PUT with request/response callback") {
        http::headers_map headers;
        auto response = client.put(base_url + "/request-response", "test body", "text/plain", headers);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["callback_type"] == "request_response");
        REQUIRE(json["received_body"] == "test body");
    }

    SECTION("PATCH with request/response callback") {
        http::headers_map headers;
        auto response = client.patch(base_url + "/request-response-patch", "", "text/plain", headers);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["callback_type"] == "patch_request_response");
    }
}

// Note: Request JSON Response callback tests require testing with proper DELETE client support
TEST_CASE("Server Route Callbacks - Request JSON Response API", "[server][routes][unit]") {
    http::server server;

    SECTION("Can register DELETE with request/json response callback") {
        server.del("/request-json-response/:id", [](http::request& req, nlohmann::json& json, http::response& res) {
            json["callback_type"] = "request_json_response";
            json["deleted_id"] = req["id"];
        });
        REQUIRE(true);  // API acceptance test
    }

    SECTION("Can register PUT with request/json response callback") {
        server.put("/request-json-response", [](http::request& req, nlohmann::json& json, http::response& res) {
            json["callback_type"] = "request_json_response";
        });
        REQUIRE(true);  // API acceptance test
    }
}

// ============================================================================
// HEAD and OPTIONS Method Tests
// ============================================================================

TEST_CASE("Server HEAD Method", "[server][head][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // Register HEAD endpoint with response-only callback
    server.head("/head-test", [](http::response& res) {
        res.header("X-Custom-Header", "head-value");
        res.status(http::http_response::status::ok);
        res.send("");
    });

    // Register HEAD endpoint with request/response callback
    server.head("/head-with-request/:id", [](http::request& req, http::response& res) {
        res.header("X-Resource-Id", req["id"]);
        res.status(http::http_response::status::ok);
        res.send("");
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("HEAD with response-only callback") {
        auto response = client.head(base_url + "/head-test");
        REQUIRE(response.status() == 200);
        REQUIRE(response.header("X-Custom-Header") == "head-value");
    }

    SECTION("HEAD with request/response callback") {
        auto response = client.head(base_url + "/head-with-request/456");
        REQUIRE(response.status() == 200);
        REQUIRE(response.header("X-Resource-Id") == "456");
    }
}

TEST_CASE("Server OPTIONS Method", "[server][options][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // Register OPTIONS endpoint
    server.options("/options-test", [](http::response& res) {
        res.header("Allow", "GET, POST, OPTIONS");
        res.header("X-Options-Test", "custom-value");
        res.status(http::http_response::status::ok);
        res.send("");
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("OPTIONS request") {
        auto response = client.options(base_url + "/options-test");
        REQUIRE(response.status() == 200);
        REQUIRE(response.header("Allow") == "GET, POST, OPTIONS");
        REQUIRE(response.header("X-Options-Test") == "custom-value");
    }
}

// ============================================================================
// Middleware Tests
// ============================================================================

// Note: Middleware tests require middleware and routes to be set before server.listen()
// The current fixture starts the server in the constructor before middleware can be added
// TODO: Create a separate fixture that allows pre-listen configuration

TEST_CASE("Server Middleware Execution - modifies request", "[server][middleware][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // Middleware that sets auth user on the request
    server.use([](http::request& req, http::response& res, std::function<void()> next) {
        req.set_auth_user("middleware-user");
        next();
    });

    server.get("/middleware-test", [](http::request& req, http::response& res) {
        res.json({{"user", req.get_auth_user()}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Middleware modifies request before route handler") {
        auto response = client.get(base_url + "/middleware-test");
        REQUIRE(response.ok());
        REQUIRE(response.json()["user"] == "middleware-user");
    }
}

TEST_CASE("Server Middleware Blocks Request", "[server][middleware][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // Middleware that blocks requests without X-Api-Key header
    server.use([](http::request& req, http::response& res, std::function<void()> next) {
        auto http_request = req.get_http_request();
        if (http_request && http_request->get_header("X-Api-Key").empty()) {
            res.error(http::http_response::status::forbidden, "API key required");
            return;
        }
        next();
    });

    server.get("/protected", [](http::response& res) {
        res.json({{"secret", "data"}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Request without API key is blocked by middleware") {
        auto response = client.get(base_url + "/protected");
        REQUIRE(response.status() == 403);
        REQUIRE(response.body().find("API key required") != std::string::npos);
    }

    SECTION("Request with API key passes middleware") {
        http::headers_map headers = {{"X-Api-Key", "my-key"}};
        auto response = client.get(base_url + "/protected", headers);
        REQUIRE(response.ok());
        REQUIRE(response.json()["secret"] == "data");
    }
}

TEST_CASE("Server Multiple Middlewares Execute In Order", "[server][middleware][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // First middleware sets auth user
    server.use([](http::request& req, http::response& res, std::function<void()> next) {
        req.set_auth_user("first");
        next();
    });

    // Second middleware appends to auth user
    server.use([](http::request& req, http::response& res, std::function<void()> next) {
        req.set_auth_user(req.get_auth_user() + "+second");
        next();
    });

    server.get("/order-test", [](http::request& req, http::response& res) {
        res.json({{"order", req.get_auth_user()}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Both middlewares execute in order") {
        auto response = client.get(base_url + "/order-test");
        REQUIRE(response.ok());
        REQUIRE(response.json()["order"] == "first+second");
    }
}

// ============================================================================
// Not Found Handler Tests
// ============================================================================

// Note: Not Found handler tests require routes to be set before server.listen()
// The current fixture starts the server in the constructor before routes can be set
// TODO: Create a separate fixture that allows pre-listen configuration

TEST_CASE("Server Not Found Handler Integration", "[server][notfound][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // Set custom not-found handler with JSON response
    server.set_not_found_handler([](http::request& req, http::response& res) {
        auto http_request = req.get_http_request();
        std::string uri = http_request ? http_request->get_uri() : "unknown";
        res.json({{"error", "not_found"}, {"path", uri}}, http::http_response::status::not_found);
    });

    // One registered route for comparison
    server.get("/exists", [](http::response& res) {
        res.json({{"found", true}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Registered route works normally") {
        auto response = client.get(base_url + "/exists");
        REQUIRE(response.ok());
        REQUIRE(response.json()["found"] == true);
    }

    SECTION("Unregistered route triggers custom not-found handler") {
        auto response = client.get(base_url + "/does-not-exist");
        REQUIRE(response.status() == 404);
        auto json = response.json();
        REQUIRE(json["error"] == "not_found");
        REQUIRE(json["path"] == "/does-not-exist");
    }
}

// ============================================================================
// Server Configuration Tests
// ============================================================================

TEST_CASE("Server Configuration - Connection Timeout", "[server][config][integration]") {
    http::server server;

    SECTION("Set connection timeout") {
        server.set_connection_timeout(std::chrono::seconds(60));
        // Server should accept timeout without error
        REQUIRE(true);
    }

    SECTION("Set max listening attempts") {
        server.set_max_listening_attempts(5);
        // Server should accept setting without error
        REQUIRE(true);
    }
}

TEST_CASE("Server Configuration - CORS", "[server][config][cors][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    server.enable_cors(true);

    server.get("/cors-test", [](http::response& res) {
        res.json({{"cors", "enabled"}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("CORS headers are present on response") {
        auto response = client.get(base_url + "/cors-test");
        REQUIRE(response.ok());
        REQUIRE(response.header("Access-Control-Allow-Origin") == "*");
        REQUIRE(response.header("Access-Control-Allow-Methods").find("GET") != std::string::npos);
        REQUIRE(response.header("Access-Control-Allow-Methods").find("POST") != std::string::npos);
        REQUIRE(response.header("Access-Control-Allow-Methods").find("DELETE") != std::string::npos);
        REQUIRE(response.header("Access-Control-Allow-Headers").find("Content-Type") != std::string::npos);
        REQUIRE(response.header("Access-Control-Allow-Headers").find("Authorization") != std::string::npos);
        REQUIRE(response.header("Access-Control-Allow-Credentials") == "true");
    }

    SECTION("CORS preflight OPTIONS request") {
        auto response = client.options(base_url + "/cors-test");
        // OPTIONS on unregistered route — CORS headers should still be on matched routes
        // Test that the server doesn't crash on OPTIONS
    }
}

TEST_CASE("Server Configuration - SSL Enable/Disable", "[server][config][ssl][integration]") {
    http::server server;

    SECTION("Enable SSL") {
        server.enable_ssl(true);
        // Should not throw
        REQUIRE(true);
    }

    SECTION("Disable SSL") {
        server.enable_ssl(false);
        // Should not throw
        REQUIRE(true);
    }
}

// ============================================================================
// Server Control Tests
// ============================================================================

TEST_CASE("Server is_listening", "[server][control][integration]") {
    http::server server;

    SECTION("Server is not listening initially") {
        REQUIRE_FALSE(server.is_listening());
    }

    SECTION("Server is listening after start") {
        REQUIRE(server.listen("0.0.0.0", 0));

        std::promise<void> ready;
        std::thread t([&server, &ready]() {
            ready.set_value();
            server.wait();
        });
        ready.get_future().wait();

        REQUIRE(server.is_listening());

        server.stop();
        t.join();

        REQUIRE_FALSE(server.is_listening());
    }
}

TEST_CASE("Server Stop", "[server][control][integration]") {
    http::server server;

    SECTION("Stop on non-started server returns false") {
        REQUIRE_FALSE(server.stop());
    }

    SECTION("Stop on running server returns true") {
        REQUIRE(server.listen("0.0.0.0", 0));

        std::promise<void> ready;
        std::thread t([&server, &ready]() {
            ready.set_value();
            server.wait();
        });
        ready.get_future().wait();

        REQUIRE(server.stop());
        t.join();
    }
}

// ============================================================================
// Static File Serving Tests
// ============================================================================

TEST_CASE("Server Static File Serving", "[server][static][integration]") {
    namespace fs = std::filesystem;

    // Create a temporary directory with test files
    auto temp_dir = fs::temp_directory_path() / "thinger_static_test";
    fs::create_directories(temp_dir);

    // Create test file
    {
        std::ofstream file(temp_dir / "test.txt");
        file << "Hello from static file";
    }

    // Create index.html
    {
        std::ofstream file(temp_dir / "index.html");
        file << "<html><body>Index</body></html>";
    }

    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    server.serve_static("/static", temp_dir.string(), true);

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Request non-existent static file returns 404") {
        auto response = client.get(base_url + "/static/nonexistent.txt");
        REQUIRE(response.status() == 404);
    }

    SECTION("Serve existing text file with correct MIME type") {
        auto response = client.get(base_url + "/static/test.txt");
        REQUIRE(response.status() == 200);
        REQUIRE(response.body() == "Hello from static file");
        REQUIRE(response.content_type().find("text/plain") != std::string::npos);
    }

    SECTION("Serve index.html for subdirectory with correct MIME type") {
        // Create a subdirectory with an index.html
        auto subdir = temp_dir / "subdir";
        fs::create_directories(subdir);
        {
            std::ofstream file(subdir / "index.html");
            file << "<html><body>Subdir Index</body></html>";
        }
        auto response = client.get(base_url + "/static/subdir");
        REQUIRE(response.ok());
        REQUIRE(response.body() == "<html><body>Subdir Index</body></html>");
        REQUIRE(response.content_type().find("text/html") != std::string::npos);
    }

    // Cleanup
    fs::remove_all(temp_dir);
}

// ============================================================================
// Router Access Tests
// ============================================================================

TEST_CASE("Server Router Access", "[server][router][integration]") {
    http::server server;

    SECTION("Non-const router access") {
        auto& router = server.router();
        // Should be able to access router
        REQUIRE(true);
    }

    SECTION("Const router access") {
        const http::server& const_server = server;
        const auto& router = const_server.router();
        // Should be able to access const router
        REQUIRE(true);
    }
}

// ============================================================================
// Route Chaining Tests
// ============================================================================

TEST_CASE("Server Route Chaining", "[server][routes][chaining][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // Routes should return route& for chaining
    server.get("/chain-test", [](http::response& res) {
        res.json({{"chained", true}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Chained route works") {
        auto response = client.get(base_url + "/chain-test");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["chained"] == true);
    }
}

// ============================================================================
// Multiple HTTP Methods on Same Path Tests
// ============================================================================

TEST_CASE("Server Multiple Methods Same Path", "[server][routes][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // Register multiple methods for the same path
    server.get("/resource", [](http::response& res) {
        res.json({{"method", "GET"}});
    });

    server.post("/resource", [](http::response& res) {
        res.json({{"method", "POST"}});
    });

    server.put("/resource", [](http::response& res) {
        res.json({{"method", "PUT"}});
    });

    server.del("/resource", [](http::response& res) {
        res.json({{"method", "DELETE"}});
    });

    server.patch("/resource", [](http::response& res) {
        res.json({{"method", "PATCH"}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("GET on multi-method path") {
        auto response = client.get(base_url + "/resource");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["method"] == "GET");
    }

    SECTION("POST on multi-method path") {
        http::headers_map headers;
        auto response = client.post(base_url + "/resource", "", "text/plain", headers);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["method"] == "POST");
    }

    SECTION("PUT on multi-method path") {
        http::headers_map headers;
        auto response = client.put(base_url + "/resource", "", "text/plain", headers);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["method"] == "PUT");
    }

    SECTION("DELETE on multi-method path") {
        auto response = client.del(base_url + "/resource");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["method"] == "DELETE");
    }

    SECTION("PATCH on multi-method path") {
        http::headers_map headers;
        auto response = client.patch(base_url + "/resource", "", "text/plain", headers);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["method"] == "PATCH");
    }
}

// ============================================================================
// Basic Auth Tests
// ============================================================================

TEST_CASE("Server Basic Auth", "[server][auth][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // Set up basic auth with single user
    server.set_basic_auth("/protected", "Test Realm", "admin", "secret");

    // Protected route that returns the authenticated username
    server.get("/protected/data", [](http::request& req, http::response& res) {
        res.json({{"user", req.get_auth_user()}, {"ok", true}});
    });

    // Public route for comparison
    server.get("/public", [](http::response& res) {
        res.json({{"public", true}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Request without credentials returns 401") {
        auto response = client.get(base_url + "/protected/data");
        REQUIRE(response.status() == 401);
    }

    SECTION("Request with valid credentials returns 200") {
        // "admin:secret" -> base64 -> "YWRtaW46c2VjcmV0"
        http::headers_map headers = {{"Authorization", "Basic YWRtaW46c2VjcmV0"}};
        auto response = client.get(base_url + "/protected/data", headers);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["user"] == "admin");
        REQUIRE(json["ok"] == true);
    }

    SECTION("Request with wrong credentials returns 401") {
        // "admin:wrong" -> base64 -> "YWRtaW46d3Jvbmc="
        http::headers_map headers = {{"Authorization", "Basic YWRtaW46d3Jvbmc="}};
        auto response = client.get(base_url + "/protected/data", headers);
        REQUIRE(response.status() == 401);
    }

    SECTION("Public route remains accessible") {
        auto response = client.get(base_url + "/public");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["public"] == true);
    }

    SECTION("Non-Basic auth scheme returns 401") {
        http::headers_map headers = {{"Authorization", "Bearer some-token"}};
        auto response = client.get(base_url + "/protected/data", headers);
        REQUIRE(response.status() == 401);
    }

    SECTION("WWW-Authenticate header includes realm") {
        auto response = client.get(base_url + "/protected/data");
        REQUIRE(response.status() == 401);
        std::string www_auth = response.header("WWW-Authenticate");
        REQUIRE(www_auth.find("Basic") != std::string::npos);
        REQUIRE(www_auth.find("Test Realm") != std::string::npos);
    }
}

TEST_CASE("Server Basic Auth with Multiple Users", "[server][auth][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // Set up basic auth with multiple users
    std::map<std::string, std::string> users = {
        {"alice", "password1"},
        {"bob", "password2"}
    };
    server.set_basic_auth("/api", "API Realm", users);

    server.get("/api/data", [](http::request& req, http::response& res) {
        res.json({{"user", req.get_auth_user()}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Alice can authenticate") {
        // "alice:password1" -> base64 -> "YWxpY2U6cGFzc3dvcmQx"
        http::headers_map headers = {{"Authorization", "Basic YWxpY2U6cGFzc3dvcmQx"}};
        auto response = client.get(base_url + "/api/data", headers);
        REQUIRE(response.ok());
        REQUIRE(response.json()["user"] == "alice");
    }

    SECTION("Bob can authenticate") {
        // "bob:password2" -> base64 -> "Ym9iOnBhc3N3b3JkMg=="
        http::headers_map headers = {{"Authorization", "Basic Ym9iOnBhc3N3b3JkMg=="}};
        auto response = client.get(base_url + "/api/data", headers);
        REQUIRE(response.ok());
        REQUIRE(response.json()["user"] == "bob");
    }

    SECTION("Unknown user is rejected") {
        // "charlie:pass" -> base64 -> "Y2hhcmxpZTpwYXNz"
        http::headers_map headers = {{"Authorization", "Basic Y2hhcmxpZTpwYXNz"}};
        auto response = client.get(base_url + "/api/data", headers);
        REQUIRE(response.status() == 401);
    }
}

TEST_CASE("Server Basic Auth with Verify Function", "[server][auth][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // Custom verify function that accepts any user with password "master"
    server.set_basic_auth("/secure", "Secure Realm",
        [](const std::string& user, const std::string& pass) {
            return pass == "master";
        });

    server.get("/secure/info", [](http::request& req, http::response& res) {
        res.json({{"user", req.get_auth_user()}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Any user with correct password passes") {
        // "anyone:master" -> base64 -> "YW55b25lOm1hc3Rlcg=="
        http::headers_map headers = {{"Authorization", "Basic YW55b25lOm1hc3Rlcg=="}};
        auto response = client.get(base_url + "/secure/info", headers);
        REQUIRE(response.ok());
        REQUIRE(response.json()["user"] == "anyone");
    }

    SECTION("Wrong password is rejected") {
        // "anyone:wrong" -> base64 -> "YW55b25lOndyb25n"
        http::headers_map headers = {{"Authorization", "Basic YW55b25lOndyb25n"}};
        auto response = client.get(base_url + "/secure/info", headers);
        REQUIRE(response.status() == 401);
    }
}

// ============================================================================
// Chunked Response Tests
// ============================================================================

TEST_CASE("Server Chunked Response", "[server][chunked][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // Route that sends multiple chunks
    server.get("/chunked/multi", [](http::response& res) {
        res.start_chunked("text/plain");
        res.write_chunk("Hello ");
        res.write_chunk("World");
        res.write_chunk("!");
        res.end_chunked();
    });

    // Route that sends a single chunk
    server.get("/chunked/single", [](http::response& res) {
        res.start_chunked("application/json");
        res.write_chunk(R"({"chunked":true})");
        res.end_chunked();
    });

    // Route that sends an empty chunked response (just headers + terminator)
    server.get("/chunked/empty", [](http::response& res) {
        res.start_chunked("text/plain");
        res.end_chunked();
    });

    // Route with custom status code
    server.get("/chunked/created", [](http::response& res) {
        res.start_chunked("text/plain", http::http_response::status::created);
        res.write_chunk("resource created");
        res.end_chunked();
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Multiple chunks are reassembled into complete body") {
        auto response = client.get(base_url + "/chunked/multi");
        REQUIRE(response.ok());
        REQUIRE(response.body() == "Hello World!");
        REQUIRE(response.content_type().find("text/plain") != std::string::npos);
    }

    SECTION("Single chunk with JSON content type") {
        auto response = client.get(base_url + "/chunked/single");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["chunked"] == true);
        REQUIRE(response.content_type().find("application/json") != std::string::npos);
    }

    SECTION("Empty chunked response returns empty body") {
        auto response = client.get(base_url + "/chunked/empty");
        REQUIRE(response.ok());
        REQUIRE(response.body().empty());
    }

    SECTION("Chunked response with custom status code") {
        auto response = client.get(base_url + "/chunked/created");
        REQUIRE(response.status() == 201);
        REQUIRE(response.body() == "resource created");
    }
}

// ============================================================================
// On-Demand Body Reading Tests (coroutine-based body read)
// ============================================================================

TEST_CASE("Server POST with JSON body - backward compat", "[server][body][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    server.post("/echo", [](http::request& req, http::response& res) {
        res.json({{"body", req.body()}, {"ok", true}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("POST with JSON body arrives correctly") {
        http::headers_map headers;
        std::string body = R"({"name":"test","value":42})";
        auto response = client.post(base_url + "/echo", body, "application/json", headers);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["body"] == body);
        REQUIRE(json["ok"] == true);
    }
}

TEST_CASE("Server POST body exceeding max_body_size returns 413", "[server][body][413][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // Set a very small max body size for testing
    server.set_max_body_size(64);

    server.post("/upload", [](http::request& req, http::response& res) {
        res.json({{"size", req.body().size()}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Body within limit succeeds") {
        http::headers_map headers;
        std::string small_body(32, 'x');
        auto response = client.post(base_url + "/upload", small_body, "text/plain", headers);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["size"] == 32);
    }

    SECTION("Body exceeding limit returns 413") {
        http::headers_map headers;
        std::string large_body(128, 'x');
        auto response = client.post(base_url + "/upload", large_body, "text/plain", headers);
        REQUIRE(response.status() == 413);
    }
}

TEST_CASE("Server large body (1MB) arrives correctly", "[server][body][large][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    server.post("/large", [](http::request& req, http::response& res) {
        // Return the size and a checksum (sum of all bytes)
        size_t sum = 0;
        for (unsigned char c : req.body()) sum += c;
        res.json({{"size", req.body().size()}, {"checksum", sum}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(30s);

    SECTION("1MB body arrives intact") {
        http::headers_map headers;
        size_t body_size = 1024 * 1024;
        std::string body(body_size, '\0');
        // Fill with a pattern
        size_t expected_sum = 0;
        for (size_t i = 0; i < body_size; ++i) {
            body[i] = static_cast<char>(i % 256);
            expected_sum += static_cast<unsigned char>(body[i]);
        }

        auto response = client.post(base_url + "/large", body, "application/octet-stream", headers);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["size"] == body_size);
        REQUIRE(json["checksum"] == expected_sum);
    }
}

// ============================================================================
// HTTP Pipelining Tests (raw TCP to send multiple requests in one shot)
// ============================================================================

TEST_CASE("Server HTTP pipelining - two GET requests in one TCP send", "[server][pipelining][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;

    std::atomic<int> call_count{0};

    server.get("/ping", [&call_count](http::response& res) {
        call_count++;
        res.json({{"pong", true}, {"count", call_count.load()}});
    });

    fixture.start_server();

    SECTION("Two pipelined GET requests both receive responses") {
        // Use raw TCP to send both requests in a single write
        boost::asio::io_context ioc;
        boost::asio::ip::tcp::socket sock(ioc);
        boost::asio::ip::tcp::resolver resolver(ioc);
        auto results = resolver.resolve("127.0.0.1", std::to_string(fixture.port));
        boost::asio::connect(sock, results);

        // Two complete HTTP requests concatenated
        std::string pipelined =
            "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"
            "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

        boost::asio::write(sock, boost::asio::buffer(pipelined));

        // Read all responses
        boost::system::error_code ec;
        boost::asio::streambuf response_buf;
        boost::asio::read(sock, response_buf, ec);
        // We expect eof after the second response (Connection: close)

        std::string response_str(
            boost::asio::buffers_begin(response_buf.data()),
            boost::asio::buffers_end(response_buf.data()));

        // Both responses should be present
        // Count "HTTP/1.1 200" occurrences
        size_t count = 0;
        size_t pos = 0;
        while ((pos = response_str.find("HTTP/1.1 200", pos)) != std::string::npos) {
            count++;
            pos += 12;
        }
        REQUIRE(count == 2);

        // Wait for handlers to complete
        std::this_thread::sleep_for(100ms);
        REQUIRE(call_count >= 2);
    }
}

// ============================================================================
// Deferred Body Mode Tests (Phase 2 — streaming upload)
// ============================================================================

TEST_CASE("Deferred body route - streaming upload 1MB with correct checksum", "[server][deferred][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // Deferred body route: reads body chunk-by-chunk
    server.put("/upload/:filename", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
        auto cl = req.content_length();
        uint8_t buffer[8192];
        size_t total = 0;
        uint8_t checksum = 0;

        while (total < cl) {
            size_t to_read = std::min(sizeof(buffer), cl - total);
            size_t bytes = co_await req.read(buffer, to_read);
            if (bytes == 0) break;
            for (size_t i = 0; i < bytes; i++) checksum ^= buffer[i];
            total += bytes;
        }

        res.json({{"bytes_received", total}, {"xor_checksum", checksum}, {"filename", req["filename"]}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(30s);

    SECTION("1MB streaming upload arrives with correct checksum") {
        size_t body_size = 1024 * 1024;
        std::string body(body_size, '\0');
        uint8_t expected_checksum = 0;
        for (size_t i = 0; i < body_size; ++i) {
            body[i] = static_cast<char>(i % 256);
            expected_checksum ^= static_cast<uint8_t>(body[i]);
        }

        http::headers_map headers;
        auto response = client.put(base_url + "/upload/testfile.bin", body, "application/octet-stream", headers);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["bytes_received"] == body_size);
        REQUIRE(json["xor_checksum"] == expected_checksum);
        REQUIRE(json["filename"] == "testfile.bin");
    }
}

TEST_CASE("Deferred body route - read_some with small chunks", "[server][deferred][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // Deferred body route using read_some (returns whatever is available)
    server.post("/stream", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
        auto cl = req.content_length();
        uint8_t buffer[256];  // small buffer to force multiple read_some calls
        size_t total = 0;
        size_t chunk_count = 0;

        while (total < cl) {
            size_t bytes = co_await req.read_some(buffer, sizeof(buffer));
            if (bytes == 0) break;
            total += bytes;
            chunk_count++;
        }

        res.json({{"bytes_received", total}, {"chunk_count", chunk_count}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("read_some accumulates correct total") {
        http::headers_map headers;
        std::string body(4096, 'A');
        auto response = client.post(base_url + "/stream", body, "text/plain", headers);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["bytes_received"] == 4096);
        REQUIRE(json["chunk_count"] > 0);
    }
}

TEST_CASE("Deferred body + non-deferred on same server", "[server][deferred][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // Non-deferred route (normal callback)
    server.get("/status", [](http::response& res) {
        res.json({{"status", "ok"}});
    });

    // Non-deferred POST (body auto-read before handler)
    server.post("/echo", [](http::request& req, http::response& res) {
        res.json({{"body", req.body()}});
    });

    // Deferred body route
    server.put("/deferred", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
        auto cl = req.content_length();
        uint8_t buf[4096];
        size_t total = 0;
        while (total < cl) {
            size_t bytes = co_await req.read(buf, std::min(sizeof(buf), cl - total));
            if (bytes == 0) break;
            total += bytes;
        }
        res.json({{"deferred_bytes", total}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Non-deferred GET works") {
        auto response = client.get(base_url + "/status");
        REQUIRE(response.ok());
        REQUIRE(response.json()["status"] == "ok");
    }

    SECTION("Non-deferred POST with body works") {
        http::headers_map headers;
        auto response = client.post(base_url + "/echo", "hello", "text/plain", headers);
        REQUIRE(response.ok());
        REQUIRE(response.json()["body"] == "hello");
    }

    SECTION("Deferred PUT works") {
        http::headers_map headers;
        std::string body(1024, 'B');
        auto response = client.put(base_url + "/deferred", body, "application/octet-stream", headers);
        REQUIRE(response.ok());
        REQUIRE(response.json()["deferred_bytes"] == 1024);
    }
}

TEST_CASE("413 still works for non-deferred routes", "[server][deferred][413][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;
    auto& base_url = fixture.base_url;

    // Set a small max body size
    server.set_max_body_size(64);

    // Non-deferred route
    server.post("/upload", [](http::request& req, http::response& res) {
        res.json({{"size", req.body().size()}});
    });

    fixture.start_server();
    http::client client;
    client.timeout(10s);

    SECTION("Body within limit succeeds") {
        http::headers_map headers;
        std::string small_body(32, 'x');
        auto response = client.post(base_url + "/upload", small_body, "text/plain", headers);
        REQUIRE(response.ok());
        REQUIRE(response.json()["size"] == 32);
    }

    SECTION("Body exceeding limit returns 413") {
        http::headers_map headers;
        std::string large_body(128, 'x');
        auto response = client.post(base_url + "/upload", large_body, "text/plain", headers);
        REQUIRE(response.status() == 413);
    }
}

TEST_CASE("HTTP pipelining - deferred body request followed by GET", "[server][deferred][pipelining][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;

    // Deferred body route
    server.put("/data", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
        auto cl = req.content_length();
        uint8_t buf[4096];
        size_t total = 0;
        while (total < cl) {
            size_t bytes = co_await req.read(buf, std::min(sizeof(buf), cl - total));
            if (bytes == 0) break;
            total += bytes;
        }
        res.json({{"put_bytes", total}});
    });

    // Simple GET
    server.get("/check", [](http::response& res) {
        res.json({{"check", "ok"}});
    });

    fixture.start_server();

    SECTION("Pipelined PUT + GET both get responses") {
        boost::asio::io_context ioc;
        boost::asio::ip::tcp::socket sock(ioc);
        boost::asio::ip::tcp::resolver resolver(ioc);
        auto results = resolver.resolve("127.0.0.1", std::to_string(fixture.port));
        boost::asio::connect(sock, results);

        // Build pipelined request: PUT with body, then GET
        std::string body(64, 'Z');
        std::string pipelined =
            "PUT /data HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nConnection: keep-alive\r\n\r\n" + body +
            "GET /check HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

        boost::asio::write(sock, boost::asio::buffer(pipelined));

        // Read all responses
        boost::system::error_code ec;
        boost::asio::streambuf response_buf;
        boost::asio::read(sock, response_buf, ec);

        std::string response_str(
            boost::asio::buffers_begin(response_buf.data()),
            boost::asio::buffers_end(response_buf.data()));

        // Count "HTTP/1.1 200" occurrences — expect 2
        size_t count = 0;
        size_t pos = 0;
        while ((pos = response_str.find("HTTP/1.1 200", pos)) != std::string::npos) {
            count++;
            pos += 12;
        }
        REQUIRE(count == 2);

        // Verify both response bodies are present
        REQUIRE(response_str.find("put_bytes") != std::string::npos);
        REQUIRE(response_str.find("check") != std::string::npos);
    }
}

// ============================================================================
// Chunked Transfer-Encoding Request Tests (raw TCP)
// ============================================================================

// Helper: send raw HTTP request and read full response until connection closes
static std::string raw_http_exchange(uint16_t port, const std::string& raw_request) {
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::socket sock(ioc);
    boost::asio::ip::tcp::resolver resolver(ioc);
    auto results = resolver.resolve("127.0.0.1", std::to_string(port));
    boost::asio::connect(sock, results);

    boost::asio::write(sock, boost::asio::buffer(raw_request));

    boost::system::error_code ec;
    boost::asio::streambuf response_buf;
    boost::asio::read(sock, response_buf, ec);

    return std::string(
        boost::asio::buffers_begin(response_buf.data()),
        boost::asio::buffers_end(response_buf.data()));
}

TEST_CASE("Chunked request - non-deferred route receives decoded body", "[server][chunked-request][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;

    // Non-deferred route: body is auto-read before handler
    server.post("/echo", [](http::request& req, http::response& res) {
        res.json({{"body", req.body()}, {"size", req.body().size()}});
    });

    fixture.start_server();

    SECTION("Chunked POST body is transparently decoded") {
        // Send: "Hello " (6 bytes) + "World!" (6 bytes) = "Hello World!" (12 bytes)
        std::string raw =
            "POST /echo HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: close\r\n"
            "\r\n"
            "6\r\n"
            "Hello \r\n"
            "6\r\n"
            "World!\r\n"
            "0\r\n"
            "\r\n";

        std::string response = raw_http_exchange(fixture.port, raw);

        REQUIRE(response.find("HTTP/1.1 200") != std::string::npos);
        REQUIRE(response.find("\"body\":\"Hello World!\"") != std::string::npos);
        REQUIRE(response.find("\"size\":12") != std::string::npos);
    }
}

TEST_CASE("Chunked request - deferred route reads chunks manually", "[server][chunked-request][deferred][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;

    // Deferred body route: reads chunked body manually
    server.post("/stream-chunked", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
        uint8_t buffer[4096];
        size_t total = 0;
        uint8_t xor_checksum = 0;

        while (true) {
            size_t bytes = co_await req.read_some(buffer, sizeof(buffer));
            if (bytes == 0) break;
            for (size_t i = 0; i < bytes; i++) xor_checksum ^= buffer[i];
            total += bytes;
        }

        res.json({{"bytes_received", total}, {"xor_checksum", xor_checksum}});
    });

    fixture.start_server();

    SECTION("Deferred route decodes chunked data correctly") {
        // Build chunked body: 3 chunks of 100 bytes each = 300 bytes total
        std::string body_data(300, '\0');
        uint8_t expected_checksum = 0;
        for (size_t i = 0; i < 300; i++) {
            body_data[i] = static_cast<char>(i % 256);
            expected_checksum ^= static_cast<uint8_t>(body_data[i]);
        }

        // Encode as 3 chunks of 100 bytes
        std::string chunked_body;
        chunked_body += "64\r\n";  // 100 in hex
        chunked_body += body_data.substr(0, 100);
        chunked_body += "\r\n";
        chunked_body += "64\r\n";  // 100 in hex
        chunked_body += body_data.substr(100, 100);
        chunked_body += "\r\n";
        chunked_body += "64\r\n";  // 100 in hex
        chunked_body += body_data.substr(200, 100);
        chunked_body += "\r\n";
        chunked_body += "0\r\n\r\n";  // terminator

        std::string raw =
            "POST /stream-chunked HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: close\r\n"
            "\r\n" + chunked_body;

        std::string response = raw_http_exchange(fixture.port, raw);

        REQUIRE(response.find("HTTP/1.1 200") != std::string::npos);
        REQUIRE(response.find("\"bytes_received\":300") != std::string::npos);
        REQUIRE(response.find("\"xor_checksum\":" + std::to_string(expected_checksum)) != std::string::npos);
    }
}

TEST_CASE("Chunked request - exceeding max_body_size returns 413", "[server][chunked-request][413][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;

    // Set very small max body size
    server.set_max_body_size(32);

    server.post("/limited", [](http::request& req, http::response& res) {
        res.json({{"size", req.body().size()}});
    });

    fixture.start_server();

    SECTION("Small chunked body within limit succeeds") {
        std::string raw =
            "POST /limited HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: close\r\n"
            "\r\n"
            "a\r\n"        // 10 bytes
            "0123456789\r\n"
            "0\r\n\r\n";

        std::string response = raw_http_exchange(fixture.port, raw);
        REQUIRE(response.find("HTTP/1.1 200") != std::string::npos);
        REQUIRE(response.find("\"size\":10") != std::string::npos);
    }

    SECTION("Chunked body exceeding limit returns 413") {
        // Send 64 bytes in chunks (exceeds 32 byte limit)
        std::string data(64, 'X');
        std::string raw =
            "POST /limited HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: close\r\n"
            "\r\n"
            "40\r\n" +     // 64 in hex
            data + "\r\n"
            "0\r\n\r\n";

        std::string response = raw_http_exchange(fixture.port, raw);
        REQUIRE(response.find("HTTP/1.1 413") != std::string::npos);
    }
}

TEST_CASE("Chunked request - pipelining with chunked then GET", "[server][chunked-request][pipelining][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;

    server.post("/chunked-echo", [](http::request& req, http::response& res) {
        res.json({{"body", req.body()}});
    });

    server.get("/health", [](http::response& res) {
        res.json({{"status", "ok"}});
    });

    fixture.start_server();

    SECTION("Chunked POST followed by GET in pipelined request") {
        std::string pipelined =
            "POST /chunked-echo HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            "5\r\n"
            "hello\r\n"
            "0\r\n"
            "\r\n"
            "GET /health HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n";

        std::string response = raw_http_exchange(fixture.port, pipelined);

        // Count HTTP 200 responses — expect 2
        size_t count = 0;
        size_t pos = 0;
        while ((pos = response.find("HTTP/1.1 200", pos)) != std::string::npos) {
            count++;
            pos += 12;
        }
        REQUIRE(count == 2);

        // Both bodies present
        REQUIRE(response.find("\"body\":\"hello\"") != std::string::npos);
        REQUIRE(response.find("\"status\":\"ok\"") != std::string::npos);
    }
}

// ============================================================================
// Keep-Alive and Connection Timeout Tests
// ============================================================================

// Helper: connect a raw TCP socket to the test server
static boost::asio::ip::tcp::socket raw_connect(boost::asio::io_context& ioc, uint16_t port) {
    boost::asio::ip::tcp::socket sock(ioc);
    boost::asio::ip::tcp::resolver resolver(ioc);
    auto results = resolver.resolve("127.0.0.1", std::to_string(port));
    boost::asio::connect(sock, results);
    return sock;
}

// Helper: read one HTTP response from a keep-alive connection.
// Parses headers to find Content-Length, then reads exactly that many body bytes.
static std::string read_one_response(boost::asio::ip::tcp::socket& sock,
                                      boost::asio::streambuf& buf) {
    boost::system::error_code ec;

    // Read until end of headers
    boost::asio::read_until(sock, buf, "\r\n\r\n", ec);
    if (ec) return "";

    // Snapshot current buffer as string
    std::string data(boost::asio::buffers_begin(buf.data()),
                     boost::asio::buffers_end(buf.data()));

    // Parse Content-Length
    size_t content_length = 0;
    auto cl_pos = data.find("Content-Length: ");
    if (cl_pos != std::string::npos) {
        auto cl_end = data.find("\r\n", cl_pos);
        content_length = std::stoul(data.substr(cl_pos + 16, cl_end - cl_pos - 16));
    }

    // Determine how much body we still need
    auto header_end = data.find("\r\n\r\n");
    size_t total_needed = header_end + 4 + content_length;

    if (buf.size() < total_needed) {
        boost::asio::read(sock, buf,
            boost::asio::transfer_exactly(total_needed - buf.size()), ec);
    }

    // Extract this response and consume from buffer
    std::string all(boost::asio::buffers_begin(buf.data()),
                    boost::asio::buffers_end(buf.data()));
    std::string response = all.substr(0, total_needed);
    buf.consume(total_needed);
    return response;
}

TEST_CASE("Server Keep-Alive behavior", "[server][keepalive][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;

    server.get("/ping", [](http::response& res) {
        res.json({{"pong", true}});
    });

    fixture.start_server();

    SECTION("HTTP/1.1 defaults to keep-alive — multiple requests on same connection") {
        boost::asio::io_context ioc;
        auto sock = raw_connect(ioc, fixture.port);
        boost::asio::streambuf buf;

        // First request (no explicit Connection header — HTTP/1.1 defaults to keep-alive)
        std::string req1 = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
        boost::asio::write(sock, boost::asio::buffer(req1));
        auto resp1 = read_one_response(sock, buf);
        REQUIRE(resp1.find("HTTP/1.1 200") != std::string::npos);
        REQUIRE(resp1.find("\"pong\":true") != std::string::npos);

        // Second request on same connection
        std::string req2 = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
        boost::asio::write(sock, boost::asio::buffer(req2));
        auto resp2 = read_one_response(sock, buf);
        REQUIRE(resp2.find("HTTP/1.1 200") != std::string::npos);
        REQUIRE(resp2.find("\"pong\":true") != std::string::npos);

        // Third request — still alive
        std::string req3 = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
        boost::asio::write(sock, boost::asio::buffer(req3));
        auto resp3 = read_one_response(sock, buf);
        REQUIRE(resp3.find("HTTP/1.1 200") != std::string::npos);

        sock.close();
    }

    SECTION("Connection: close causes server to close connection after response") {
        boost::asio::io_context ioc;
        auto sock = raw_connect(ioc, fixture.port);

        // Send request with Connection: close
        std::string req =
            "GET /ping HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n";
        boost::asio::write(sock, boost::asio::buffer(req));

        // Read until server closes the connection
        boost::system::error_code ec;
        boost::asio::streambuf response_buf;
        boost::asio::read(sock, response_buf, ec);

        // Server should close — we get EOF
        REQUIRE(ec == boost::asio::error::eof);

        std::string response(boost::asio::buffers_begin(response_buf.data()),
                             boost::asio::buffers_end(response_buf.data()));
        REQUIRE(response.find("HTTP/1.1 200") != std::string::npos);
        REQUIRE(response.find("\"pong\":true") != std::string::npos);
    }

    SECTION("Keep-alive then close on last request") {
        boost::asio::io_context ioc;
        auto sock = raw_connect(ioc, fixture.port);
        boost::asio::streambuf buf;

        // First request with keep-alive
        std::string req1 =
            "GET /ping HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
        boost::asio::write(sock, boost::asio::buffer(req1));
        auto resp1 = read_one_response(sock, buf);
        REQUIRE(resp1.find("HTTP/1.1 200") != std::string::npos);

        // Second request with close
        std::string req2 =
            "GET /ping HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n";
        boost::asio::write(sock, boost::asio::buffer(req2));

        // Read remaining data — server will close after this response
        boost::system::error_code ec;
        boost::asio::streambuf tail;
        boost::asio::read(sock, tail, ec);
        REQUIRE(ec == boost::asio::error::eof);

        std::string resp2(boost::asio::buffers_begin(tail.data()),
                          boost::asio::buffers_end(tail.data()));
        REQUIRE(resp2.find("HTTP/1.1 200") != std::string::npos);
    }
}

TEST_CASE("Server Connection Timeout", "[server][timeout][keepalive][integration]") {
    ServerBaseTestFixture fixture;
    auto& server = fixture.server;

    // Short timeout for testing
    server.set_connection_timeout(std::chrono::seconds(2));

    server.get("/ping", [](http::response& res) {
        res.json({{"pong", true}});
    });

    fixture.start_server();

    SECTION("Idle connection is closed after timeout") {
        boost::asio::io_context ioc;
        auto sock = raw_connect(ioc, fixture.port);
        boost::asio::streambuf buf;

        // Send a request to establish the connection
        std::string req = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
        boost::asio::write(sock, boost::asio::buffer(req));
        auto resp = read_one_response(sock, buf);
        REQUIRE(resp.find("HTTP/1.1 200") != std::string::npos);

        // Wait longer than the 2s timeout
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));

        // Try to read — server should have closed the connection
        char tmp[64];
        boost::system::error_code ec;
        sock.read_some(boost::asio::buffer(tmp), ec);
        REQUIRE(ec); // EOF or connection_reset
    }

    SECTION("Timeout resets with each request — connection survives beyond initial timeout") {
        boost::asio::io_context ioc;
        auto sock = raw_connect(ioc, fixture.port);
        boost::asio::streambuf buf;

        // Send 3 requests with 1.5s gaps (total ~3s, exceeds 2s timeout)
        // If the timeout resets on each request, all should succeed
        for (int i = 0; i < 3; i++) {
            if (i > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            }

            std::string req = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
            boost::system::error_code write_ec;
            boost::asio::write(sock, boost::asio::buffer(req), write_ec);
            REQUIRE_FALSE(write_ec);

            auto resp = read_one_response(sock, buf);
            REQUIRE(resp.find("HTTP/1.1 200") != std::string::npos);
        }
        // Total elapsed ~3s > 2s timeout, but connection is still alive
        // because each request reset the timer

        sock.close();
    }

    SECTION("Fresh connection with no request is closed after timeout") {
        boost::asio::io_context ioc;
        auto sock = raw_connect(ioc, fixture.port);

        // Don't send anything — just wait
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));

        // Server should have closed the idle connection
        char tmp[64];
        boost::system::error_code ec;
        sock.read_some(boost::asio::buffer(tmp), ec);
        REQUIRE(ec);
    }
}

#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/client/client.hpp>
#include <nlohmann/json.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <chrono>
#include <thread>
#include <fstream>
#include <filesystem>

using namespace thinger;
using namespace std::chrono_literals;

// Test fixture for server request/response tests
struct RequestResponseTestFixture {
    http::server server;
    uint16_t port = 9500;
    std::string base_url;
    std::thread server_thread;

    RequestResponseTestFixture() {
        setup_endpoints();
        start_server();
    }

    ~RequestResponseTestFixture() {
        server.stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

private:
    void setup_endpoints() {
        // Test URI parameters
        server.get("/users/:user_id", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["user_id"] = req["user_id"];
            response["has_user_id"] = req.has("user_id");
            res.json(response);
        });

        // Test multiple URI parameters
        server.get("/users/:user_id/posts/:post_id", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["user_id"] = req["user_id"];
            response["post_id"] = req["post_id"];
            response["has_user_id"] = req.has("user_id");
            response["has_post_id"] = req.has("post_id");
            response["has_missing"] = req.has("missing_param");
            res.json(response);
        });

        // Test query parameters
        server.get("/query", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["name"] = req.query("name");
            response["age"] = req.query("age");
            response["default_value"] = req.query("missing", "default_value");
            res.json(response);
        });

        // Test request body and JSON parsing
        server.post("/json-body", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["raw_body"] = req.body();
            try {
                response["parsed_json"] = req.json();
                response["parse_success"] = true;
            } catch (...) {
                response["parse_success"] = false;
            }
            res.json(response);
        });

        // Test request headers
        server.get("/request-headers", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["user_agent"] = req.header("User-Agent");
            response["custom_header"] = req.header("X-Custom-Header");
            response["missing_header"] = req.header("X-Missing-Header");
            res.json(response);
        });

        // Test keep-alive
        server.get("/keep-alive", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["keep_alive"] = req.keep_alive();
            res.json(response);
        });

        // Test auth user and groups (simulated)
        server.get("/auth-info", [](http::request& req, http::response& res) {
            // Simulate setting auth info
            auto& mutable_req = const_cast<http::request&>(req);
            mutable_req.set_auth_user("test_user");
            mutable_req.set_auth_groups({"admin", "users"});

            nlohmann::json response;
            response["auth_user"] = req.get_auth_user();
            auto groups = req.get_auth_groups();
            response["auth_groups"] = nlohmann::json::array();
            for (const auto& g : groups) {
                response["auth_groups"].push_back(g);
            }
            res.json(response);
        });

        // Test URI parameter manipulation
        server.get("/param-manipulation/:id", [](http::request& req, http::response& res) {
            auto& mutable_req = const_cast<http::request&>(req);

            nlohmann::json response;
            response["original_id"] = req["id"];

            // Add a parameter
            mutable_req.add_uri_parameter("extra", "value1");
            mutable_req.add_uri_parameter("extra", "value2"); // multimap allows duplicates
            response["has_extra"] = req.has("extra");

            // Set a parameter (replaces)
            mutable_req.set_uri_parameter("new_param", "new_value");
            response["new_param"] = req["new_param"];

            // Erase a parameter
            bool erased = mutable_req.erase("extra");
            response["erased_extra"] = erased;
            response["has_extra_after_erase"] = req.has("extra");

            // Debug parameters
            response["debug"] = req.debug_parameters();

            res.json(response);
        });

        // === Response Tests ===

        // Test JSON response
        server.get("/response/json", [](http::request& req, http::response& res) {
            nlohmann::json data;
            data["message"] = "Hello JSON";
            data["number"] = 42;
            data["array"] = {1, 2, 3};
            res.json(data);
        });

        // Test JSON with custom status
        server.get("/response/json-status/:code", [](http::request& req, http::response& res) {
            int code = std::stoi(req["code"]);
            nlohmann::json data;
            data["status_code"] = code;
            res.json(data, static_cast<http::http_response::status>(code));
        });

        // Test text response
        server.get("/response/text", [](http::request& req, http::response& res) {
            res.send("Plain text response");
        });

        // Test text with custom content-type
        server.get("/response/text-custom", [](http::request& req, http::response& res) {
            res.send("<xml>data</xml>", "application/xml");
        });

        // Test HTML response
        server.get("/response/html", [](http::request& req, http::response& res) {
            res.html("<html><body><h1>Hello HTML</h1></body></html>");
        });

        // Test error response
        server.get("/response/error/:code", [](http::request& req, http::response& res) {
            int code = std::stoi(req["code"]);
            res.error(static_cast<http::http_response::status>(code), "Error message for code " + req["code"]);
        });

        // Test error response without message
        server.get("/response/error-no-msg/:code", [](http::request& req, http::response& res) {
            int code = std::stoi(req["code"]);
            res.error(static_cast<http::http_response::status>(code));
        });

        // Test redirect
        server.get("/response/redirect", [this](http::request& req, http::response& res) {
            res.redirect(base_url + "/response/json");
        });

        // Test redirect with status
        server.get("/response/redirect-301", [this](http::request& req, http::response& res) {
            res.redirect(base_url + "/response/json", http::http_response::status::moved_permanently);
        });

        // Test custom headers
        server.get("/response/headers", [](http::request& req, http::response& res) {
            res.status(http::http_response::status::ok);
            res.header("X-Custom-Response", "custom-value");
            res.header("X-Another-Header", "another-value");
            res.send("Response with custom headers");
        });

        // Test has_responded check
        server.get("/response/check-responded", [](http::request& req, http::response& res) {
            bool before = res.has_responded();
            res.send("test");
            // Note: after send, has_responded should be true, but we can't check it in this context
            // The test is mainly to ensure has_responded() works without throwing
            nlohmann::json data;
            data["before_send"] = before;
        });

        // Test get_connection
        server.get("/response/connection", [](http::request& req, http::response& res) {
            auto conn = res.get_connection();
            nlohmann::json response;
            response["has_connection"] = (conn != nullptr);
            res.json(response);
        });

        // Test file sending - create a temp file for this test
        server.get("/response/file", [](http::request& req, http::response& res) {
            // Create a temporary test file
            std::filesystem::path temp_path = std::filesystem::temp_directory_path() / "test_file.txt";
            {
                std::ofstream file(temp_path);
                file << "Test file content";
            }
            res.send_file(temp_path);
            // Clean up
            std::filesystem::remove(temp_path);
        });

        // Test file download (force download)
        server.get("/response/file-download", [](http::request& req, http::response& res) {
            std::filesystem::path temp_path = std::filesystem::temp_directory_path() / "download_test.txt";
            {
                std::ofstream file(temp_path);
                file << "Download test content";
            }
            res.send_file(temp_path, true);  // force_download = true
            std::filesystem::remove(temp_path);
        });

        // Test non-existent file
        server.get("/response/file-not-found", [](http::request& req, http::response& res) {
            res.send_file("/non/existent/path/file.txt");
        });

        // Test directory (not a file)
        server.get("/response/file-directory", [](http::request& req, http::response& res) {
            res.send_file(std::filesystem::temp_directory_path());
        });

        // Test send_response with custom response object
        server.get("/response/custom", [](http::request& req, http::response& res) {
            auto custom_response = std::make_shared<http::http_response>();
            custom_response->set_status(http::http_response::status::ok);
            custom_response->set_content("{\"custom\":true}", "application/json");
            custom_response->add_header("X-Custom", "from-custom-response");
            res.send_response(custom_response);
        });

        // Test various HTTP methods
        server.put("/response/put", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["method"] = "PUT";
            response["body"] = req.body();
            res.json(response);
        });

        server.patch("/response/patch", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["method"] = "PATCH";
            response["body"] = req.body();
            res.json(response);
        });

        server.del("/response/delete", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["method"] = "DELETE";
            res.json(response);
        });

    }

    void start_server() {
        bool started = false;
        int attempts = 0;
        const int max_attempts = 10;

        while (!started && attempts < max_attempts) {
            if (server.listen("0.0.0.0", port)) {
                started = true;
                base_url = "http://localhost:" + std::to_string(port);
            } else {
                port++;
                attempts++;
            }
        }

        if (!started) {
            FAIL("Could not start test server");
        }

        server_thread = std::thread([this]() {
            server.wait();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
};

// === Request Tests ===

TEST_CASE("Server Request URI Parameters", "[server][request][integration]") {
    RequestResponseTestFixture fixture;
    http::client client;
    client.timeout(10s);

    SECTION("Single URI parameter") {
        auto response = client.get(fixture.base_url + "/users/123");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["user_id"] == "123");
        REQUIRE(json["has_user_id"] == true);
    }

    SECTION("Multiple URI parameters") {
        auto response = client.get(fixture.base_url + "/users/456/posts/789");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["user_id"] == "456");
        REQUIRE(json["post_id"] == "789");
        REQUIRE(json["has_user_id"] == true);
        REQUIRE(json["has_post_id"] == true);
        REQUIRE(json["has_missing"] == false);
    }

    SECTION("URI parameter manipulation") {
        auto response = client.get(fixture.base_url + "/param-manipulation/test_id");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["original_id"] == "test_id");
        REQUIRE(json["new_param"] == "new_value");
        REQUIRE(json["erased_extra"] == true);
        REQUIRE(json["has_extra_after_erase"] == false);
    }
}

TEST_CASE("Server Request Query Parameters", "[server][request][integration]") {
    RequestResponseTestFixture fixture;
    http::client client;
    client.timeout(10s);

    SECTION("Query parameters") {
        auto response = client.get(fixture.base_url + "/query?name=John&age=30");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["name"] == "John");
        REQUIRE(json["age"] == "30");
        REQUIRE(json["default_value"] == "default_value");
    }

    SECTION("Missing query parameters use default") {
        auto response = client.get(fixture.base_url + "/query");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["name"] == "");
        REQUIRE(json["default_value"] == "default_value");
    }
}

TEST_CASE("Server Request Body and JSON", "[server][request][integration]") {
    RequestResponseTestFixture fixture;
    http::client client;
    client.timeout(10s);

    SECTION("JSON body parsing") {
        std::string json_body = R"({"key": "value", "number": 42})";
        http::headers_map headers;
        auto response = client.post(fixture.base_url + "/json-body", json_body, "application/json", headers);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["parse_success"] == true);
        REQUIRE(json["parsed_json"]["key"] == "value");
        REQUIRE(json["parsed_json"]["number"] == 42);
    }

    SECTION("Invalid JSON body") {
        std::string invalid_body = "not valid json {{{";
        http::headers_map headers;
        auto response = client.post(fixture.base_url + "/json-body", invalid_body, "text/plain", headers);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["parse_success"] == false);
    }

    SECTION("Empty body") {
        http::headers_map headers;
        auto response = client.post(fixture.base_url + "/json-body", "", "application/json", headers);
        REQUIRE(response.ok());
        // Empty body should result in empty JSON
    }
}

TEST_CASE("Server Request Headers", "[server][request][integration]") {
    RequestResponseTestFixture fixture;
    http::client client;
    client.timeout(10s);

    SECTION("Custom headers are accessible") {
        std::map<std::string, std::string> headers = {
            {"X-Custom-Header", "custom-value"}
        };
        auto response = client.get(fixture.base_url + "/request-headers", headers);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["custom_header"] == "custom-value");
        REQUIRE(json["missing_header"] == "");
    }

    SECTION("User-Agent header") {
        client.user_agent("TestAgent/1.0");
        auto response = client.get(fixture.base_url + "/request-headers");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["user_agent"] == "TestAgent/1.0");
    }
}

TEST_CASE("Server Request Auth Info", "[server][request][integration]") {
    RequestResponseTestFixture fixture;
    http::client client;
    client.timeout(10s);

    SECTION("Auth user and groups") {
        auto response = client.get(fixture.base_url + "/auth-info");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["auth_user"] == "test_user");
        REQUIRE(json["auth_groups"].size() == 2);
    }
}

TEST_CASE("Server Request Keep-Alive", "[server][request][integration]") {
    RequestResponseTestFixture fixture;
    http::client client;
    client.timeout(10s);

    SECTION("Keep-alive is detected") {
        auto response = client.get(fixture.base_url + "/keep-alive");
        REQUIRE(response.ok());
        auto json = response.json();
        // HTTP/1.1 defaults to keep-alive
        REQUIRE(json["keep_alive"] == true);
    }
}

// === Response Tests ===

TEST_CASE("Server Response JSON", "[server][response][integration]") {
    RequestResponseTestFixture fixture;
    http::client client;
    client.timeout(10s);

    SECTION("JSON response with default status") {
        auto response = client.get(fixture.base_url + "/response/json");
        REQUIRE(response.ok());
        REQUIRE(response.status() == 200);
        auto json = response.json();
        REQUIRE(json["message"] == "Hello JSON");
        REQUIRE(json["number"] == 42);
        REQUIRE(json["array"].size() == 3);
    }

    SECTION("JSON response with custom status 201") {
        auto response = client.get(fixture.base_url + "/response/json-status/201");
        REQUIRE(response.status() == 201);
        auto json = response.json();
        REQUIRE(json["status_code"] == 201);
    }
}

TEST_CASE("Server Response Text and HTML", "[server][response][integration]") {
    RequestResponseTestFixture fixture;
    http::client client;
    client.timeout(10s);

    SECTION("Plain text response") {
        auto response = client.get(fixture.base_url + "/response/text");
        REQUIRE(response.ok());
        REQUIRE(response.body() == "Plain text response");
    }

    SECTION("Custom content-type response") {
        auto response = client.get(fixture.base_url + "/response/text-custom");
        REQUIRE(response.ok());
        REQUIRE(response.body() == "<xml>data</xml>");
        std::string content_type = response.header("Content-Type");
        REQUIRE(content_type.find("application/xml") != std::string::npos);
    }

    SECTION("HTML response") {
        auto response = client.get(fixture.base_url + "/response/html");
        REQUIRE(response.ok());
        REQUIRE(response.body().find("<h1>Hello HTML</h1>") != std::string::npos);
        std::string content_type = response.header("Content-Type");
        REQUIRE(content_type.find("text/html") != std::string::npos);
    }
}

TEST_CASE("Server Response Errors", "[server][response][integration]") {
    RequestResponseTestFixture fixture;
    http::client client;
    client.timeout(10s);

    SECTION("Error 400 with message") {
        auto response = client.get(fixture.base_url + "/response/error/400");
        REQUIRE(response.status() == 400);
        REQUIRE(response.is_client_error());
        REQUIRE(response.body().find("Error message") != std::string::npos);
    }

    SECTION("Error 500 with message") {
        auto response = client.get(fixture.base_url + "/response/error/500");
        REQUIRE(response.status() == 500);
        REQUIRE(response.is_server_error());
    }

    SECTION("Error 404 without message") {
        auto response = client.get(fixture.base_url + "/response/error-no-msg/404");
        REQUIRE(response.status() == 404);
    }
}

TEST_CASE("Server Response Redirects", "[server][response][integration]") {
    RequestResponseTestFixture fixture;
    http::client client;
    client.timeout(10s);

    SECTION("302 redirect (default)") {
        client.follow_redirects(false);
        auto response = client.get(fixture.base_url + "/response/redirect");
        REQUIRE(response.status() == 302);
        std::string location = response.header("Location");
        REQUIRE(location.find("/response/json") != std::string::npos);
    }

    SECTION("301 redirect") {
        client.follow_redirects(false);
        auto response = client.get(fixture.base_url + "/response/redirect-301");
        REQUIRE(response.status() == 301);
    }

    SECTION("Follow redirect") {
        client.follow_redirects(true);
        auto response = client.get(fixture.base_url + "/response/redirect");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["message"] == "Hello JSON");
    }
}

TEST_CASE("Server Response Custom Headers", "[server][response][integration]") {
    RequestResponseTestFixture fixture;
    http::client client;
    client.timeout(10s);

    SECTION("Custom headers are set") {
        auto response = client.get(fixture.base_url + "/response/headers");
        REQUIRE(response.ok());
        REQUIRE(response.header("X-Custom-Response") == "custom-value");
        REQUIRE(response.header("X-Another-Header") == "another-value");
    }
}

TEST_CASE("Server Response Files", "[server][response][integration]") {
    RequestResponseTestFixture fixture;
    http::client client;
    client.timeout(10s);

    SECTION("Send file") {
        auto response = client.get(fixture.base_url + "/response/file");
        REQUIRE(response.ok());
        REQUIRE(response.body() == "Test file content");
    }

    SECTION("Send file with force download") {
        auto response = client.get(fixture.base_url + "/response/file-download");
        REQUIRE(response.ok());
        std::string disposition = response.header("Content-Disposition");
        REQUIRE(disposition.find("attachment") != std::string::npos);
    }

    SECTION("File not found") {
        auto response = client.get(fixture.base_url + "/response/file-not-found");
        REQUIRE(response.status() == 404);
    }

    SECTION("Directory instead of file") {
        auto response = client.get(fixture.base_url + "/response/file-directory");
        REQUIRE(response.status() == 403);
    }
}

TEST_CASE("Server Response Custom Response Object", "[server][response][integration]") {
    RequestResponseTestFixture fixture;
    http::client client;
    client.timeout(10s);

    SECTION("Custom response object") {
        auto response = client.get(fixture.base_url + "/response/custom");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["custom"] == true);
        REQUIRE(response.header("X-Custom") == "from-custom-response");
    }
}

TEST_CASE("Server Response HTTP Methods", "[server][response][integration]") {
    RequestResponseTestFixture fixture;
    http::client client;
    client.timeout(10s);

    SECTION("PUT request") {
        http::headers_map headers;
        auto response = client.put(fixture.base_url + "/response/put", "put body", "text/plain", headers);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["method"] == "PUT");
        REQUIRE(json["body"] == "put body");
    }

    SECTION("PATCH request") {
        http::headers_map headers;
        auto response = client.patch(fixture.base_url + "/response/patch", "patch body", "text/plain", headers);
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["method"] == "PATCH");
        REQUIRE(json["body"] == "patch body");
    }

    SECTION("DELETE request") {
        auto response = client.del(fixture.base_url + "/response/delete");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["method"] == "DELETE");
    }
}

TEST_CASE("Server Response Connection Info", "[server][response][integration]") {
    RequestResponseTestFixture fixture;
    http::client client;
    client.timeout(10s);

    SECTION("Response has connection") {
        auto response = client.get(fixture.base_url + "/response/connection");
        REQUIRE(response.ok());
        auto json = response.json();
        REQUIRE(json["has_connection"] == true);
    }
}


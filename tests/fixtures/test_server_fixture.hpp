#ifndef THINGER_HTTP_TEST_SERVER_FIXTURE_HPP
#define THINGER_HTTP_TEST_SERVER_FIXTURE_HPP

#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <nlohmann/json.hpp>
#include <catch2/catch_test_macros.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <chrono>
#include <thread>

namespace thinger::http::test {

// Test server fixture that starts a local HTTP server for testing
struct TestServerFixture {
    http::server server;  // Single-threaded standalone server
    uint16_t port = 9090;
    std::string base_url;
    std::thread server_thread;  // Thread to run the server
    
    TestServerFixture() : TestServerFixture(9090) {}
    
    explicit TestServerFixture(uint16_t custom_port) : port(custom_port) {
        setup_default_endpoints();
        start_server();
    }
    
    virtual ~TestServerFixture() {
        server.stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }
    
protected:
    // Setup default test endpoints - can be overridden in derived classes
    virtual void setup_default_endpoints() {
        // /delay/:seconds endpoint
        server.get("/delay/:seconds", [](http::request& req, http::response& res) {
            auto seconds_str = req["seconds"];
            int seconds = seconds_str.empty() ? 1 : std::stoi(seconds_str);
            
            // Get io_context from the connection's socket
            auto connection = req.get_http_connection();
            if (!connection) {
                res.status(http::http_response::status::internal_server_error);
                res.send("No connection available");
                return;
            }
            
            auto socket = connection->get_socket();
            if (!socket) {
                res.status(http::http_response::status::internal_server_error);
                res.send("No socket available");
                return;
            }
            
            auto& io_context = socket->get_io_context();
            
            // Create an async timer
            auto timer = std::make_shared<boost::asio::deadline_timer>(io_context);
            timer->expires_from_now(boost::posix_time::seconds(seconds));
            
            // Capture response by move to ensure it stays alive
            timer->async_wait([res = std::move(res), seconds, timer](const boost::system::error_code& ec) mutable {
                if (!ec) {
                    // Timer expired normally
                    nlohmann::json response;
                    response["delay"] = seconds;
                    response["status"] = "ok";
                    res.json(response);
                } else {
                    // Timer was cancelled or error occurred
                    res.status(http::http_response::status::internal_server_error);
                    res.send("Timer error");
                }
            });
        });
        
        // /get endpoint
        server.get("/get", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["method"] = "GET";
            response["status"] = "ok";
            res.json(response);
        });
        
        // /post endpoint - echoes back body and headers
        server.post("/post", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["method"] = "POST";
            response["status"] = "ok";

            auto http_req = req.get_http_request();
            if (http_req) {
                response["body"] = http_req->get_body();
                response["headers"] = nlohmann::json::object();

                const auto& headers = http_req->get_headers();
                for (const auto& header : headers) {
                    response["headers"][header.first] = header.second;
                }
            }

            res.json(response);
        });

        // /large endpoint - returns large response
        server.get("/large", [](http::request& req, http::response& res) {
            // Generate ~20KB of content
            std::string large_content;
            large_content.reserve(20000);
            for (int i = 0; i < 1000; ++i) {
                large_content += "Lorem ipsum dolor sit amet. ";
            }

            nlohmann::json response;
            response["content"] = large_content;
            res.json(response);
        });
        
        // /status/:code endpoint
        server.get("/status/:code", [](http::request& req, http::response& res) {
            auto code_str = req["code"];
            int code = code_str.empty() ? 200 : std::stoi(code_str);
            
            nlohmann::json response;
            response["status_code"] = code;
            res.json(response, static_cast<http::http_response::status>(code));
        });
        
        // /redirect/:n endpoint
        server.get("/redirect/:n", [this](http::request& req, http::response& res) {
            auto n_str = req["n"];
            int n = n_str.empty() ? 1 : std::stoi(n_str);
            
            if (n > 1) {
                res.redirect(base_url + "/redirect/" + std::to_string(n - 1));
            } else {
                res.redirect(base_url + "/get");
            }
        });
        
        // /headers endpoint - returns request headers
        server.get("/headers", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["headers"] = nlohmann::json::object();
            
            // Get all headers from request
            auto http_req = req.get_http_request();
            if (http_req) {
                const auto& headers = http_req->get_headers();
                for (const auto& header : headers) {
                    response["headers"][header.first] = header.second;
                }
            }
            
            res.json(response);
        });
        
        // /redirect-to-headers endpoint - redirects to /headers
        server.get("/redirect-to-headers", [this](http::request& req, http::response& res) {
            res.redirect(base_url + "/headers");
        });
        
        // /redirect-307-to-post endpoint - 307 redirect to /post (preserves method)
        server.post("/redirect-307-to-post", [this](http::request& req, http::response& res) {
            res.status(http::http_response::status::temporary_redirect);
            res.header("Location", base_url + "/post");
            res.send("");
        });
        
        // Enhanced /post endpoint that echoes back the request body
        server.post("/echo", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["method"] = "POST";
            auto http_req = req.get_http_request();
            if (http_req) {
                response["body"] = http_req->get_body();
                response["headers"] = nlohmann::json::object();

                const auto& headers = http_req->get_headers();
                for (const auto& header : headers) {
                    response["headers"][header.first] = header.second;
                }
            }

            res.json(response);
        });

        // /put endpoint
        server.put("/put", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["method"] = "PUT";
            response["status"] = "ok";

            auto http_req = req.get_http_request();
            if (http_req) {
                response["body"] = http_req->get_body();
            }

            res.json(response);
        });

        // /patch endpoint
        server.patch("/patch", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["method"] = "PATCH";
            response["status"] = "ok";

            auto http_req = req.get_http_request();
            if (http_req) {
                response["body"] = http_req->get_body();
            }

            res.json(response);
        });

        // /delete endpoint
        server.del("/delete", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["method"] = "DELETE";
            response["status"] = "ok";
            res.json(response);
        });

        // HEAD for /get is automatically handled, but let's also support /headers
        // Note: HEAD requests return same headers as GET but no body
    }
    
    // Add custom endpoint helper
    template<typename Handler>
    void add_get_endpoint(const std::string& path, Handler handler) {
        server.get(path, handler);
    }
    
    template<typename Handler>
    void add_post_endpoint(const std::string& path, Handler handler) {
        server.post(path, handler);
    }
    
private:
    void start_server() {
        // Try to start server, with fallback to find available port
        bool started = false;
        int attempts = 0;
        const int max_attempts = 10;
        
        while (!started && attempts < max_attempts) {
            if (server.listen("0.0.0.0", port)) {
                started = true;
                base_url = "http://localhost:" + std::to_string(port);
            } else {
                // Try next port
                port++;
                attempts++;
            }
        }
        
        if (!started) {
            FAIL("Could not start test server after " + std::to_string(max_attempts) + " attempts");
        }
        
        // Start server thread
        server_thread = std::thread([this]() {
            server.wait();  // This runs io_context.run() in the thread
        });
        
        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
};

} // namespace thinger::http::test

#endif // THINGER_HTTP_TEST_SERVER_FIXTURE_HPP
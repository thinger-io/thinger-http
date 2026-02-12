#ifndef THINGER_HTTP_SSL_TEST_SERVER_FIXTURE_HPP
#define THINGER_HTTP_SSL_TEST_SERVER_FIXTURE_HPP

#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/asio/ssl/certificate_manager.hpp>
#include <nlohmann/json.hpp>
#include <catch2/catch_test_macros.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <thread>

namespace thinger::http::test {

// HTTPS test server fixture with SSL support and self-signed certificate
struct SSLTestServerFixture {
    http::server server;
    uint16_t port = 9443;
    std::string base_url;
    std::thread server_thread;

    SSLTestServerFixture() : SSLTestServerFixture(9443) {}

    explicit SSLTestServerFixture(uint16_t custom_port) : port(custom_port) {
        // Enable SSL - certificate_manager will auto-generate self-signed cert
        server.enable_ssl(true);
        setup_default_endpoints();
        start_server();
    }

    virtual ~SSLTestServerFixture() {
        server.stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

protected:
    virtual void setup_default_endpoints() {
        // /get endpoint
        server.get("/get", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["method"] = "GET";
            response["url"] = req.get_http_request()->get_url();
            response["secure"] = req.get_http_request()->is_ssl();
            res.json(response);
        });

        // /post endpoint
        server.post("/post", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["method"] = "POST";
            response["secure"] = req.get_http_request()->is_ssl();

            auto http_req = req.get_http_request();
            if (http_req) {
                // Try to parse body as JSON
                try {
                    response["json"] = nlohmann::json::parse(http_req->get_body());
                } catch (...) {
                    response["data"] = http_req->get_body();
                }

                response["headers"] = nlohmann::json::object();
                const auto& headers = http_req->get_headers();
                for (const auto& header : headers) {
                    response["headers"][header.first] = header.second;
                }
            }

            res.json(response);
        });

        // /headers endpoint
        server.get("/headers", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["headers"] = nlohmann::json::object();

            auto http_req = req.get_http_request();
            if (http_req) {
                const auto& headers = http_req->get_headers();
                for (const auto& header : headers) {
                    response["headers"][header.first] = header.second;
                }
            }

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

        // /delay/:seconds endpoint
        server.get("/delay/:seconds", [](http::request& req, http::response& res) {
            auto seconds_str = req["seconds"];
            int seconds = seconds_str.empty() ? 1 : std::stoi(seconds_str);

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
            auto timer = std::make_shared<boost::asio::steady_timer>(io_context);
            timer->expires_after(std::chrono::seconds(seconds));

            timer->async_wait([res = std::move(res), seconds, timer](const boost::system::error_code& ec) mutable {
                if (!ec) {
                    nlohmann::json response;
                    response["delay"] = seconds;
                    response["status"] = "ok";
                    res.json(response);
                } else {
                    res.status(http::http_response::status::internal_server_error);
                    res.send("Timer error");
                }
            });
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

        // /user-agent endpoint
        server.get("/user-agent", [](http::request& req, http::response& res) {
            nlohmann::json response;
            auto http_req = req.get_http_request();
            if (http_req && http_req->has_header("User-Agent")) {
                response["user-agent"] = http_req->get_header("User-Agent");
            } else {
                response["user-agent"] = "";
            }
            res.json(response);
        });

        // /response-headers endpoint - sets custom response headers
        server.get("/response-headers", [](http::request& req, http::response& res) {
            auto http_req = req.get_http_request();
            if (http_req) {
                // Parse query params and set as response headers
                auto url = http_req->get_url();
                auto query_pos = url.find('?');
                if (query_pos != std::string::npos) {
                    auto query = url.substr(query_pos + 1);
                    // Simple query parsing
                    size_t pos = 0;
                    while (pos < query.size()) {
                        auto eq_pos = query.find('=', pos);
                        auto amp_pos = query.find('&', pos);
                        if (amp_pos == std::string::npos) amp_pos = query.size();

                        if (eq_pos != std::string::npos && eq_pos < amp_pos) {
                            auto key = query.substr(pos, eq_pos - pos);
                            auto value = query.substr(eq_pos + 1, amp_pos - eq_pos - 1);
                            res.header(key, value);
                        }
                        pos = amp_pos + 1;
                    }
                }
            }
            nlohmann::json response;
            response["status"] = "ok";
            res.json(response);
        });

        // /json endpoint
        server.get("/json", [](http::request& req, http::response& res) {
            nlohmann::json response;
            response["message"] = "Hello, JSON!";
            response["success"] = true;
            res.json(response);
        });

        // /image/png endpoint - returns a minimal PNG
        server.get("/image/png", [](http::request& req, http::response& res) {
            // Minimal 1x1 transparent PNG
            static const unsigned char png_data[] = {
                0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,  // PNG signature
                0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,  // IHDR chunk
                0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,  // 1x1
                0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
                0x89, 0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41,  // IDAT chunk
                0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
                0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00,
                0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE,  // IEND chunk
                0x42, 0x60, 0x82
            };
            res.header("Content-Type", "image/png");
            res.send(std::string(reinterpret_cast<const char*>(png_data), sizeof(png_data)));
        });

        // /large endpoint
        server.get("/large", [](http::request& req, http::response& res) {
            std::string large_content;
            large_content.reserve(20000);
            for (int i = 0; i < 1000; ++i) {
                large_content += "Lorem ipsum dolor sit amet. ";
            }
            nlohmann::json response;
            response["content"] = large_content;
            res.json(response);
        });
    }

private:
    void start_server() {
        bool started = false;
        int attempts = 0;
        const int max_attempts = 10;

        while (!started && attempts < max_attempts) {
            if (server.listen("0.0.0.0", port)) {
                started = true;
                base_url = "https://localhost:" + std::to_string(port);
            } else {
                port++;
                attempts++;
            }
        }

        if (!started) {
            FAIL("Could not start SSL test server after " + std::to_string(max_attempts) + " attempts");
        }

        server_thread = std::thread([this]() {
            server.wait();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
};

} // namespace thinger::http::test

#endif // THINGER_HTTP_SSL_TEST_SERVER_FIXTURE_HPP

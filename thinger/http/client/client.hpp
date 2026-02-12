#ifndef THINGER_HTTP_CLIENT_STANDALONE_HPP
#define THINGER_HTTP_CLIENT_STANDALONE_HPP

#include "http_client_base.hpp"
#include "request_builder.hpp"
#include <boost/asio/io_context.hpp>

namespace thinger::http {

/**
 * Standalone HTTP client with simple synchronous API.
 * Perfect for scripts, CLI tools, and simple applications.
 *
 * Usage:
 *   http::client client;
 *
 *   // Simple GET
 *   auto response = client.get("https://api.example.com/users");
 *   if (response) {
 *       std::cout << response.body() << std::endl;
 *   }
 *
 *   // POST with JSON
 *   auto response = client.post("https://api.example.com/users", R"({"name":"test"})");
 *
 *   // With configuration (fluent API)
 *   client.timeout(std::chrono::seconds(30)).verify_ssl(false);
 *   auto response = client.get("https://internal-api/data");
 *
 * For async/concurrent operations, use http::async_client instead.
 */
class client : public http_client_base {
private:
    boost::asio::io_context io_context_;

    // Internal helper to run an awaitable synchronously
    template<typename T>
    T exec(awaitable<T> coro) {
        T result;
        co_spawn(io_context_, [&result, coro = std::move(coro)]() mutable -> awaitable<void> {
            result = co_await std::move(coro);
        }, detached);
        io_context_.run();
        io_context_.restart();
        return result;
    }

protected:
    boost::asio::io_context& get_io_context() override {
        return io_context_;
    }

public:
    client() = default;
    ~client() override = default;

    // ============================================
    // Synchronous HTTP methods
    // ============================================

    client_response get(const std::string& url, headers_map headers = {}) {
        return exec(http_client_base::get(url, std::move(headers)));
    }

    client_response post(const std::string& url, std::string body = {},
                         std::string content_type = "application/json", headers_map headers = {}) {
        return exec(http_client_base::post(url, std::move(body), std::move(content_type), std::move(headers)));
    }

    client_response post(const std::string& url, const form& form, headers_map headers = {}) {
        return exec(http_client_base::post(url, form, std::move(headers)));
    }

    client_response put(const std::string& url, std::string body = {},
                        std::string content_type = "application/json", headers_map headers = {}) {
        return exec(http_client_base::put(url, std::move(body), std::move(content_type), std::move(headers)));
    }

    client_response patch(const std::string& url, std::string body = {},
                          std::string content_type = "application/json", headers_map headers = {}) {
        return exec(http_client_base::patch(url, std::move(body), std::move(content_type), std::move(headers)));
    }

    client_response del(const std::string& url, headers_map headers = {}) {
        return exec(http_client_base::del(url, std::move(headers)));
    }

    client_response head(const std::string& url, headers_map headers = {}) {
        return exec(http_client_base::head(url, std::move(headers)));
    }

    client_response options(const std::string& url, headers_map headers = {}) {
        return exec(http_client_base::options(url, std::move(headers)));
    }

    // Unix socket variants
    client_response get(const std::string& url, const std::string& unix_socket, headers_map headers = {}) {
        return exec(http_client_base::get(url, unix_socket, std::move(headers)));
    }

    client_response post(const std::string& url, const std::string& unix_socket,
                         std::string body = {}, std::string content_type = "application/json",
                         headers_map headers = {}) {
        return exec(http_client_base::post(url, unix_socket, std::move(body), std::move(content_type), std::move(headers)));
    }

    // Generic send with custom request
    client_response send(std::shared_ptr<http_request> request) {
        return exec(http_client_base::send(std::move(request)));
    }

    // Streaming send
    stream_result send_streaming(std::shared_ptr<http_request> request, stream_callback callback) {
        return exec(http_client_base::send_streaming(std::move(request), std::move(callback)));
    }

    // ============================================
    // Request builder for fluent API
    // ============================================

    /**
     * Create a request builder for fluent API with streaming support.
     *
     * Usage:
     *   auto res = client.request("https://api.com/data")
     *       .header("Authorization", "Bearer xxx")
     *       .get();
     */
    request_builder<client> request(const std::string& url) {
        return request_builder<client>(this, url);
    }

    // ============================================
    // WebSocket
    // ============================================

    /**
     * Connect to a WebSocket server (simple API).
     *
     * Usage:
     *   if (auto ws = client.websocket("ws://server.com/path")) {
     *       ws->send_text("Hello!");
     *       auto [msg, binary] = ws->receive();
     *       ws->close();
     *   }
     */
    std::optional<websocket_client> websocket(const std::string& url,
                                               const std::string& subprotocol = "") {
        return exec(http_client_base::upgrade_websocket(url, subprotocol));
    }

    /**
     * Connect to a WebSocket server with custom request/headers (used by request_builder).
     */
    std::optional<websocket_client> websocket(std::shared_ptr<http_request> request,
                                               const std::string& subprotocol = "") {
        return exec(http_client_base::upgrade_websocket(std::move(request), subprotocol));
    }
};

} // namespace thinger::http

#endif

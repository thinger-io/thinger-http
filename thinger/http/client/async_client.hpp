#ifndef THINGER_HTTP_CLIENT_ASYNC_CLIENT_HPP
#define THINGER_HTTP_CLIENT_ASYNC_CLIENT_HPP

#include "http_client_base.hpp"
#include "stream_types.hpp"
#include "request_builder.hpp"
#include "../../asio/worker_client.hpp"
#include "../../asio/workers.hpp"
#include <condition_variable>
#include <mutex>
#include <atomic>
#include <fstream>
#include <filesystem>

namespace thinger::http {

/**
 * Asynchronous HTTP client with callback-based API.
 * Uses worker thread pool for optimal performance.
 * Perfect for concurrent requests and non-blocking I/O.
 *
 * Usage with callbacks:
 *   http::async_client client;
 *   client.get("https://api.example.com/data", [](auto& response) {
 *       std::cout << response.body() << std::endl;
 *   });
 *   client.wait();  // Wait for all requests to complete
 *
 * Usage with coroutines:
 *   http::async_client client;
 *   client.run([&]() -> awaitable<void> {
 *       auto response = co_await client.get("https://api.example.com/data");
 *   });
 *   client.wait();
 */
class async_client : public http_client_base, public asio::worker_client {
private:
    std::condition_variable requests_cv_;
    std::mutex requests_mutex_;
    std::atomic<size_t> active_requests_{0};

protected:
    boost::asio::io_context& get_io_context() override {
        return asio::get_workers().get_thread_io_context();
    }

public:
    async_client();
    ~async_client() override;

    // Track active requests for synchronization
    void track_request_start();
    void track_request_end();

    // worker_client interface
    bool stop() override;
    void wait() override;

    // Wait with timeout
    bool wait_for(std::chrono::milliseconds timeout);

    // Query
    size_t pending_requests() const { return active_requests_.load(); }
    bool has_pending_requests() const { return active_requests_.load() > 0; }

    // Run an awaitable on the worker pool
    template<typename T>
    void run(awaitable<T> coro) {
        track_request_start();
        co_spawn(get_io_context(),
            [this, coro = std::move(coro)]() mutable -> awaitable<void> {
                try {
                    co_await std::move(coro);
                } catch (...) {
                    // Log or handle error
                }
                track_request_end();
            },
            detached);
    }

    // Run a coroutine factory on the worker pool
    template<typename F>
    void run(F&& coro_factory) {
        track_request_start();
        co_spawn(get_io_context(),
            [this, factory = std::forward<F>(coro_factory)]() mutable -> awaitable<void> {
                try {
                    co_await factory();
                } catch (...) {
                    // Log or handle error
                }
                track_request_end();
            },
            detached);
    }

    // ============================================
    // Callback-based async methods
    // ============================================
    // These provide a simpler interface when you're not already in a coroutine.
    // The callback is invoked with the response when the request completes.
    //
    // Usage:
    //   client.get("https://api.example.com/users", [](client_response& response) {
    //       std::cout << response.body() << std::endl;
    //   });
    //   client.wait();  // Wait for all requests to complete

    // Bring base class awaitable methods into scope for co_await usage
    using http_client_base::get;
    using http_client_base::post;
    using http_client_base::put;
    using http_client_base::patch;
    using http_client_base::del;
    using http_client_base::head;
    using http_client_base::options;
    using http_client_base::send;
    using http_client_base::send_streaming;

    template<typename Callback>
    void get(const std::string& url, Callback&& callback, headers_map headers = {}) {
        run([this, url, cb = std::forward<Callback>(callback), h = std::move(headers)]() mutable -> awaitable<void> {
            auto response = co_await http_client_base::get(url, std::move(h));
            cb(response);
        });
    }

    template<typename Callback>
    void post(const std::string& url, Callback&& callback, std::string body = {},
              std::string content_type = "application/json", headers_map headers = {}) {
        run([this, url, cb = std::forward<Callback>(callback), b = std::move(body),
             ct = std::move(content_type), h = std::move(headers)]() mutable -> awaitable<void> {
            auto response = co_await http_client_base::post(url, std::move(b), std::move(ct), std::move(h));
            cb(response);
        });
    }

    template<typename Callback>
    void post(const std::string& url, const form& form, Callback&& callback, headers_map headers = {}) {
        run([this, url, cb = std::forward<Callback>(callback),
             f = form, h = std::move(headers)]() mutable -> awaitable<void> {
            auto response = co_await http_client_base::post(url, f, std::move(h));
            cb(response);
        });
    }

    template<typename Callback>
    void put(const std::string& url, Callback&& callback, std::string body = {},
             std::string content_type = "application/json", headers_map headers = {}) {
        run([this, url, cb = std::forward<Callback>(callback), b = std::move(body),
             ct = std::move(content_type), h = std::move(headers)]() mutable -> awaitable<void> {
            auto response = co_await http_client_base::put(url, std::move(b), std::move(ct), std::move(h));
            cb(response);
        });
    }

    template<typename Callback>
    void patch(const std::string& url, Callback&& callback, std::string body = {},
               std::string content_type = "application/json", headers_map headers = {}) {
        run([this, url, cb = std::forward<Callback>(callback), b = std::move(body),
             ct = std::move(content_type), h = std::move(headers)]() mutable -> awaitable<void> {
            auto response = co_await http_client_base::patch(url, std::move(b), std::move(ct), std::move(h));
            cb(response);
        });
    }

    template<typename Callback>
    void del(const std::string& url, Callback&& callback, headers_map headers = {}) {
        run([this, url, cb = std::forward<Callback>(callback), h = std::move(headers)]() mutable -> awaitable<void> {
            auto response = co_await http_client_base::del(url, std::move(h));
            cb(response);
        });
    }

    template<typename Callback>
    void head(const std::string& url, Callback&& callback, headers_map headers = {}) {
        run([this, url, cb = std::forward<Callback>(callback), h = std::move(headers)]() mutable -> awaitable<void> {
            auto response = co_await http_client_base::head(url, std::move(h));
            cb(response);
        });
    }

    template<typename Callback>
    void options(const std::string& url, Callback&& callback, headers_map headers = {}) {
        run([this, url, cb = std::forward<Callback>(callback), h = std::move(headers)]() mutable -> awaitable<void> {
            auto response = co_await http_client_base::options(url, std::move(h));
            cb(response);
        });
    }

    // ============================================
    // Streaming methods
    // ============================================

    /**
     * Send a streaming request with callback.
     *
     * Usage:
     *   client.send_streaming(request, stream_callback, [](stream_result& result) {
     *       std::cout << "Downloaded: " << result.bytes_transferred << " bytes" << std::endl;
     *   });
     */
    template<typename Callback>
    void send_streaming(std::shared_ptr<http_request> request, stream_callback stream_cb, Callback&& result_cb) {
        run([this, req = std::move(request), scb = std::move(stream_cb),
             rcb = std::forward<Callback>(result_cb)]() mutable -> awaitable<void> {
            auto result = co_await http_client_base::send_streaming(std::move(req), std::move(scb));
            rcb(result);
        });
    }

    /**
     * Streaming GET with callback.
     */
    template<typename Callback>
    void get_streaming(const std::string& url, stream_callback stream_cb, Callback&& result_cb, headers_map headers = {}) {
        auto request = std::make_shared<http_request>();
        request->set_url(url);
        request->set_method(method::GET);
        for (const auto& [key, value] : headers) {
            request->add_header(key, value);
        }
        send_streaming(std::move(request), std::move(stream_cb), std::forward<Callback>(result_cb));
    }

    /**
     * Download file with progress callback.
     */
    template<typename Callback>
    void download(const std::string& url, const std::filesystem::path& path,
                  Callback&& result_cb, progress_callback progress = {}) {
        run([this, url, path, rcb = std::forward<Callback>(result_cb),
             prog = std::move(progress)]() mutable -> awaitable<void> {
            // Open file
            std::ofstream file(path, std::ios::binary);
            if (!file) {
                stream_result result;
                result.error = "Cannot open file for writing: " + path.string();
                rcb(result);
                co_return;
            }

            // Create request
            auto request = std::make_shared<http_request>();
            request->set_url(url);
            request->set_method(method::GET);

            // Stream to file
            auto result = co_await http_client_base::send_streaming(request,
                [&file, &prog](const stream_info& info) {
                    file.write(info.data.data(), static_cast<std::streamsize>(info.data.size()));
                    if (prog) {
                        prog(info.downloaded, info.total);
                    }
                    return true;
                });

            rcb(result);
        });
    }

    // ============================================
    // Request builder for fluent API
    // ============================================

    /**
     * Create a request builder for fluent API.
     *
     * Usage:
     *   auto res = co_await client.request("https://api.com/data")
     *       .header("Authorization", "Bearer xxx")
     *       .get();
     */
    request_builder<async_client> request(const std::string& url) {
        return request_builder<async_client>(this, url);
    }

    // ============================================
    // WebSocket
    // ============================================

    /**
     * Connect to a WebSocket server (awaitable version).
     *
     * Usage:
     *   auto ws = co_await client.websocket("wss://server.com/ws", "subprotocol");
     *   if (ws) {
     *       co_await ws->send_text_async("Hello!");
     *   }
     */
    awaitable<std::optional<websocket_client>> websocket(
        const std::string& url, const std::string& subprotocol = "") {
        return upgrade_websocket(url, subprotocol);
    }

    /**
     * Connect to a WebSocket server with custom request (for builder pattern).
     */
    awaitable<std::optional<websocket_client>> websocket(
        std::shared_ptr<http_request> request, const std::string& subprotocol = "") {
        return upgrade_websocket(std::move(request), subprotocol);
    }

    /**
     * Connect to a WebSocket server (callback version).
     *
     * Usage:
     *   client.websocket("ws://server.com/path", [](auto ws) {
     *       if (ws) {
     *           ws->send_text("Hello!");
     *           ws->run();  // Start message loop
     *       }
     *   });
     *
     * The callback receives a shared_ptr to maintain ownership.
     */
    template<typename Callback>
    void websocket(const std::string& url, Callback&& callback, const std::string& subprotocol = "") {
        run([this, url, cb = std::forward<Callback>(callback), proto = subprotocol]() mutable -> awaitable<void> {
            auto result = co_await upgrade_websocket(url, proto);
            if (result) {
                auto ws = std::make_shared<websocket_client>(std::move(*result));
                cb(ws);
            } else {
                cb(std::shared_ptr<websocket_client>{});
            }
        });
    }
};

} // namespace thinger::http

#endif

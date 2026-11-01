#ifndef THINGER_HTTP_CLIENT_ASYNC_CLIENT_HPP
#define THINGER_HTTP_CLIENT_ASYNC_CLIENT_HPP

#include "http_client_base.hpp"
#include "stream_types.hpp"
#include "request_builder.hpp"
#include "../../asio/workers.hpp"
#include <concepts>
#include <condition_variable>
#include <mutex>
#include <atomic>
#include <fstream>
#include <filesystem>

namespace thinger::http {

/**
 * Asynchronous HTTP client with callback-based API.
 * Runs its requests and callbacks on a pool of workers, which the application owns, starts,
 * and keeps alive longer than the client (stop the client, or destroy it, before the workers).
 * Perfect for concurrent requests and non-blocking I/O.
 *
 * stop() and the destructor end the requests in flight before returning: they cancel them
 * (their callbacks are not called; the coroutines of run() see them fail with
 * operation_aborted) and, called from a thread that is not one of the workers, wait until none
 * of them runs, nor any callback of the client (see stop()). Once the destructor returned,
 * called from any thread (a callback of the client included), no callback or coroutine of the
 * client runs, so what they captured may be destroyed. The WebSockets handed out outlive the
 * client. The coroutines of run() run on an executor of the client (that of their worker,
 * wrapped), which drops them once stopped: work spawned on it (this_coro::executor) is dropped
 * with them.
 *
 * Usage with callbacks:
 *   asio::workers workers;
 *   workers.start();
 *   http::async_client client(workers);
 *   client.get("https://api.example.com/data", [](auto& response) {
 *       std::cout << response.body() << std::endl;
 *   });
 *   client.wait();  // Wait for all requests to complete
 *
 * Usage with coroutines:
 *   http::async_client client(workers);
 *   client.run([&]() -> awaitable<void> {
 *       auto response = co_await client.get("https://api.example.com/data");
 *   });
 *   client.wait();
 */
namespace detail {
    // What the requests of an async_client share with it, kept alive by them: they may outlive
    // the client (see async_client.cpp)
    class request_scope;
    // A request of an async_client, from run() to its end
    class client_request;
    // The io_context of the executor of a request of an async_client (that of a coroutine of
    // run()): its worker's; null for any other executor
    boost::asio::io_context* request_io_context(const boost::asio::any_io_executor& executor);
}

class async_client : public http_client_base {
private:
    asio::workers& workers_;
    std::shared_ptr<detail::request_scope> scope_;
    std::atomic<bool> running_{true};

    // The end of a request run by run(): the completion of its coroutine, which releases the
    // request when destroyed, whether it was called or not (it is not once the client closed
    // the request: see stop()). It touches only the state the requests share with the client
    class request_end {
    public:
        explicit request_end(std::shared_ptr<detail::client_request> request) noexcept;
        request_end(request_end&&) noexcept = default;
        request_end& operator=(request_end&&) noexcept = default;
        ~request_end();

        void operator()(const std::exception_ptr&, auto&&...) const noexcept {}

    private:
        std::shared_ptr<detail::client_request> request_;
    };

    // A request about to run, registered: null while stop() or the destructor runs, when it
    // is not run
    std::shared_ptr<detail::client_request> start_request();
    // Where it runs (the executor of the caller's worker, or of one of the pool, which drops
    // the request once the client closed it), and what cancels it
    static boost::asio::any_io_executor executor_of(const detail::client_request& request);
    static boost::asio::cancellation_slot slot_of(detail::client_request& request);
    // Once its coroutine is spawned: it may be cancelled from then on
    static void spawned(const std::shared_ptr<detail::client_request>& request);

    // Cancel the requests in flight and close them (see stop()); with `final`, for good
    void close_requests(bool final);

    template<typename F>
    void spawn(F&& coro) {
        auto request = start_request();
        if (!request) return;
        co_spawn(executor_of(*request), std::forward<F>(coro),
                 boost::asio::bind_cancellation_slot(slot_of(*request), request_end(request)));
        spawned(request);
    }

protected:
    // The worker of the calling thread, or else one of the pool
    boost::asio::io_context& get_io_context() override {
        return workers_.get_thread_io_context();
    }

    // The WebSockets handed out run on the worker, not on the executor of the requests: they
    // belong to the application, and outlive the client
    boost::asio::any_io_executor websocket_executor(const boost::asio::any_io_executor& executor) override;

public:
    explicit async_client(asio::workers& workers);
    // Ends the requests in flight as stop() does, for good: none of them (no callback, no
    // coroutine of run()) runs once it returns, and those it could not wait for touch nothing of
    // the client
    ~async_client() override;

    // Count a request the application tracks itself (wait() waits for it), and its end
    void track_request_start();
    void track_request_end();

    // Stop: release wait(), cancel the requests in flight and end them, and close the idle
    // connections. Returns whether it was running. The callbacks of the requests cancelled
    // are not called (one that completed meanwhile may still call its own), and the
    // coroutines of run() see their requests fail with operation_aborted. From a thread that
    // is not one of the workers, it returns once no request of the client runs, nor any of its
    // callbacks; from one of them (e.g. in a callback), it does not wait, but none of those
    // requests resumes anymore. Requests started while it runs are not run; those started
    // afterwards are
    bool stop();
    bool is_running() const { return running_; }

    // Wait until no request is active, or the client stops
    void wait();

    // Wait with timeout
    bool wait_for(std::chrono::milliseconds timeout);

    // Query
    size_t pending_requests() const;
    bool has_pending_requests() const { return pending_requests() > 0; }

    // Run an awaitable on the worker pool (counted until it ends, also by an exception)
    template<typename T>
    void run(awaitable<T> coro) {
        spawn(std::move(coro));
    }

    // Run a coroutine factory on the worker pool (counted until it ends, also by an exception)
    template<typename F>
    void run(F&& coro_factory) {
        spawn(std::forward<F>(coro_factory));
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
            co_await call_and_await(cb, response);
        });
    }

    template<typename Callback>
    void post(const std::string& url, Callback&& callback, std::string body = {},
              std::string content_type = "application/json", headers_map headers = {}) {
        run([this, url, cb = std::forward<Callback>(callback), b = std::move(body),
             ct = std::move(content_type), h = std::move(headers)]() mutable -> awaitable<void> {
            auto response = co_await http_client_base::post(url, std::move(b), std::move(ct), std::move(h));
            co_await call_and_await(cb, response);
        });
    }

    template<typename Callback>
    void post(const std::string& url, const form& form, Callback&& callback, headers_map headers = {}) {
        run([this, url, cb = std::forward<Callback>(callback),
             f = form, h = std::move(headers)]() mutable -> awaitable<void> {
            auto response = co_await http_client_base::post(url, f, std::move(h));
            co_await call_and_await(cb, response);
        });
    }

    template<typename Callback>
    void put(const std::string& url, Callback&& callback, std::string body = {},
             std::string content_type = "application/json", headers_map headers = {}) {
        run([this, url, cb = std::forward<Callback>(callback), b = std::move(body),
             ct = std::move(content_type), h = std::move(headers)]() mutable -> awaitable<void> {
            auto response = co_await http_client_base::put(url, std::move(b), std::move(ct), std::move(h));
            co_await call_and_await(cb, response);
        });
    }

    template<typename Callback>
    void patch(const std::string& url, Callback&& callback, std::string body = {},
               std::string content_type = "application/json", headers_map headers = {}) {
        run([this, url, cb = std::forward<Callback>(callback), b = std::move(body),
             ct = std::move(content_type), h = std::move(headers)]() mutable -> awaitable<void> {
            auto response = co_await http_client_base::patch(url, std::move(b), std::move(ct), std::move(h));
            co_await call_and_await(cb, response);
        });
    }

    template<typename Callback>
    void del(const std::string& url, Callback&& callback, headers_map headers = {}) {
        run([this, url, cb = std::forward<Callback>(callback), h = std::move(headers)]() mutable -> awaitable<void> {
            auto response = co_await http_client_base::del(url, std::move(h));
            co_await call_and_await(cb, response);
        });
    }

    template<typename Callback>
    void head(const std::string& url, Callback&& callback, headers_map headers = {}) {
        run([this, url, cb = std::forward<Callback>(callback), h = std::move(headers)]() mutable -> awaitable<void> {
            auto response = co_await http_client_base::head(url, std::move(h));
            co_await call_and_await(cb, response);
        });
    }

    template<typename Callback>
    void options(const std::string& url, Callback&& callback, headers_map headers = {}) {
        run([this, url, cb = std::forward<Callback>(callback), h = std::move(headers)]() mutable -> awaitable<void> {
            auto response = co_await http_client_base::options(url, std::move(h));
            co_await call_and_await(cb, response);
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
            co_await call_and_await(rcb, result);
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
                co_await call_and_await(rcb, result);
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

            co_await call_and_await(rcb, result);
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
        requires std::invocable<Callback&, std::shared_ptr<websocket_client>>   // not a subprotocol
    void websocket(const std::string& url, Callback&& callback, const std::string& subprotocol = "") {
        run([this, url, cb = std::forward<Callback>(callback), proto = subprotocol]() mutable -> awaitable<void> {
            auto result = co_await upgrade_websocket(url, proto);
            if (result) {
                auto ws = std::make_shared<websocket_client>(std::move(*result));
                co_await call_and_await(cb, ws);
            } else {
                co_await call_and_await(cb, std::shared_ptr<websocket_client>{});
            }
        });
    }
};

} // namespace thinger::http

#endif

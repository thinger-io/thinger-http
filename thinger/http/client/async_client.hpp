#ifndef THINGER_HTTP_CLIENT_ASYNC_CLIENT_HPP
#define THINGER_HTTP_CLIENT_ASYNC_CLIENT_HPP

#include "http_client_base.hpp"
#include "stream_types.hpp"
#include "request_builder.hpp"
#include "../../asio/workers.hpp"
#include <atomic>
#include <concepts>
#include <fstream>
#include <filesystem>
#include <functional>

namespace thinger::http {

namespace detail {
    // What an async_client shares with its requests, which keep it alive: the HTTP client they
    // use, and the requests in flight (see async_client.cpp)
    class client_state;
    // A request of an async_client, from run() to its end
    class client_request;
}

/**
 * Asynchronous HTTP client with callback-based API.
 * Runs its requests and callbacks on a pool of workers, which the application owns, starts,
 * and keeps alive longer than the client (stop the client, or destroy it, before the workers).
 * Perfect for concurrent requests and non-blocking I/O.
 *
 * It is a handle: every request keeps what it uses of the client (its settings, connections and
 * TLS contexts) alive, so a request may outlive the client and touch no freed memory.
 *
 * stop() and the destructor cancel the requests in flight, in any phase, and close their
 * connections. Once they returned, from any thread, the callback of a request cancelled is not
 * called. A coroutine, of run() or a callback that is one, sees its requests fail with
 * operation_aborted, and any further one fail at once, without connecting. Called from a thread
 * that is not one of the workers, they also wait until no callback of the client runs and the
 * requests cancelled ended, so what their callbacks and coroutines captured may be destroyed then;
 * a coroutine ignoring the cancellation is waited for up to 1 s, and left behind. Called from a
 * worker, they do not wait for the requests: a coroutine, as one left behind, may resume after
 * they returned, and see operation_aborted or a response that completed before the cancellation
 * reached it. What it uses, this client too, must stay alive: hold it by shared_ptr (see README),
 * or stop it from a thread that is not a worker. The WebSockets handed out outlive the client,
 * and so does what the application spawns on the plain executor of a coroutine of run().
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
class async_client {
private:
    // Kept by the requests too; client_ is the HTTP client in it
    std::shared_ptr<detail::client_state> state_;
    http_client_base& client_;
    std::atomic<bool> running_{true};

    template<typename> friend class request_builder;

    // The end of a request: the completion of its coroutine, which outlives every frame of the
    // request and releases it when destroyed, whether it was called or not (a frame dropped by
    // workers that stopped). It holds the request, and so what the frames use of the client
    class request_end {
    public:
        explicit request_end(std::shared_ptr<detail::client_request> request) noexcept : request_(std::move(request)) {}
        request_end(request_end&&) noexcept = default;
        ~request_end();

        void operator()(const std::exception_ptr&) const noexcept {}

    private:
        std::shared_ptr<detail::client_request> request_;
    };

    // The synchronous callback of a request about to run: not to be called if the request was
    // cancelled; otherwise, counted as running until the end of this scope
    class callback_scope {
    public:
        explicit callback_scope(detail::client_request& request);
        ~callback_scope();

        explicit operator bool() const { return entered_; }

    private:
        detail::client_request& request_;
        bool entered_;
    };

    // What the operation of a request with a callback runs with
    class request_context {
    public:
        explicit request_context(detail::client_request& request);

        http_client_base& client;

        // The chunks of a streamed body as callbacks of the request: not called once it was
        // cancelled, which stops the stream
        stream_callback chunks(stream_callback callback) const;

    private:
        detail::client_request& request_;
    };

    // A request about to run, registered: null while stop() or the destructor runs, when it
    // is not run
    std::shared_ptr<detail::client_request> start_request();
    // Where it runs (the worker of the caller, or one of the pool), and what cancels it
    static boost::asio::io_context::executor_type executor_of(const detail::client_request& request);
    static boost::asio::cancellation_slot slot_of(detail::client_request& request);
    // Once its coroutine is spawned: it may be cancelled from then on
    static void spawned(const std::shared_ptr<detail::client_request>& request);
    // Whether it was cancelled: it does not start, nor call its callback
    static bool cancelled(detail::client_request& request);

    // Cancel the requests in flight and wait for them as the thread allows (see stop()); with
    // `final`, for good
    void close_requests(bool final);

    // Run the coroutine `start` makes for a request. Its frames hold the request by reference,
    // and whatever else they use, so they all end before the completion of the coroutine
    template<typename Start>
    void spawn(Start start) {
        auto request = start_request();
        if (!request) return;
        co_spawn(executor_of(*request), start(*request),
                 boost::asio::bind_cancellation_slot(slot_of(*request), request_end(request)));
        spawned(request);
    }

    template<typename F>
    static awaitable<void> run_factory(detail::client_request& request, F factory) {
        if (!cancelled(request)) (void) co_await factory();
    }

    // The coroutine of a request with a callback. A synchronous callback is waited for by stop()
    // and the destructor until it returns; a coroutine is part of the request: the cancellation
    // reaches what it waits for, and it is waited for as the rest of the request is
    template<typename Operation, typename Callback>
    static awaitable<void> run_callback(detail::client_request& request, Operation operation, Callback callback) {
        if (cancelled(request)) co_return;
        auto result = co_await operation(request_context(request));
        if constexpr (is_awaitable_v<std::invoke_result_t<Callback&, decltype(result)&>>) {
            if (!cancelled(request)) (void) co_await std::invoke(callback, result);
        } else {
            callback_scope running(request);
            if (running) std::invoke(callback, result);
        }
    }

    // A request with a callback: every one of them goes through here. `operation` takes a
    // request_context and returns the awaitable of the result handed to the callback
    template<typename Operation, typename Callback>
    void run_with_callback(Operation operation, Callback&& callback) {
        spawn([&](detail::client_request& request) {
            return run_callback(request, std::move(operation), std::forward<Callback>(callback));
        });
    }

    // A WebSocket with a callback, by URL or with a request of its own
    template<typename Target, typename Callback>
    void websocket_with_callback(Target target, std::string subprotocol, Callback&& callback) {
        run_with_callback([target = std::move(target), proto = std::move(subprotocol)](auto c) mutable
                              -> awaitable<std::shared_ptr<websocket_client>> {
            auto result = co_await c.client.upgrade_websocket(std::move(target), proto);
            if (!result) co_return nullptr;
            co_return std::make_shared<websocket_client>(std::move(*result));
        }, std::forward<Callback>(callback));
    }

public:
    explicit async_client(asio::workers& workers);
    // Ends the requests in flight as stop() does, for good. Called from a worker, it does not
    // wait for the requests, but it waits for the callbacks running on other workers: no callback
    // of the client runs once it returned, from any thread
    ~async_client();

    async_client(const async_client&) = delete;
    async_client& operator=(const async_client&) = delete;

    // Count a request the application tracks itself (wait() waits for it), and its end
    void track_request_start();
    void track_request_end();

    // Stop: release wait(), cancel the requests in flight, and close their connections and the
    // idle ones. Returns whether it was running. The callbacks of the requests cancelled are not
    // called (one running, of a request that completed before, is not interrupted), and coroutines
    // see their requests fail with operation_aborted. From a thread that is not one of the
    // workers, it returns once none of its callbacks runs and the requests cancelled ended
    // (up to 1 s: one ignoring the cancellation is left behind); from one of them (e.g. in a
    // callback), it waits for nothing, and the coroutines cancelled may resume after it returned.
    // Requests started while it runs are not run; those started afterwards are
    bool stop();
    bool is_running() const { return running_; }

    // Wait until no request is active, or the client stops; with a timeout
    void wait();
    bool wait_for(std::chrono::milliseconds timeout);

    size_t pending_requests() const;
    bool has_pending_requests() const { return pending_requests() > 0; }

    // Run an awaitable on the worker pool (counted until it ends, also by an exception)
    template<typename T>
    void run(awaitable<T> coro) {
        run([coro = std::move(coro)]() mutable { return std::move(coro); });
    }

    // Run a coroutine factory on the worker pool (counted until it ends, also by an exception)
    template<typename F>
    void run(F&& coro_factory) {
        spawn([&](detail::client_request& request) { return run_factory(request, std::forward<F>(coro_factory)); });
    }

    // --- Settings (see http_client_base) ---

    http_client_base& timeout(std::chrono::seconds t) { return client_.timeout(t); }
    http_client_base& max_redirects(unsigned int max) { return client_.max_redirects(max); }
    http_client_base& follow_redirects(bool follow) { return client_.follow_redirects(follow); }
    http_client_base& user_agent(const std::string& agent) { return client_.user_agent(agent); }
    http_client_base& auto_decompress(bool decompress) { return client_.auto_decompress(decompress); }
    http_client_base& verify_ssl(bool verify) { return client_.verify_ssl(verify); }
    http_client_base& log_bodies(bool enabled = true, size_t max_length = body_logging::default_max_length) {
        return client_.log_bodies(enabled, max_length);
    }
    http_client_base& redact_parameters(util::url::redacted_parameters names) { return client_.redact_parameters(std::move(names)); }
    http_client_base& unix_socket(const std::string& path) { return client_.unix_socket(path); }
    http_client_base& max_content_size(size_t size) { return client_.max_content_size(size); }
    http_client_base& proxy(const std::string& url, const std::string& no_proxy = "") { return client_.proxy(url, no_proxy); }
    http_client_base& proxy(const proxy_config& config) { return client_.proxy(config); }
    http_client_base& proxy_from_environment(bool enabled) { return client_.proxy_from_environment(enabled); }
    http_client_base& add_ca_file(const std::string& path) { return client_.add_ca_file(path); }
    http_client_base& add_ca_pem(const std::string& pem) { return client_.add_ca_pem(pem); }
    http_client_base& use_system_ca(bool enabled) { return client_.use_system_ca(enabled); }
    boost::system::error_code client_certificate(const std::string& certificate, const std::string& private_key,
                                                 const std::string& chain = "") {
        return client_.client_certificate(certificate, private_key, chain);
    }
    boost::system::error_code client_certificate_file(const std::string& certificate_path, const std::string& private_key_path,
                                                      const std::string& chain_path = "") {
        return client_.client_certificate_file(certificate_path, private_key_path, chain_path);
    }
    void clear_client_certificate() { client_.clear_client_certificate(); }
    bool has_client_certificate() const { return client_.has_client_certificate(); }
    http_client_base& max_idle_connections_per_host(size_t max) { return client_.max_idle_connections_per_host(max); }
    http_client_base& max_idle_connections(size_t max) { return client_.max_idle_connections(max); }
    http_client_base& idle_timeout(std::chrono::milliseconds timeout) { return client_.idle_timeout(timeout); }

    std::chrono::seconds get_timeout() const { return client_.get_timeout(); }
    unsigned int get_max_redirects() const { return client_.get_max_redirects(); }
    bool get_follow_redirects() const { return client_.get_follow_redirects(); }
    const std::string& get_user_agent() const { return client_.get_user_agent(); }
    bool get_auto_decompress() const { return client_.get_auto_decompress(); }
    bool get_verify_ssl() const { return client_.get_verify_ssl(); }
    const body_logging& get_body_logging() const { return client_.get_body_logging(); }
    size_t get_max_content_size() const { return client_.get_max_content_size(); }
    size_t get_max_idle_connections_per_host() const { return client_.get_max_idle_connections_per_host(); }
    size_t get_max_idle_connections() const { return client_.get_max_idle_connections(); }
    std::chrono::milliseconds get_idle_timeout() const { return client_.get_idle_timeout(); }

    std::string proxy_for(const std::string& url) const { return client_.proxy_for(url); }
    std::string proxy_for(const http_request& request) const { return client_.proxy_for(request); }

    std::shared_ptr<http_request> create_request(method m, const std::string& url) { return client_.create_request(m, url); }

    void clear_connections() { client_.clear_connections(); }
    size_t pool_size() const { return client_.pool_size(); }
    connection_pool::stats pool_stats() const { return client_.pool_stats(); }

    // --- Awaitable methods (see http_client_base) ---
    // Inside run(), the request keeps what they use alive; elsewhere, the client must outlive
    // them

    awaitable<client_response> get(const std::string& url, headers_map headers = {}) {
        return client_.get(url, std::move(headers));
    }
    awaitable<client_response> post(const std::string& url, std::string body = {},
                                    std::string content_type = "application/json", headers_map headers = {}) {
        return client_.post(url, std::move(body), std::move(content_type), std::move(headers));
    }
    awaitable<client_response> post(const std::string& url, const form& form, headers_map headers = {}) {
        return client_.post(url, form, std::move(headers));
    }
    awaitable<client_response> put(const std::string& url, std::string body = {},
                                   std::string content_type = "application/json", headers_map headers = {}) {
        return client_.put(url, std::move(body), std::move(content_type), std::move(headers));
    }
    awaitable<client_response> patch(const std::string& url, std::string body = {},
                                     std::string content_type = "application/json", headers_map headers = {}) {
        return client_.patch(url, std::move(body), std::move(content_type), std::move(headers));
    }
    awaitable<client_response> del(const std::string& url, headers_map headers = {}) {
        return client_.del(url, std::move(headers));
    }
    awaitable<client_response> head(const std::string& url, headers_map headers = {}) {
        return client_.head(url, std::move(headers));
    }
    awaitable<client_response> options(const std::string& url, headers_map headers = {}) {
        return client_.options(url, std::move(headers));
    }
    awaitable<client_response> send(std::shared_ptr<http_request> request) { return client_.send(std::move(request)); }
    awaitable<stream_result> send_streaming(std::shared_ptr<http_request> request, stream_callback callback) {
        return client_.send_streaming(std::move(request), std::move(callback));
    }
    awaitable<std::pair<client_response, std::shared_ptr<client_connection>>>
    send_with_connection(std::shared_ptr<http_request> request) {
        return client_.send_with_connection(std::move(request));
    }
    // The set http_client_base declares (and H8 counts there), forwarded with its return type
    auto upgrade_websocket(const std::string& url, const std::string& subprotocol = "") {
        return client_.upgrade_websocket(url, subprotocol);
    }
    auto upgrade_websocket(std::shared_ptr<http_request> request, const std::string& subprotocol = "") {
        return client_.upgrade_websocket(std::move(request), subprotocol);
    }

    // --- Callback-based async methods ---
    // A simpler interface when you're not already in a coroutine: the callback is invoked with
    // the response when the request completes (see the usage at the top)

    template<typename Callback>
    void get(const std::string& url, Callback&& callback, headers_map headers = {}) {
        run_with_callback([url, h = std::move(headers)](auto c) mutable { return c.client.get(url, std::move(h)); },
                          std::forward<Callback>(callback));
    }

    template<typename Callback>
    void post(const std::string& url, Callback&& callback, std::string body = {},
              std::string content_type = "application/json", headers_map headers = {}) {
        run_with_callback([url, b = std::move(body), ct = std::move(content_type), h = std::move(headers)](auto c) mutable {
            return c.client.post(url, std::move(b), std::move(ct), std::move(h));
        }, std::forward<Callback>(callback));
    }

    template<typename Callback>
    void post(const std::string& url, const form& form, Callback&& callback, headers_map headers = {}) {
        run_with_callback([url, f = form, h = std::move(headers)](auto c) mutable { return c.client.post(url, f, std::move(h)); },
                          std::forward<Callback>(callback));
    }

    template<typename Callback>
    void put(const std::string& url, Callback&& callback, std::string body = {},
             std::string content_type = "application/json", headers_map headers = {}) {
        run_with_callback([url, b = std::move(body), ct = std::move(content_type), h = std::move(headers)](auto c) mutable {
            return c.client.put(url, std::move(b), std::move(ct), std::move(h));
        }, std::forward<Callback>(callback));
    }

    template<typename Callback>
    void patch(const std::string& url, Callback&& callback, std::string body = {},
               std::string content_type = "application/json", headers_map headers = {}) {
        run_with_callback([url, b = std::move(body), ct = std::move(content_type), h = std::move(headers)](auto c) mutable {
            return c.client.patch(url, std::move(b), std::move(ct), std::move(h));
        }, std::forward<Callback>(callback));
    }

    template<typename Callback>
    void del(const std::string& url, Callback&& callback, headers_map headers = {}) {
        run_with_callback([url, h = std::move(headers)](auto c) mutable { return c.client.del(url, std::move(h)); },
                          std::forward<Callback>(callback));
    }

    template<typename Callback>
    void head(const std::string& url, Callback&& callback, headers_map headers = {}) {
        run_with_callback([url, h = std::move(headers)](auto c) mutable { return c.client.head(url, std::move(h)); },
                          std::forward<Callback>(callback));
    }

    template<typename Callback>
    void options(const std::string& url, Callback&& callback, headers_map headers = {}) {
        run_with_callback([url, h = std::move(headers)](auto c) mutable { return c.client.options(url, std::move(h)); },
                          std::forward<Callback>(callback));
    }

    // --- Streaming methods ---

    // Send a streaming request with callback: stream_cb takes each chunk, result_cb the result
    template<typename Callback>
    void send_streaming(std::shared_ptr<http_request> request, stream_callback stream_cb, Callback&& result_cb) {
        run_with_callback([req = std::move(request), scb = std::move(stream_cb)](auto c) mutable {
            return c.client.send_streaming(std::move(req), c.chunks(std::move(scb)));
        }, std::forward<Callback>(result_cb));
    }

    // Streaming GET with callback
    template<typename Callback>
    void get_streaming(const std::string& url, stream_callback stream_cb, Callback&& result_cb, headers_map headers = {}) {
        auto request = std::make_shared<http_request>();
        request->set_url(url);
        request->set_method(method::GET);
        for (const auto& [key, value] : headers) request->add_header(key, value);
        send_streaming(std::move(request), std::move(stream_cb), std::forward<Callback>(result_cb));
    }

    // Download file with progress callback
    template<typename Callback>
    void download(const std::string& url, const std::filesystem::path& path,
                  Callback&& result_cb, progress_callback progress = {}) {
        run_with_callback([url, path, prog = std::move(progress)](auto c) mutable -> awaitable<stream_result> {
            std::ofstream file(path, std::ios::binary);
            if (!file) {
                stream_result result;
                result.error = "Cannot open file for writing: " + path.string();
                co_return result;
            }
            auto request = std::make_shared<http_request>();
            request->set_url(url);
            request->set_method(method::GET);
            co_return co_await c.client.send_streaming(request, c.chunks([&file, &prog](const stream_info& info) {
                file.write(info.data.data(), static_cast<std::streamsize>(info.data.size()));
                if (prog) prog(info.downloaded, info.total);
                return true;
            }));
        }, std::forward<Callback>(result_cb));
    }

    // --- Request builder for fluent API ---

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

    // --- WebSocket ---

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
        return client_.upgrade_websocket(url, subprotocol);
    }

    // Connect to a WebSocket server with custom request (for builder pattern)
    awaitable<std::optional<websocket_client>> websocket(
        std::shared_ptr<http_request> request, const std::string& subprotocol = "") {
        return client_.upgrade_websocket(std::move(request), subprotocol);
    }

    /**
     * Connect to a WebSocket server (callback version).
     *
     * Usage:
     *   client.websocket("ws://server.com/path", [](auto ws) -> awaitable<void> {
     *       if (ws) {
     *           co_await ws->send_text_async("Hello!");
     *           auto [msg, binary] = co_await ws->receive_async();
     *           co_await ws->close_async();
     *       }
     *   });
     *
     * The callback receives a shared_ptr to maintain ownership (null if the connection
     * failed). It runs on a worker, which runs the io_context of the WebSocket: there the
     * synchronous calls of websocket_client (send_text(), receive(), close()...) throw
     * std::logic_error. Use its coroutine API, as above (the callback may be a coroutine),
     * run() with on_message() (the loop runs while the websocket_client lives: keep the
     * shared_ptr), or stop() to close without waiting.
     */
    template<typename Callback>
        requires std::invocable<Callback&, std::shared_ptr<websocket_client>>   // not a subprotocol
    void websocket(const std::string& url, Callback&& callback, const std::string& subprotocol = "") {
        websocket_with_callback(url, subprotocol, std::forward<Callback>(callback));
    }
};

} // namespace thinger::http

#endif

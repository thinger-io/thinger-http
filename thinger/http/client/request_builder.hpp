#ifndef THINGER_HTTP_CLIENT_REQUEST_BUILDER_HPP
#define THINGER_HTTP_CLIENT_REQUEST_BUILDER_HPP

#include "../common/http_request.hpp"
#include "client_response.hpp"
#include "stream_types.hpp"
#include "websocket_client.hpp"
#include "form.hpp"
#include <filesystem>
#include <fstream>
#include <string>
#include <map>
#include <functional>
#include <optional>

namespace thinger::http {

/**
 * Fluent builder for HTTP requests with streaming support.
 * Works with both sync (client) and async (async_client) clients.
 *
 * Usage with sync client:
 *   auto res = client.request("https://api.com/data")
 *       .header("Authorization", "Bearer xxx")
 *       .get();  // Returns client_response
 *
 * Usage with async client:
 *   auto res = co_await async_client.request("https://api.com/data")
 *       .header("Authorization", "Bearer xxx")
 *       .get();  // Returns awaitable<client_response>
 */
template<typename Client>
class request_builder {
public:
    request_builder(Client* c, const std::string& url)
        : client_(c)
        , request_(std::make_shared<http_request>()) {
        request_->set_url(url);
    }

    // Non-copyable, movable
    request_builder(const request_builder&) = delete;
    request_builder& operator=(const request_builder&) = delete;
    request_builder(request_builder&&) = default;
    request_builder& operator=(request_builder&&) = default;

    // ============================================
    // Configuration methods (chainable)
    // ============================================

    request_builder& header(const std::string& name, const std::string& value) {
        request_->add_header(name, value);
        return *this;
    }

    request_builder& headers(const std::map<std::string, std::string>& hdrs) {
        for (const auto& [name, value] : hdrs) {
            request_->add_header(name, value);
        }
        return *this;
    }

    request_builder& body(std::string content, std::string content_type = "application/json") {
        body_content_ = std::move(content);
        body_content_type_ = std::move(content_type);
        return *this;
    }

    request_builder& body(const form& f) {
        body_content_ = f.body();
        body_content_type_ = f.content_type();
        return *this;
    }

    request_builder& protocol(std::string proto) {
        protocol_ = std::move(proto);
        return *this;
    }

    // ============================================
    // Terminal methods - return type depends on Client
    // sync client: returns value directly
    // async client: returns awaitable
    // ============================================

    auto get() {
        return execute(method::GET);
    }

    auto get(stream_callback callback) {
        return execute_streaming(method::GET, std::move(callback));
    }

    auto post() {
        return execute(method::POST);
    }

    auto post(stream_callback callback) {
        return execute_streaming(method::POST, std::move(callback));
    }

    auto put() {
        return execute(method::PUT);
    }

    auto put(stream_callback callback) {
        return execute_streaming(method::PUT, std::move(callback));
    }

    auto patch() {
        return execute(method::PATCH);
    }

    auto patch(stream_callback callback) {
        return execute_streaming(method::PATCH, std::move(callback));
    }

    auto del() {
        return execute(method::DELETE);
    }

    auto del(stream_callback callback) {
        return execute_streaming(method::DELETE, std::move(callback));
    }

    auto head() {
        return execute(method::HEAD);
    }

    auto options() {
        return execute(method::OPTIONS);
    }

    /**
     * Download response body to a file.
     * For sync client: blocks and returns stream_result
     * For async client: returns awaitable<stream_result>
     */
    auto download(const std::filesystem::path& path, progress_callback progress = {}) {
        // Open file for writing
        auto file = std::make_shared<std::ofstream>(path, std::ios::binary);
        if (!*file) {
            if constexpr (requires { client_->send(request_).body(); }) {
                // Sync client - return error directly
                stream_result result;
                result.error = "Cannot open file for writing: " + path.string();
                return result;
            } else {
                // Async client - this path won't be taken at runtime due to the check above
                // but we need to return the right type
            }
        }

        // Stream to file
        return get([file, progress](const stream_info& info) {
            file->write(info.data.data(), static_cast<std::streamsize>(info.data.size()));
            if (progress) {
                progress(info.downloaded, info.total);
            }
            return true;
        });
    }

    /**
     * Upgrade connection to WebSocket.
     * For sync client: returns std::optional<websocket_client>
     * For async client: returns awaitable<std::optional<websocket_client>>
     */
    auto websocket() {
        return client_->websocket(request_, protocol_);
    }

    // ============================================
    // Callback-based terminal methods (async_client only)
    // ============================================
    // These are only available for async_client, which has a run() method.
    // They allow using the builder pattern without coroutines.
    //
    // Usage:
    //   client.request("https://api.com/data")
    //       .header("Authorization", "Bearer xxx")
    //       .get([](auto& response) {
    //           std::cout << response.body() << std::endl;
    //       });
    //   client.wait();

    template<typename Callback>
        requires requires { std::declval<Client>().run(std::declval<std::function<awaitable<void>()>>()); }
    void get(Callback&& callback) {
        execute_with_callback(method::GET, std::forward<Callback>(callback));
    }

    template<typename Callback>
        requires requires { std::declval<Client>().run(std::declval<std::function<awaitable<void>()>>()); }
    void post(Callback&& callback) {
        execute_with_callback(method::POST, std::forward<Callback>(callback));
    }

    template<typename Callback>
        requires requires { std::declval<Client>().run(std::declval<std::function<awaitable<void>()>>()); }
    void put(Callback&& callback) {
        execute_with_callback(method::PUT, std::forward<Callback>(callback));
    }

    template<typename Callback>
        requires requires { std::declval<Client>().run(std::declval<std::function<awaitable<void>()>>()); }
    void patch(Callback&& callback) {
        execute_with_callback(method::PATCH, std::forward<Callback>(callback));
    }

    template<typename Callback>
        requires requires { std::declval<Client>().run(std::declval<std::function<awaitable<void>()>>()); }
    void del(Callback&& callback) {
        execute_with_callback(method::DELETE, std::forward<Callback>(callback));
    }

    template<typename Callback>
        requires requires { std::declval<Client>().run(std::declval<std::function<awaitable<void>()>>()); }
    void head(Callback&& callback) {
        execute_with_callback(method::HEAD, std::forward<Callback>(callback));
    }

    template<typename Callback>
        requires requires { std::declval<Client>().run(std::declval<std::function<awaitable<void>()>>()); }
    void options(Callback&& callback) {
        execute_with_callback(method::OPTIONS, std::forward<Callback>(callback));
    }

    template<typename Callback>
        requires requires { std::declval<Client>().run(std::declval<std::function<awaitable<void>()>>()); }
    void websocket(Callback&& callback) {
        auto req = request_;
        auto proto = protocol_;
        client_->run([client = client_, req, proto, cb = std::forward<Callback>(callback)]() mutable -> awaitable<void> {
            auto result = co_await client->http_client_base::upgrade_websocket(std::move(req), proto);
            if (result) {
                auto ws = std::make_shared<websocket_client>(std::move(*result));
                cb(ws);
            } else {
                cb(std::shared_ptr<websocket_client>{});
            }
        });
    }

private:
    Client* client_;
    std::shared_ptr<http_request> request_;
    std::string body_content_;
    std::string body_content_type_;
    std::string protocol_;

    auto execute(method m) {
        request_->set_method(m);
        if (!body_content_.empty()) {
            request_->set_content(std::move(body_content_), std::move(body_content_type_));
        }
        return client_->send(request_);
    }

    auto execute_streaming(method m, stream_callback callback) {
        request_->set_method(m);
        if (!body_content_.empty()) {
            request_->set_content(std::move(body_content_), std::move(body_content_type_));
        }
        return client_->send_streaming(request_, std::move(callback));
    }

    template<typename Callback>
    void execute_with_callback(method m, Callback&& callback) {
        request_->set_method(m);
        if (!body_content_.empty()) {
            request_->set_content(std::move(body_content_), std::move(body_content_type_));
        }
        auto req = request_;
        client_->run([client = client_, req, cb = std::forward<Callback>(callback)]() mutable -> awaitable<void> {
            auto response = co_await client->http_client_base::send(std::move(req));
            cb(response);
        });
    }
};

} // namespace thinger::http

#endif // THINGER_HTTP_CLIENT_REQUEST_BUILDER_HPP

#ifndef THINGER_HTTP_WEBSOCKET_CLIENT_HPP
#define THINGER_HTTP_WEBSOCKET_CLIENT_HPP

#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <functional>
#include <atomic>

#include "../../asio/sockets/websocket.hpp"
#include "../../util/types.hpp"

namespace thinger::http {

/**
 * Represents an established WebSocket connection from the client side.
 *
 * This class provides a clean synchronous API for WebSocket operations.
 * Obtained via client.websocket() or async_client.websocket().
 *
 * Usage:
 *   http::client client;
 *   if (auto ws = client.websocket("ws://server.com/path")) {
 *       ws->send_text("Hello!");
 *       auto [msg, binary] = ws->receive();
 *       ws->close();
 *   }
 */
class websocket_client {
public:
    using message_callback = std::function<void(const std::string&, bool)>;
    using close_callback = std::function<void()>;
    using error_callback = std::function<void(const std::string&)>;

    /**
     * Construct from an established WebSocket.
     * Not intended for direct use - use client.websocket() instead.
     */
    explicit websocket_client(std::shared_ptr<asio::websocket> ws);

    ~websocket_client();

    // Non-copyable
    websocket_client(const websocket_client&) = delete;
    websocket_client& operator=(const websocket_client&) = delete;

    // Movable
    websocket_client(websocket_client&& other) noexcept;
    websocket_client& operator=(websocket_client&& other) noexcept;

    // ============================================
    // Synchronous API
    // ============================================

    /**
     * Send a text message.
     * @return true if sent successfully
     */
    bool send_text(std::string_view message);

    /**
     * Send a binary message.
     * @return true if sent successfully
     */
    bool send_binary(const void* data, size_t size);
    bool send_binary(std::string_view data);

    /**
     * Receive the next message.
     * Blocks until a message is received or connection closes.
     * @return {message, is_binary} pair, empty message on error/close
     */
    std::pair<std::string, bool> receive();

    /**
     * Close the connection gracefully.
     */
    void close();

    /**
     * Check if connection is open.
     */
    bool is_open() const;

    /**
     * Check if connection is valid (for use with optional).
     */
    explicit operator bool() const { return is_open(); }

    /**
     * Release ownership of the underlying WebSocket.
     * After calling this method, the websocket_client becomes invalid.
     * Use this to take full control of the raw WebSocket for advanced use cases.
     * @return The underlying WebSocket, or nullptr if already released
     */
    std::shared_ptr<asio::websocket> release_socket();

    // ============================================
    // Async/Callback API (for async_client usage)
    // ============================================

    /**
     * Set callback for incoming messages.
     * Used with run() for event-driven operation.
     */
    void on_message(message_callback callback) { on_message_ = std::move(callback); }

    /**
     * Set callback for connection close.
     */
    void on_close(close_callback callback) { on_close_ = std::move(callback); }

    /**
     * Set callback for errors.
     */
    void on_error(error_callback callback) { on_error_ = std::move(callback); }

    /**
     * Start the message receive loop (non-blocking).
     * Messages are delivered via on_message callback.
     */
    void run();

    /**
     * Stop the message loop and close connection.
     */
    void stop();

    // ============================================
    // Coroutine API (for advanced use)
    // ============================================

    /**
     * Send text message asynchronously.
     */
    awaitable<bool> send_text_async(std::string message);

    /**
     * Send binary message asynchronously.
     */
    awaitable<bool> send_binary_async(std::vector<uint8_t> data);

    /**
     * Receive message asynchronously.
     */
    awaitable<std::pair<std::string, bool>> receive_async();

    /**
     * Close connection asynchronously.
     */
    awaitable<void> close_async();

private:
    std::shared_ptr<asio::websocket> websocket_;
    std::atomic<bool> running_{false};

    // Callbacks
    message_callback on_message_;
    close_callback on_close_;
    error_callback on_error_;

    // Internal message loop coroutine
    awaitable<void> message_loop();
};

} // namespace thinger::http

#endif // THINGER_HTTP_WEBSOCKET_CLIENT_HPP

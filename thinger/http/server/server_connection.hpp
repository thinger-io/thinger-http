#ifndef THINGER_SERVER_HTTP_SERVER_CONNECTION_HPP
#define THINGER_SERVER_HTTP_SERVER_CONNECTION_HPP

#include <queue>
#include <atomic>
#include <mutex>
#include "request_factory.hpp"
#include "../common/http_frame.hpp"
#include "../common/http_request.hpp"
#include "../common/http_response.hpp"
#include "http_stream.hpp"
#include "request_handler.hpp"
#include "../../util/types.hpp"

namespace thinger::http {

class request;

class server_connection : public std::enable_shared_from_this<server_connection>, public boost::noncopyable {

    static constexpr size_t MAX_BUFFER_SIZE = 4096;
    static constexpr auto DEFAULT_TIMEOUT = std::chrono::seconds{120};
    static constexpr size_t DEFAULT_MAX_BODY_SIZE = 8 * 1024 * 1024; // 8MB

public:
    static std::atomic<unsigned long> connections;

    explicit server_connection(std::shared_ptr<asio::socket> socket);
    virtual ~server_connection();

    // Start processing requests (spawns the read loop coroutine)
    void start(std::chrono::seconds timeout = DEFAULT_TIMEOUT);

    // Release the socket for upgrades (WebSocket, etc.)
    std::shared_ptr<asio::socket> release_socket();

    // Release this instance without touching the socket
    void release();

    // Get the raw socket
    std::shared_ptr<asio::socket> get_socket();

    // Handle a response frame (can be called from any thread)
    void handle_stream(std::shared_ptr<http_stream> stream, std::shared_ptr<http_frame> frame);

    // Update connection timeout
    void update_connection_timeout(std::chrono::seconds timeout);

    // Set request handler (awaitable — dispatch coroutine)
    void set_handler(std::function<awaitable<void>(std::shared_ptr<request>)> handler) {
        handler_ = std::move(handler);
    }

    // Set maximum allowed body size
    void set_max_body_size(size_t size) {
        max_body_size_ = size;
    }

private:
    // Main read loop coroutine
    awaitable<void> read_loop();

    // Write output queue
    awaitable<void> write_frame(std::shared_ptr<http_stream> stream, std::shared_ptr<http_frame> frame);

    // Process the output queue
    void process_output_queue();

    // Handle stock error responses
    void handle_stock_error(std::shared_ptr<http_stream> stream, http_response::status status);

    // Reset timeout timer
    void reset_timeout();

    // Close connection
    void close();

private:
    std::shared_ptr<asio::socket> socket_;
    boost::asio::steady_timer timeout_timer_;
    std::chrono::seconds timeout_{DEFAULT_TIMEOUT};

    uint8_t buffer_[MAX_BUFFER_SIZE];
    request_factory request_parser_;

    // Queue for HTTP pipelining
    std::queue<std::shared_ptr<http_stream>> request_queue_;
    std::mutex queue_mutex_;

    // Request handler callback (awaitable coroutine)
    std::function<awaitable<void>(std::shared_ptr<request>)> handler_;

    // State
    bool writing_{false};
    bool running_{false};
    stream_id request_id_{0};
    size_t max_body_size_{DEFAULT_MAX_BODY_SIZE};
};

}

#endif

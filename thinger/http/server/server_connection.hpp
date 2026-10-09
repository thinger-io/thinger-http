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
#include "../../util/types.hpp"

namespace thinger::http {

class request;

// Receives a connection taken over from the HTTP server: the socket, and any bytes the
// server had already read past the request that took it over
using takeover_handler = std::function<void(std::shared_ptr<asio::socket>, std::string buffered)>;

class server_connection : public std::enable_shared_from_this<server_connection>, public boost::noncopyable {

    static constexpr size_t MAX_BUFFER_SIZE = 4096;
    static constexpr auto DEFAULT_TIMEOUT = std::chrono::seconds{120};

public:
    static std::atomic<unsigned long> connections;

    explicit server_connection(std::shared_ptr<asio::socket> socket);
    virtual ~server_connection();

    // Start processing requests (spawns the read loop coroutine)
    void start(std::chrono::seconds timeout = DEFAULT_TIMEOUT);

    // Release the socket for upgrades (WebSocket, etc.)
    std::shared_ptr<asio::socket> release_socket();

    // Take over the connection: once the current request has been handled and its
    // response written, stop serving HTTP and hand the socket to `handler`
    void begin_takeover(takeover_handler handler);

    // Called when the response of the request taking over the connection is written
    void takeover_response_sent();

    // Whether the connection is being (or has been) taken over
    bool is_taken_over() const { return takeover_requested_; }

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

    // Hand the socket over once the response is written and the read loop has stopped
    void complete_takeover();

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

    // Connection takeover
    takeover_handler takeover_handler_;
    std::string takeover_buffer_;
    bool takeover_requested_{false};
    bool takeover_response_sent_{false};
    bool takeover_reader_stopped_{false};
};

}

#endif

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

    // Take over the connection from a stream: send `response`, then, once its request
    // has been handled, stop serving HTTP and hand the socket to `handler` (can be called
    // from any thread). If the connection was closed meanwhile (e.g. the client left while
    // the response was pending), nothing is sent and the handler is released without
    // being called.
    void take_over(std::shared_ptr<http_stream> stream, std::shared_ptr<http_frame> response,
                   takeover_handler handler);

    // Release this instance without touching the socket
    void release();

    // Get the raw socket
    std::shared_ptr<asio::socket> get_socket();

    // Queue a response frame, in call order (can be called from any thread)
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

    // Wait until the response of a stream has started, or the connection is closed
    awaitable<void> wait_response(const http_stream& stream);

    // Wait until the client sends data (false) or closes the connection (true)
    awaitable<bool> wait_peer_closed();

    // Queue a response frame (on the connection executor)
    void queue_frame(const std::shared_ptr<http_stream>& stream, std::shared_ptr<http_frame> frame);

    // Write output queue
    awaitable<void> write_frame(std::shared_ptr<http_stream> stream, std::shared_ptr<http_frame> frame);

    // Process the output queue
    void process_output_queue();

    // Handle stock error responses
    void handle_stock_error(std::shared_ptr<http_stream> stream, http_response::status status);

    // Extend the connection deadline after some activity
    void reset_timeout();

    // Wait for the timer up to the deadline (it is re-armed there if the deadline moved)
    void arm_timeout();

    // Stop the timer (the next reset_timeout() arms it again)
    void cancel_timeout();

    // Close connection
    void close();

    // Hand the socket over once the response is written and the read loop has stopped
    void complete_takeover(http_stream& stream);

private:
    std::shared_ptr<asio::socket> socket_;

    // The connection is closed when its deadline passes. Activity just moves the deadline:
    // the timer is not touched on every request, it only checks the deadline on expiry
    boost::asio::steady_timer timeout_timer_;
    std::chrono::steady_clock::time_point deadline_;
    bool timeout_armed_{false};

    // Cancelled when a response starts, waking the read loop waiting for it
    boost::asio::steady_timer response_started_;
    bool waiting_response_{false};
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
    // The read loop stopped without handing the connection over: it cannot be taken over
    bool reader_stopped_{false};
    stream_id request_id_{0};
};

}

#endif

#ifndef THINGER_SERVER_HTTP_SERVER_CONNECTION_HPP
#define THINGER_SERVER_HTTP_SERVER_CONNECTION_HPP

#include <queue>
#include <atomic>
#include "request_factory.hpp"
#include "../common/http_frame.hpp"
#include "../common/http_request.hpp"
#include "../common/http_response.hpp"
#include "http_stream.hpp"
#include "connection_tracker.hpp"
#include "../../util/types.hpp"

namespace thinger::http {

class request;

class server_connection : public std::enable_shared_from_this<server_connection>, public tracked_connection {

    static constexpr size_t MAX_BUFFER_SIZE = 4096;
    static constexpr auto DEFAULT_TIMEOUT = std::chrono::seconds{120};

public:
    explicit server_connection(std::shared_ptr<asio::socket> socket);
    virtual ~server_connection();

    // Count the connection among those of a server (its statistics, and its shutdown), before
    // start(). A connection taken over then counts as such, while its socket lives
    void track(const std::shared_ptr<connection_tracker>& tracker);

    // Start processing requests (spawns the read loop coroutine). Closed right away if the
    // server it is tracked by shuts down
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

    // Send the 100 Continue the client of `stream` waits for before sending the body (Expect:
    // 100-continue), as the body is about to be read: once, in order with the responses, and
    // only while its response has not started (on the connection thread)
    void send_continue(http_stream& stream);

    // Update connection timeout
    void update_connection_timeout(std::chrono::seconds timeout);

    // Extend the connection deadline after some activity outside the read loop, such as
    // reading the request body (on the connection thread)
    void extend_timeout();

    // Keep the connection open at least until `time`, whatever its activity: a request with
    // a timeout of its own (see route::timeout) may wait for its response longer than the
    // connection timeout (on the connection thread)
    void hold_until(std::chrono::steady_clock::time_point time);

    // Lingering close (RFC 9112 section 9.6), for the connections of every server in the
    // process: a connection closed after a response while input of its request may still be
    // arriving (e.g. a body over the limit, answered 413 before it was read) stops sending, then
    // reads and drops what arrives until the client closes, `max_bytes` were dropped or `timeout`
    // passed, whatever comes first, and closes. Closed at once instead, the unread data makes the
    // system reset the connection, and a client still sending may never read the response. Zero
    // for either closes at once (also on sockets that cannot half-close, such as custom ones).
    // A graceful shutdown of the server closes the connections lingering at once
    static constexpr size_t default_linger_max_bytes = 8 * 1024 * 1024;
    static constexpr std::chrono::milliseconds default_linger_timeout{5000};
    static void set_lingering_close(size_t max_bytes, std::chrono::milliseconds timeout);

    // Whether the connection closes gracefully (see set_lingering_close()): nothing more is read
    // from it but by itself (on the connection thread)
    bool lingering() const { return lingering_; }

    void set_handler(std::function<awaitable<void>(std::shared_ptr<request>)> handler) {
        handler_ = std::move(handler);
    }

    // How the requests and responses are logged (see body_logging)
    void set_body_logging(const body_logging& options) { body_logging_ = options; }

private:
    // Read requests, one after another, and hand each to the handler
    awaitable<void> read_loop();

    // The parsed request, queued for its response, with the bytes read past its headers
    std::pair<std::shared_ptr<http_stream>, std::shared_ptr<request>> start_request(
        const std::shared_ptr<server_connection>& self, const uint8_t* read_ahead, size_t read_ahead_size);

    // Answer a request that cannot be parsed, or whose header section is too large
    void reject_request();

    // The request took the connection over: stop reading, and hand it what was read past it
    void hand_over_reader(http_stream& stream, request& req);

    // Move what was read past the request to the buffer, for the next request; false if it
    // does not fit
    bool keep_pipelined(request& req, size_t& buffered);

    // The connection ended without being taken over
    void stop_reading();

    // The server shuts down: close once idle, or after the response in progress (which tells
    // the client); or close right away
    void shutdown_drain() override;
    void shutdown_close() override;

    // A frame is queued while frame_watch_ is set. Kept out of queue_frame(), which only tests
    // the flags on the way of every frame
    [[gnu::noinline, gnu::cold]] void watch_frame(const std::shared_ptr<http_stream>& stream,
                                                  const std::shared_ptr<http_frame>& frame);

    // Wait until the response of a stream has started, or the connection is closed
    awaitable<void> wait_response(const http_stream& stream);

    // Wait until the client sends data or closes the connection; a socket that cannot wait
    // (e.g. one defined by an application) is not watched
    enum class peer_state { data, closed, not_watchable };
    awaitable<peer_state> wait_peer_closed();

    // Queue a response frame (on the connection executor)
    void queue_frame(const std::shared_ptr<http_stream>& stream, std::shared_ptr<http_frame> frame);

    // Once a frame of `stream` is written (or its write failed): close the connection or
    // hand it over when its response ends, as required
    void frame_written(http_stream& stream, const std::shared_ptr<http_frame>& frame, bool written);

    // A frame of a stream whose response is observed (see http_application::on_response) is
    // about to be written
    [[gnu::noinline, gnu::cold]] static void observe_frame(http_stream& stream, http_frame& frame);

    // Write the queued frames, in order
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

    // Close the connection once the response of `stream` ended: gracefully if input of its
    // request may still be arriving (see linger()), else at once
    void close_after(const http_stream& stream);

    // Close gracefully (RFC 9112 section 9.6): stop sending, drop what keeps arriving (see
    // set_lingering_close()), then close. At once if the socket cannot half-close
    void linger();
    awaitable<void> drop_input(size_t max_bytes, std::chrono::milliseconds timeout);

    // Hand the socket over once the response is written and the read loop has stopped
    void complete_takeover(http_stream& stream);

private:
    std::shared_ptr<asio::socket> socket_;

    // The connection is closed when its deadline passes. Activity just moves the deadline:
    // the timer is not touched on every request, it only checks the deadline on expiry
    boost::asio::steady_timer timeout_timer_;
    std::chrono::steady_clock::time_point deadline_;
    // Not closed before this time either (see hold_until)
    std::chrono::steady_clock::time_point hold_{};
    bool timeout_armed_{false};

    // Cancelled when a response starts, waking the read loop waiting for it
    boost::asio::steady_timer response_started_;

    // Rare cases where a queued frame is looked at, all tested at once on the way of every
    // frame: the read loop waits for a response to start, the connection drains, or the
    // request in progress waits for its 100 Continue
    enum : uint8_t { watch_response_start = 1, watch_draining = 2, watch_continue = 4 };
    uint8_t frame_watch_{0};

    // Stream of the request the read loop is handling, none while it waits for a request
    http_stream* in_progress_{nullptr};
    std::chrono::seconds timeout_{DEFAULT_TIMEOUT};

    uint8_t buffer_[MAX_BUFFER_SIZE];
    request_factory request_parser_;

    // Queue for HTTP pipelining (only used from the connection thread, as everything that
    // reads or writes the connection)
    std::queue<std::shared_ptr<http_stream>> request_queue_;

    std::function<awaitable<void>(std::shared_ptr<request>)> handler_;

    body_logging body_logging_;

    // Buffers of the frame being written (kept, so they are not allocated for every frame)
    std::vector<boost::asio::const_buffer> write_buffers_;

    // State
    bool writing_{false};
    bool running_{false};
    // The read loop stopped without handing the connection over: it cannot be taken over
    bool reader_stopped_{false};
    // Closing gracefully (see linger()). Where it takes no room
    bool lingering_{false};
    stream_id request_id_{0};
};

}

#endif

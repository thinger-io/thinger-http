#ifndef THINGER_SERVER_HTTP_SERVER_CONNECTION_HPP
#define THINGER_SERVER_HTTP_SERVER_CONNECTION_HPP

#include <deque>
#include <atomic>
#include "request_factory.hpp"
#include "../common/http_frame.hpp"
#include "../common/http_request.hpp"
#include "../common/http_response.hpp"
#include "http_stream.hpp"
#include "connection_tracker.hpp"
#include "../../asio/async_event.hpp"
#include "../../asio/watchdog.hpp"
#include "../../util/types.hpp"

namespace thinger::http {

class request;
struct connection_options;

namespace detail {
    class connection_source;
}

class server_connection : public std::enable_shared_from_this<server_connection>, public tracked_connection {

    static constexpr size_t MAX_BUFFER_SIZE = 4096;

public:
    // A connection served with the settings of `options` (its server's, see
    // server_base::set_connection_options()): the defaults if null
    explicit server_connection(std::shared_ptr<asio::socket> socket,
                               std::shared_ptr<const connection_options> options = nullptr);
    virtual ~server_connection();

    // Count the connection among those of a server (its statistics, and its shutdown), before
    // start(). A connection taken over then counts as such, while its socket lives
    void track(const std::shared_ptr<connection_tracker>& tracker);

    // Start processing requests (spawns the read loop coroutine), closed once idle for the
    // idle_timeout of its options, or at the deadlines of a request (see connection_options).
    // Closed right away if the server it is tracked by shuts down
    void start();

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

    // Do not close the connection as idle before `time`: a request with a timeout of its own
    // (see route::timeout) may wait for its response longer than the idle timeout. The
    // deadlines of a request still apply (on the connection thread)
    void hold_until(std::chrono::steady_clock::time_point time);

    // Whether the connection closes gracefully (see linger()): nothing more is read from it but
    // by itself (on the connection thread)
    bool lingering() const { return lingering_; }

    void set_handler(std::function<awaitable<void>(std::shared_ptr<request>)> handler) {
        handler_ = std::move(handler);
    }

private:
    // The source of a request read here, for its stream (see detail::request_source)
    friend class detail::connection_source;
    std::unique_ptr<detail::request_source> make_source(http_stream& stream);

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

    // Once reading stops after a request whose response ended: close gracefully if its body
    // still comes
    void linger_unread(const http_stream& stream);

    // The server shuts down: close once idle, or after the response in progress (which tells
    // the client); or close right away. At the deadline, before the close, the handlers still
    // running are cancelled (cancel_reason::shutdown)
    void shutdown_drain() override;
    void shutdown_cancel() override;
    void shutdown_close() override;

    // A frame of `stream` is queued while frame_watch_ is set. Kept out of queue_frame(), which
    // only tests the flags on the way of every frame
    [[gnu::noinline, gnu::cold]] void watch_frame(http_stream& stream);

    // Wait until the response of a stream has started, or the connection is closed
    awaitable<void> wait_response(const http_stream& stream);

    // Requests read are bounded by the responses still to write out: no next request is read
    // while max_queued_responses wait in the queue, or the front one waits for the socket (a
    // client that does not read). Node pauses its parser so, Go reads one request at a time
    static constexpr size_t max_queued_responses = 16;
    bool output_backed_up() const { return writing_ || request_queue_.size() >= max_queued_responses; }
    // The read loop waits for the output before the next request. A head begun is read to its
    // end first, within its deadline, which a pause would eat
    bool reading_paused() const { return output_backed_up() && !reading_head_; }
    // Wait until the output is no longer backed up, or the connection stops; woken as a response
    // leaves the queue, or a write that waited completes
    awaitable<void> wait_output();
    void output_progressed() { if (output_waited_) [[unlikely]] reader_wake_->set(); }

    // The peer watcher: the only reader of the socket while a request is handled, and until
    // its response starts (the read loop reads it otherwise). Once the body of the request
    // was read, it waits for the socket to be readable: data (the next request) stays there,
    // a half-close makes the connection close after the response, and anything else (a reset)
    // cancels the scope of the request (client_gone) and closes the connection. A socket that
    // cannot wait (e.g. one defined by an application) is not watched
    awaitable<void> watch_loop();
    awaitable<void> watch_peer(http_stream& stream);
    // Watch the client for `stream`; stop it (on its own, but for a read in progress over
    // TLS, see watch_reading_); stop it for good, as the read loop stops
    void watch(const std::shared_ptr<http_stream>& stream);
    void unwatch();
    void stop_watcher();
    // Wake the watcher for the stream it watches: started then, the first time
    void wake_watcher();
    // Whether the watcher still watches for `stream`
    bool watching(const http_stream& stream) const;

    // Queue a response frame (on the connection executor)
    void queue_frame(const std::shared_ptr<http_stream>& stream, std::shared_ptr<http_frame> frame);

    // Once a frame of `stream` is written (or its write failed, with `ec`): close the
    // connection or hand it over when its response ends, as required
    void frame_written(http_stream& stream, const std::shared_ptr<http_frame>& frame,
                       const boost::system::error_code& ec);

    // A frame of a stream whose response is observed (see http_application::on_response) is
    // about to be written
    [[gnu::noinline, gnu::cold]] static void observe_frame(http_stream& stream, http_frame& frame);

    // Write the queued frames, in order
    void process_output_queue();

    // Handle stock error responses
    void handle_stock_error(std::shared_ptr<http_stream> stream, http_response::status status);

    // Close connection
    void close();

    // Close the connection once the response of `stream` ended: gracefully if input of its
    // request may still be arriving (see linger()), else at once
    void close_after(const http_stream& stream);

    // Close gracefully (RFC 9112 section 9.6): stop sending, drop what keeps arriving (up to
    // the linger limits of its options, see connection_options), then close. At once if the
    // socket cannot half-close, or either limit is zero; briefly if the server shuts down
    void linger();
    awaitable<void> drop_input(size_t max_bytes, std::chrono::milliseconds timeout);

    // Hand the socket over once the response is written and the read loop has stopped
    void complete_takeover(http_stream& stream);

    // The deadlines of the connection (server_connection_deadlines.cpp), all on timeout_timer_,
    // armed at the earliest of those that apply: idle (from the last activity, not before the
    // hold); while the head of a request is read, the header deadline, and while its body is,
    // the request deadline (both from the start of the request, see connection_options); while
    // a response waits for the socket, the write deadline instead of idle (from its last
    // progress). On expiry, the one that passed acts (see deadline_expired()); none passed (the
    // head completed, the body was read, or the write went on, meanwhile), it is armed again
    using clock = std::chrono::steady_clock;
    void arm_deadline();
    clock::time_point next_deadline() const;
    void deadline_expired();
    // Idle, or the write deadline while a response waits for the socket (see write_timeout)
    clock::time_point idle_deadline() const;
    // A frame waits for the socket (see process_output_queue()): its write deadline starts
    void write_started();
    // Whether `buffers` are written in slices, each one moving the write deadline (see
    // asio::socket::write_slices()): a write deadline applies and they exceed one
    bool write_in_slices(const std::vector<boost::asio::const_buffer>& buffers) const;
    // The deadline of the head being read, if any applies (false otherwise)
    bool head_deadline(clock::time_point& deadline) const;
    // Bytes of a request head are about to be parsed (`pipelined`: read with the request before
    // it): the first ones start the request, at the read that got them, or now for a pipelined
    // one. A read leaving its head incomplete arms its header deadline (see head_incomplete())
    void head_started(bool pipelined);
    void head_incomplete();
    // The head of a request was not complete at its deadline: 408, or a silent close if no
    // byte of it arrived
    void head_timed_out();
    // The deadline of the body of the request in progress, while it is not read to its end, if
    // any applies (false otherwise)
    bool body_deadline(clock::time_point& deadline) const;
    // The request deadline of the request of `stream`, set by its route (see
    // route::request_timeout), from the start of the request; zero for none
    void set_request_deadline(const http_stream& stream, std::chrono::milliseconds deadline);
    // The deadline the handler set for reading the body of the request of `stream` (only compared:
    // it may be gone), replacing its request deadline (see request::set_read_deadline()); the end
    // of time removes it
    void set_read_deadline(const http_stream* stream, clock::time_point deadline);
    // Whether a read deadline is set and has passed: a read then fails at once
    // (read_deadline_expired)
    bool read_deadline_passed() const;
    // A read of the body starts and ends, bound to read_cancel_, which the watchdog emits as its
    // deadline passes, whenever it was set: nothing else on the socket is cancelled, and the
    // same read goes on when the deadline only moves. `caller` is the cancellation of the
    // coroutine reading (its `||`, the scope of the request), forwarded to the read while it waits
    void body_read_started(boost::asio::cancellation_slot caller);
    void body_read_ended(boost::asio::cancellation_slot caller, io_result& result);
    // The body of the request of `stream` was not read to its end at its deadline: its scope is
    // cancelled (cancel_reason::timeout), and it is answered 408 unless its response started,
    // or another one is being written; the connection is closed after it (or at once). What
    // the handler reads or writes afterwards fails
    void request_timed_out(http_stream& stream);
    // Answer `status` with a stock reply on a stream of its own, closing the connection after
    // it (gracefully if `input_pending`)
    void answer_and_close(http_response::status status, detail::close_reason reason, bool input_pending);

private:
    std::shared_ptr<asio::socket> socket_;

    // The connection is closed when a deadline passes (see arm_deadline()): activity moves the
    // idle one later (see extend_timeout()), a request holds it (see hold_until())
    asio::watchdog timeout_timer_;

    // Wakes the read loop waiting for a response to start, or for the responses to write out
    // (see wait_output()); set as it stops too. Created by its first wait
    std::optional<asio::async_event> reader_wake_;

    // Bound to the read loop, which runs the handlers: the scope of the request being handled
    // emits it to interrupt its handler (see http_stream::bind_handler())
    boost::asio::cancellation_signal handler_signal_;

    // The peer watcher (see watch_peer()): the stream it watches for (none while idle), set
    // by the read loop, which wakes it (see wake_watcher()); the slot of its coroutine, emitted
    // to end its wait; set once a read of it ends. Its events are created as it starts
    std::shared_ptr<http_stream> watched_;
    std::optional<asio::async_event> watch_wake_;
    boost::asio::cancellation_signal watch_stop_;
    std::optional<asio::async_event> watch_idle_;

    // Rare cases where a queued frame matters, all tested at once on the way of every frame:
    // the read loop waits for a response to start, the connection drains, or the request in
    // progress waits for its 100 Continue
    enum : uint8_t { watch_response_start = 1, watch_draining = 2, watch_continue = 4 };
    uint8_t frame_watch_{0};

    // Stream of the request the read loop is handling, none while it waits for a request
    http_stream* in_progress_{nullptr};
    // The idle timeout (from the options, see update_connection_timeout())
    std::chrono::seconds timeout_;
    // The last activity (a read, a frame written), the idle deadline counting from it; the
    // connection is never closed as idle before the hold (see hold_until())
    clock::time_point last_activity_{};
    clock::time_point hold_{};
    // The start of the request being read: its first byte, the start of serving the connection
    // for the first one, or the start of parsing a pipelined one
    clock::time_point request_start_{};
    // The last progress of the frame waiting for the socket (see write_started())
    clock::time_point write_progress_{};
    // The request deadline of the request in progress while its body is read: the options',
    // or its route's (zero: none)
    std::chrono::milliseconds request_deadline_{0};
    // The read deadline its handler set (see set_read_deadline()), and the signal ending the body
    // read that waits (see body_read_started())
    clock::time_point read_deadline_{};
    boost::asio::cancellation_signal read_cancel_;

    uint8_t buffer_[MAX_BUFFER_SIZE];
    request_factory request_parser_;

    // Queue for HTTP pipelining (only used from the connection thread, as everything that
    // reads or writes the connection)
    std::deque<std::shared_ptr<http_stream>> request_queue_;

    std::function<awaitable<void>(std::shared_ptr<request>)> handler_;

    // The settings of its server, shared by its connections
    std::shared_ptr<const connection_options> options_;

    // Buffers of the frame being written (kept, so they are not allocated for every frame)
    std::vector<boost::asio::const_buffer> write_buffers_;

    // State
    bool writing_{false};
    bool running_{false};
    // The read loop stopped without handing the connection over: it cannot be taken over
    bool reader_stopped_{false};
    // Closing gracefully (see linger()). Where it takes no room
    bool lingering_{false};
    // The watcher waits (for the body to be read, or the socket to be readable): unwatch()
    // ends the wait. Or it reads (peer_closed(), which may wait for a TLS record)
    bool watch_waiting_{false};
    bool watch_reading_{false};
    // The head of a request is being read, incomplete: its header deadline applies
    bool reading_head_{false};
    // A frame waits for the socket, within its write deadline (see write_started())
    bool write_waits_{false};
    // The read loop waits for the output (see wait_output())
    bool output_waited_{false};
    // Whether the request in progress has a read deadline, a read of its body waits for the
    // socket, and it was ended at that deadline
    bool read_bounded_{false};
    bool read_waits_{false};
    bool read_expired_{false};
    stream_id request_id_{0};
};

}

#endif

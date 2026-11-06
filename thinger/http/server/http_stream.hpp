#ifndef HTTP_STREAM_HPP
#define HTTP_STREAM_HPP

#include <atomic>
#include <chrono>
#include <list>
#include <queue>
#include <memory>
#include <optional>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include "../common/http_frame.hpp"
#include "../../asio/async_event.hpp"
#include "../../asio/ssl/tls_session.hpp"
#include "../../util/types.hpp"
#include "upgrade.hpp"

namespace thinger::asio {
    class socket;
}

namespace thinger::http {

    class response_observer;
    class http_request;

    typedef uint32_t stream_id;

    // Why the scope of a request was cancelled (defined in request.hpp)
    enum class cancel_reason : uint8_t;

    namespace detail {

        // Why a connection closes after the response of a stream
        enum class close_reason : uint8_t {
            client_asked,       // the request said Connection: close (or its client sends no more)
            http10,             // an HTTP/1.0 request without keep-alive
            draining,           // the server shuts down
            continue_skipped,   // answered before the 100 Continue its client waits for
            body_unread,        // a request body left unread that could not be dropped
            body_error,         // a request (or its body) that could not be read: where it ends is unknown
            handler_failed,     // the handler failed once its response had started (or before routing)
            route_timeout,      // the route timed out with the request body still unread
            request_timeout,    // the request was not received within its deadline (answered 408)
            framing_by_close,   // a response body that ends when the connection closes
            aborted             // a streamed response aborted: its body is incomplete
        };

        // What happens to the connection once the response of a stream is written
        enum class after_response : uint8_t { keep_alive, close, take_over };

        // Interim response asking the client to send the body (RFC 9110 section 15.2.1)
        class continue_frame : public http_frame {
        public:
            continue_frame() { set_last_frame(false); }

            frame_kind kind() const override { return frame_kind::interim; }

            size_t get_size() override { return 0; }

            void to_buffer(std::vector<boost::asio::const_buffer>& buffer) const override;

            void log(const char* scope, const body_logging&) const override;
        };

    }

    class server_connection;

    // The connection a request came from (see request::connection()): empty for a request
    // without one, or once it is gone
    struct connection_info {
        std::string peer_ip;
        std::string peer_port;
        std::string local_port;
        bool secure = false;    // TLS
    };

    namespace detail {

        // Where a request reads its body from, and what it knows of its connection: the input
        // twin of response_sink, one per request, owned by it. A server connection reads its
        // client (connection_source, in connection_source.cpp); an in-memory dispatch reads
        // memory (memory_source)
        class request_source {
        public:
            virtual ~request_source() = default;

            // Up to `size` bytes of what follows the request head (the body reader frames them):
            // none once nothing more comes, with the error why. The network source sends the
            // 100 Continue its client waits for first, and moves the connection deadline
            virtual awaitable<io_result> read_body_some(uint8_t* buffer, size_t size) = 0;

            virtual connection_info info() const = 0;

            // The TLS session of the connection: none without TLS or without a connection
            virtual std::optional<asio::tls_session> tls() const { return std::nullopt; }

            // Whether a request for `host` is misdirected on the connection (see
            // asio::socket::misdirected()): false without TLS or without a connection
            virtual bool misdirected(std::string_view /*host*/) const { return false; }

            // Keep the connection open at least until `time`, for a request with a timeout of
            // its own (see server_connection::hold_until()); nothing without a connection
            virtual void hold_until(std::chrono::steady_clock::time_point /*time*/) {}

            // The request deadline set by the route of the request (see route::request_timeout),
            // from the start of the request, zero for none (see
            // server_connection::set_request_deadline()); nothing without a connection
            virtual void set_request_deadline(std::chrono::milliseconds /*deadline*/) {}

            // The server connection, for the sink writing the response (none in memory)
            virtual std::weak_ptr<server_connection> connection() const { return {}; }

            // The io_context of the connection, for the sockets a reverse proxy opens to its
            // target (none in memory, or once the connection is gone)
            virtual boost::asio::io_context* io_context() const { return nullptr; }
        };

    }

    /**
     * A HTTP stream represents a communication channel inside a single HTTP connection. As a
     * single connection can be used for multiple HTTP request, each request will generate a new
     * HTTP stream. In HTTP 1.1, all requests in a connection must be answered in order, even if
     * the responses are generated in a different order. In HTTP 2.0, it is possible to answer
     * asynchronously to each single stream within a connection using a stream identifier.
     * TODO check HTTP 2.0 support.
     */
    class http_stream : public std::enable_shared_from_this<http_stream> {

    private:
        /**
         * Unique identifier within a http connection, generate for each request
         */
        stream_id stream_id_;

        /**
         * Queue for each HTTP frame composing a response. A response can be composed on several frames
         * i.e., while sending large files. A list, as a deque allocates its storage up front,
         * and most responses are a single frame
         */
        std::queue<std::shared_ptr<http_frame>, std::list<std::shared_ptr<http_frame>>> queue_;

        /**
         * Set when the response takes over the connection: receives the socket, and the
         * bytes read past the request, once the response is written and the read loop
         * stopped at this stream
         */
        takeover_handler takeover_;
        std::string takeover_buffer_;
        bool takes_over_ = false;
        bool takeover_response_written_ = false;
        bool takeover_reader_stopped_ = false;

        /**
         * Whether input of the request may still be unread on the connection: its body, until
         * read to the end, or the rest of a request that could not be parsed. A connection
         * closed after the response then closes gracefully (see server_connection::linger()).
         * Here, where it takes no room
         */
        bool input_pending_ = false;

        /**
         * Once the input is read: whether bytes of what follows the request (a pipelined
         * request) were already read past it. And the event set then, which the connection
         * waits for before it reads the socket itself (see server_connection::watch_peer()):
         * created by its first wait, set then if the input was read already
         */
        bool input_follows_ = false;
        std::optional<asio::async_event> input_read_;

        /**
         * Frames posted to the connection thread and not queued yet (frames are queued in
         * call order: none runs inline while one is posted)
         */
        std::atomic<unsigned> posted_frames_{0};

        /**
         * Whether the response has started: a frame was queued, or a takeover requested
         */
        bool responded_ = false;

        /**
         * Whether the last frame of the response was written (or failed to be)
         */
        bool response_ended_ = false;

        /**
         * Why the connection closes after the response: the first reason given, if any
         * (see disposition())
         */
        std::optional<detail::close_reason> close_reason_;

        /**
         * Told about the response (head, bytes written, end) while a response hook is set
         * (see http_application::on_response)
         */
        std::shared_ptr<response_observer> observer_;

        /**
         * Cancellation scope of the request (see cancel()). Everything but the reason belongs
         * to the executor of the stream: the one of its connection (or of its dispatch)
         */
        boost::asio::any_io_executor executor_;
        // none until cancelled; read from any thread (a handler may change executor)
        std::atomic<cancel_reason> cancelled_;
        // Set once cancelled, for request::client_gone(): created by its first wait, set then if
        // the scope was cancelled already
        std::optional<asio::async_event> cancel_event_;
        // Bound to the coroutine running the handler (see handler_slot()), unless that
        // coroutine is bound to a signal of its own, its owner's (see bind_handler())
        boost::asio::cancellation_signal handler_signal_;
        boost::asio::cancellation_signal* bound_signal_ = nullptr;
        // A coroutine about to run the handler is bound to the scope (see bind_handler())
        bool handler_bound_ = false;
        // The slot request::cancellation_slot() gives, for the awaits the handler binds to it
        boost::asio::cancellation_signal observer_signal_;
        // The reasons interrupting the handler, as bits 1 << reason (see route::cancel_on)
        uint8_t cancel_on_ = 0;
        // The handler is running: neither signal is emitted before or after it
        bool handler_running_ = false;
        // The handler signal was emitted
        bool handler_interrupted_ = false;
        // Emit it, on the executor of the stream
        void interrupt_handler();

    public:
        /**
         * 100 Continue of a request whose client waits for it before sending the body
         * (Expect: 100-continue): none expected, pending until the body is first read, sent,
         * or skipped because the response started first (the body may never come)
         */
        enum class continue_state : uint8_t { none, pending, sent, skipped };

    private:
        continue_state continue_ = continue_state::none;

    public:
        /**
         * Stream of `request` on `executor` (that of its connection), whose own Connection
         * header and HTTP version seed what happens to the connection after its response
         */
        http_stream(stream_id stream_id, const http_request& request, boost::asio::any_io_executor executor);

        /**
         * Stream whose response closes the connection, for `reason` (e.g. a request that could
         * not be parsed)
         */
        http_stream(stream_id stream_id, detail::close_reason reason, boost::asio::any_io_executor executor);

        virtual ~http_stream() {}

    public:

        size_t get_queue_size() const;

        bool empty_queue() const;

        std::shared_ptr<http_frame> current_frame() const;

        void pop_frame();

        void add_frame(std::shared_ptr<http_frame> frame);

        /**
         * Queue an interim response (100 Continue): the response has not started yet
         */
        void add_interim_frame(std::shared_ptr<http_frame> frame);

        continue_state get_continue() const {
            return continue_;
        }

        void set_continue(continue_state state) {
            continue_ = state;
        }

        bool input_pending() const {
            return input_pending_;
        }

        /**
         * Whether input of the request may still be unread on the connection. Once it is not
         * (read to its end, or none past the head), `follows` tells whether bytes of what
         * follows the request were read past it, and the event wait_input_read() waits for is
         * set. Where the body is read: on the executor of the stream
         */
        void set_input_pending(bool pending, bool follows = false) {
            input_pending_ = pending;
            input_follows_ = follows;
            if (!pending && input_read_) input_read_->set();
        }

        bool input_follows() const {
            return input_follows_;
        }

        /**
         * The request read its input to its end (see set_input_pending()), from any thread (a
         * handler may read its body from another executor): on the executor of the stream,
         * right away if called there
         */
        void end_input(bool follows);

        /**
         * Wait until input of the request is no longer pending: false if the wait is cancelled
         */
        awaitable<bool> wait_input_read() {
            if (!input_read_) {
                input_read_.emplace(executor_);
                if (!input_pending_) input_read_->set();
            }
            return input_read_->wait();
        }

        size_t get_queued_frames() const;

        bool responded() const {
            return responded_;
        }

        void set_response_ended() {
            response_ended_ = true;
        }

        bool response_ended() const {
            return response_ended_;
        }

        /**
         * Take over the connection once the response of this stream is written: the
         * disposition is take_over from now on
         */
        void set_takeover(takeover_handler handler) {
            takeover_ = std::move(handler);
            takes_over_ = true;
            responded_ = true;
        }

        bool takes_over() const {
            return takes_over_;
        }

        /**
         * What the takeover waits for, in either order: the response written, and the read
         * loop stopped (with the bytes it read past the request)
         */
        void takeover_response_written() {
            takeover_response_written_ = true;
        }

        void takeover_reader_stopped(std::string buffered) {
            takeover_buffer_ = std::move(buffered);
            takeover_reader_stopped_ = true;
        }

        /**
         * Handler of the takeover once both are done, and the bytes read past the request;
         * the handler is empty before, and after it was released once
         */
        std::pair<takeover_handler, std::string> release_takeover() {
            if (!takeover_response_written_ || !takeover_reader_stopped_) return {};
            return {std::exchange(takeover_, nullptr), std::move(takeover_buffer_)};
        }

        void frame_posted() {
            ++posted_frames_;
        }

        void posted_frame_queued() {
            --posted_frames_;
        }

        bool has_posted_frames() const {
            return posted_frames_ > 0;
        }

        stream_id id() const;

        /**
         * What happens to the connection once the response is written: taken over if a
         * takeover was set, else closed if a reason was given, else kept alive. It never goes
         * back to keep_alive, and a takeover stays one
         */
        detail::after_response disposition() const {
            if (takes_over_) return detail::after_response::take_over;
            return close_reason_ ? detail::after_response::close : detail::after_response::keep_alive;
        }

        /**
         * Close the connection after the response, for `reason`; the first reason given stays.
         * A takeover still takes over: the reason only makes its head say Connection: close
         */
        void must_close(detail::close_reason reason) {
            if (!close_reason_) close_reason_ = reason;
        }

        /**
         * Why the connection closes after the response, if it does (also under a takeover)
         */
        std::optional<detail::close_reason> reason() const {
            return close_reason_;
        }

        /**
         * Observer of the response, set before the response is answered (none without a
         * response hook)
         */
        void set_observer(std::shared_ptr<response_observer> observer) {
            observer_ = std::move(observer);
        }

        response_observer* observer() const {
            return observer_.get();
        }

        // --- Cancellation scope (see route::cancel_on, request::cancelled()) ---

        /**
         * Cancel the scope for `reason`, from any thread, once: the first reason stays. On the
         * executor of the stream, it then sets the event client_gone() waits for and, while the
         * handler runs, emits terminal on the observer signal, and on the handler signal if the
         * route declared the reason or it is shutdown. Later calls do nothing, except a shutdown
         * while the handler runs not interrupted yet: it emits both signals, and neither
         * cancelled() nor wait_cancelled() see it. For a stream owned by a shared_ptr (it stays
         * alive until then)
         */
        void cancel(cancel_reason reason);

        /**
         * Why the scope was cancelled: none until then
         */
        cancel_reason cancelled() const {
            return cancelled_.load(std::memory_order_acquire);
        }

        /**
         * Wait, on the executor of the stream, until the scope is cancelled: its reason, or
         * none if the wait itself is cancelled
         */
        awaitable<cancel_reason> wait_cancelled();

        /**
         * The reasons interrupting the handler, as bits 1 << reason (see route::get_cancel_on);
         * set before the handler runs
         */
        void set_cancel_on(uint8_t reasons) {
            cancel_on_ = reasons;
        }

        /**
         * Slot of the handler signal, bound once to the coroutine running the handler (see
         * http_application::serve), and that of the observer signal, for request::cancellation_slot()
         */
        boost::asio::cancellation_slot handler_slot() {
            return handler_signal_.slot();
        }

        /**
         * The coroutine about to run the handler is bound to the scope: through handler_slot(),
         * or to `signal`, which its owner emits only for this stream while the handler runs
         * (that of the read loop of the connection, which runs every handler of its requests).
         * The handler then runs on that coroutine; until a coroutine is bound,
         * http_application::serve runs it on one of its own, bound to handler_slot()
         */
        void bind_handler(boost::asio::cancellation_signal* signal = nullptr) {
            handler_bound_ = true;
            bound_signal_ = signal;
        }

        bool handler_bound() const {
            return handler_bound_;
        }

        boost::asio::cancellation_slot observer_slot() {
            return observer_signal_.slot();
        }

        /**
         * The handler starts and ends (on the executor of the stream): the signals are emitted
         * only in between, so a cancellation after its end is a no-op. Their slots are left as
         * they are: what they hold (the cancellation state of the coroutine that ran the
         * handler) goes with the stream
         */
        void handler_started() {
            handler_running_ = true;
        }

        void handler_ended() {
            handler_running_ = false;
        }

        bool handler_running() const {
            return handler_running_;
        }

        /**
         * Whether the handler signal was emitted: the handler was interrupted
         */
        bool handler_interrupted() const {
            return handler_interrupted_;
        }

        /**
         * The connection is done with the stream (its response ended, or the connection is
         * gone): what it kept for the response goes now, not with a request that may keep the
         * stream longer (the observer, or a takeover handler, may hold that request)
         */
        void detach() {
            observer_.reset();
            takeover_ = nullptr;
            queue_ = {};
        }
    };

}

#endif
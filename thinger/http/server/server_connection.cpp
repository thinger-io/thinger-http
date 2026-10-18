#include "server_connection.hpp"
#include "request.hpp"
#include "response_observer.hpp"
#include "../../util/logger.hpp"

namespace thinger::http {

namespace {

    // Interim response asking the client to send the body (RFC 9110 section 15.2.1)
    class continue_frame : public http_frame {
    public:
        continue_frame() { set_last_frame(false); }

        size_t get_size() override { return 0; }

        void to_buffer(std::vector<boost::asio::const_buffer>& buffer) const override {
            static constexpr std::string_view head = "HTTP/1.1 100 Continue\r\n\r\n";
            buffer.emplace_back(boost::asio::buffer(head.data(), head.size()));
        }

        void log(const char* scope, const body_logging&) const override {
            LOG_DEBUG("[{}] HTTP/1.1 100 Continue", scope);
        }
    };

}

server_connection::server_connection(std::shared_ptr<asio::socket> socket)
    : socket_(std::move(socket))
    , timeout_timer_(socket_->get_io_context())
    , response_started_(socket_->get_io_context()) {
    LOG_DEBUG("created http server connection");
}

server_connection::~server_connection() {
    LOG_DEBUG("releasing http server connection");
}

void server_connection::track(const std::shared_ptr<connection_tracker>& tracker) {
    tracked_connection::track(tracker, weak_from_this(), socket_->get_io_context(), connection_type::http);
}

void server_connection::start(std::chrono::seconds timeout) {
    if (running_) return;
    // Accepted while the server shuts down (e.g. it was in a TLS handshake): nothing is
    // waiting for it yet
    if (tracked() && tracker()->current_phase() != connection_tracker::phase::serving) {
        untrack();
        socket_->close();
        return;
    }
    running_ = true;
    timeout_ = timeout;

    reset_timeout();

    co_spawn(socket_->get_io_context(),
        [self = shared_from_this()]() -> awaitable<void> {
            co_await self->read_loop();
        },
        detached);
}

void server_connection::reset_timeout() {
    deadline_ = std::chrono::steady_clock::now() + timeout_;
    // A running timer checks the new deadline when it expires, unless it is earlier (the
    // timeout was shortened)
    if (!timeout_armed_ || std::max(deadline_, hold_) < timeout_timer_.expiry()) {
        arm_timeout();
    }
}

void server_connection::arm_timeout() {
    timeout_armed_ = true;
    timeout_timer_.expires_at(std::max(deadline_, hold_));
    // The timer does not keep the connection alive: once neither the read loop nor a write
    // holds it (e.g. the read loop stopped with a response left unfinished), it is released
    // and the socket closed
    timeout_timer_.async_wait([weak = weak_from_this()](const boost::system::error_code& ec) {
        if (ec) return; // Timer was cancelled or re-armed
        auto self = weak.lock();
        if (!self || !self->timeout_armed_) return; // Expired as it was cancelled

        // Some activity moved the deadline meanwhile, or a request holds the connection
        auto now = std::chrono::steady_clock::now();
        if (now < self->deadline_ || now < self->hold_) {
            self->arm_timeout();
            return;
        }

        self->timeout_armed_ = false;
        LOG_DEBUG("http server connection timed out after {} seconds", self->timeout_.count());
        self->close();
    });
}

void server_connection::cancel_timeout() {
    timeout_armed_ = false;
    timeout_timer_.cancel();
}

void server_connection::close() {
    running_ = false;
    cancel_timeout();
    response_started_.cancel();
    socket_->close();
    // Nothing left to drain, even if the connection is still referenced
    untrack();
}

awaitable<void> server_connection::read_loop() {
    auto self = shared_from_this();

    // Headers only: the handler layer reads the body
    request_parser_.set_headers_only(true);

    size_t buffered = 0; // bytes of valid data in buffer_

    while (running_ && socket_->is_open()) {
        if (buffered == 0) {
            in_progress_ = nullptr;
            auto [ec, bytes] = co_await socket_->read_some(buffer_, MAX_BUFFER_SIZE);
            if (ec) break;
            reset_timeout();
            buffered = bytes;
        }

        uint8_t* begin = buffer_;
        uint8_t* end = buffer_ + buffered;
        boost::tribool result = request_parser_.parse(begin, end);
        if (boost::indeterminate(result)) {
            // Every byte went to the parser: more are needed
            buffered = 0;
            continue;
        }
        if (!result) {
            reject_request();
            break;
        }

        auto [stream, req] = start_request(self, begin, static_cast<size_t>(end - begin));

        // The handler decides how to read the body
        if (handler_) {
            co_await handler_(req);
            // Not once the response closed the connection (or it was taken over): the
            // timer would be armed just to be cancelled
            if (running_) reset_timeout();
        }

        // A response sent later, from a copy of the response, may still take over the
        // connection: wait for it to start before reading what follows this request
        if (!stream->responded()) co_await wait_response(*stream);

        // Read-ahead bytes the request did not consume belong to whatever follows it (they
        // may come from later socket reads, e.g. after a chunked body)
        if (stream->takes_over()) {
            hand_over_reader(*stream, *req);
            co_return;
        }

        // Without keep-alive, reading stops after this request. The leftover stays with the
        // request: its handler may still be reading the body (e.g. the request was answered
        // by its timeout)
        if (!stream->keep_alive() || !keep_pipelined(*req, buffered)) break;
    }

    stop_reading();
}

void server_connection::stop_reading() {
    running_ = false;
    reader_stopped_ = true;
    in_progress_ = nullptr;
    cancel_timeout();

    // Draining, with every response written: done now, even if something still holds the
    // connection (e.g. a copy of the request)
    if ((frame_watch_ & watch_draining) && request_queue_.empty()) close();
}

std::pair<std::shared_ptr<http_stream>, std::shared_ptr<request>> server_connection::start_request(
    const std::shared_ptr<server_connection>& self, const uint8_t* read_ahead, size_t read_ahead_size) {
    auto http_req = request_parser_.consume_request();
    http_req->set_ssl(socket_->is_secure());

    auto stream = std::make_shared<http_stream>(++request_id_, http_req->keep_alive());
    in_progress_ = stream.get();

    // The client may wait for 100 Continue before sending the body: sent once the body is
    // read, unless the client already started sending it
    if (http_req->get_expectation() == http_request::expectation::continue_100 &&
        http_req->has_pending_body() && read_ahead_size == 0) [[unlikely]] {
        stream->set_continue(http_stream::continue_state::pending);
        frame_watch_ |= watch_continue;
    }

    // Queued in order, for pipelining
    request_queue_.push(stream);

    http_req->log("SERVER REQUEST", body_logging_);

    auto req = std::make_shared<request>(self, stream, std::move(http_req));
    if (read_ahead_size > 0) {
        req->set_read_ahead(read_ahead, read_ahead_size);
    }
    return {std::move(stream), std::move(req)};
}

void server_connection::reject_request() {
    // Bad request, or a header section over the limits
    auto status = http_response::status::bad_request;
    if (request_parser_.header_section_too_large()) {
        LOG_ERROR("http request header section too large");
        status = http_response::status::request_header_fields_too_large;
    } else {
        LOG_ERROR("invalid http request");
    }
    auto stream = std::make_shared<http_stream>(++request_id_, false);
    request_queue_.push(stream);
    handle_stock_error(stream, status);
}

void server_connection::hand_over_reader(http_stream& stream, request& req) {
    auto leftover = req.take_read_ahead();
    stream.takeover_reader_stopped(std::string(leftover.begin(), leftover.end()));
    in_progress_ = nullptr;
    complete_takeover(stream);
}

bool server_connection::keep_pipelined(request& req, size_t& buffered) {
    // The leftover is at most what was read at once: the connection buffer, or the body
    // reader framing reads
    auto leftover = req.take_read_ahead();
    static_assert(body_reader::framing_read_size <= MAX_BUFFER_SIZE,
                  "body framing reads must fit in the connection buffer, or the leftover is dropped");
    if (leftover.size() > MAX_BUFFER_SIZE) {
        LOG_ERROR("pipelined data exceeds the connection buffer");
        return false;
    }
    if (!leftover.empty()) {
        std::memcpy(buffer_, leftover.data(), leftover.size());
    }
    buffered = leftover.size();
    return true;
}

awaitable<void> server_connection::wait_response(const http_stream& stream) {
    // Watch the client meanwhile, so a client leaving (closing or half-closing the
    // connection, or resetting it) before a late response is noticed now and not at the
    // connection timeout. Nothing the client sends is lost: it stays readable from the
    // socket for the next request or a takeover
    bool watch_peer = true;
    frame_watch_ |= watch_response_start;
    while (!stream.responded() && running_ && socket_->is_open()) {
        response_started_.expires_at(boost::asio::steady_timer::time_point::max());
        if (!watch_peer) {
            co_await response_started_.async_wait(use_nothrow_awaitable);
            continue;
        }
        auto result = co_await (response_started_.async_wait(use_nothrow_awaitable) || wait_peer_closed());
        if (result.index() == 0) continue;

        if (std::get<1>(result) == peer_state::closed) {
            LOG_DEBUG("http client left while waiting for a response");
            frame_watch_ &= ~watch_response_start;
            close();
            co_return;
        }

        // A pipelined request (or data for a takeover) is waiting: leave it for later. Or the
        // socket cannot be watched: the response is waited for alone
        watch_peer = false;
    }
    frame_watch_ &= ~watch_response_start;
}

awaitable<server_connection::peer_state> server_connection::wait_peer_closed() {
    if (auto ec = co_await socket_->wait(boost::asio::socket_base::wait_read)) {
        co_return ec == boost::asio::error::operation_not_supported ? peer_state::not_watchable : peer_state::closed;
    }
    co_return co_await socket_->peer_closed() ? peer_state::closed : peer_state::data;
}

void server_connection::observe_frame(http_stream& stream, http_frame& frame) {
    // The head of the response (an interim response is not one)
    if (auto* head = dynamic_cast<http_response*>(&frame)) {
        stream.observer()->head(head->get_status(), !frame.end_stream() && !stream.takes_over());
    }
}

void server_connection::frame_written(http_stream& stream, const std::shared_ptr<http_frame>& frame, bool written) {
    // Whoever waits for the frame (a streamed response) may write the next one
    frame->notify_written(written);

    // The response ended (or cannot be written anymore), for the response hook
    if (auto* observer = stream.observer(); observer && (frame->end_stream() || !written)) [[unlikely]] {
        observer->ended(!written ? response_outcome::aborted
                                 : stream.takes_over() ? response_outcome::taken_over : response_outcome::completed);
    }

    // Reset timeout on activity. The timer holds a weak reference: once the read loop stopped
    // (the connection closes after this response), only a pending write keeps the connection,
    // so a response still unfinished when its queued frames are written (e.g. chunks written
    // from detached code after the handler returned) loses the connection then
    reset_timeout();

    if (frame->end_stream()) {
        // Response of a request taking over the connection: the socket may now belong to
        // someone else, never close it from here
        if (stream.takes_over()) {
            stream.takeover_response_written();
            complete_takeover(stream);
            return;
        }

        if (!stream.keep_alive()) {
            close();
        } else {
            if (!request_queue_.empty()) {
                request_queue_.pop();
            }
        }
    }
}

void server_connection::process_output_queue() {
    while (!writing_) {
        if (request_queue_.empty()) return;
        auto stream = request_queue_.front();
        if (stream->empty_queue()) return;

        auto frame = stream->current_frame();
        stream->pop_frame();

        frame->log("SERVER RESPONSE", body_logging_);
        auto* observer = stream->observer();
        if (observer) [[unlikely]] observe_frame(*stream, *frame);

        auto& buffers = write_buffers_;
        buffers.clear();
        frame->fill_buffer(buffers);

        // Most frames fit in the socket send buffer: write them right away, without waiting.
        // A write error is left to the read loop, as for a write that waited
        boost::system::error_code ec;
        size_t written = socket_->write_now(buffers, ec);
        if (observer) [[unlikely]] observer->add_bytes(written);
        if (written == boost::asio::buffer_size(buffers)) {
            frame_written(*stream, frame, true);
            continue;
        }
        if (ec && ec != boost::asio::error::would_block && ec != boost::asio::error::operation_not_supported) {
            frame_written(*stream, frame, false);
            continue;
        }

        // Write the rest once the socket accepts it
        auto remaining = boost::asio::buffer_sequence_begin(buffers);
        while (written > 0 && written >= remaining->size()) {
            written -= remaining->size();
            ++remaining;
        }
        if (written > 0) *remaining += written;
        buffers.erase(buffers.begin(), remaining);

        writing_ = true;
        co_spawn(socket_->get_io_context(),
            [this, self = shared_from_this(), stream = std::move(stream), frame = std::move(frame),
             buffers = std::move(buffers)]() -> awaitable<void> {
                auto [ec, bytes] = co_await socket_->write(buffers);
                if (auto* observer = stream->observer()) [[unlikely]] observer->add_bytes(bytes);
                frame_written(*stream, frame, !ec);
                writing_ = false;

                process_output_queue();
            },
            detached);
    }
}

void server_connection::queue_frame(const std::shared_ptr<http_stream>& stream, std::shared_ptr<http_frame> frame) {
    if (frame_watch_) [[unlikely]] watch_frame(stream, frame);
    stream->add_frame(std::move(frame));

    if (request_queue_.empty()) {
        LOG_ERROR("trying to send response without a pending request!");
        return;
    }

    // Only the front stream writes: responses go in the order of their requests
    if (request_queue_.front()->id() == stream->id()) {
        process_output_queue();
    }
}

void server_connection::watch_frame(const std::shared_ptr<http_stream>& stream,
                                    const std::shared_ptr<http_frame>& frame) {
    // The waiter wakes up once this call returns, with the frame queued
    if (frame_watch_ & watch_response_start) response_started_.cancel();

    // A response starting while the connection drains is the last one: it tells the client
    if ((frame_watch_ & watch_draining) && !stream->responded() && !stream->takes_over()) {
        stream->set_keep_alive(false);
        if (auto* head = dynamic_cast<headers*>(frame.get())) head->set_keep_alive(false);
    }

    // A response starting before the 100 Continue its client waits for: the client may send
    // the body or not, so where the next request starts is unknown. Not sent anymore, and the
    // connection is closed after the response, which tells the client (as Go and Node do)
    if ((frame_watch_ & watch_continue) && stream->get_continue() == http_stream::continue_state::pending) {
        frame_watch_ &= ~watch_continue;
        stream->set_continue(http_stream::continue_state::skipped);
        if (!stream->takes_over()) {
            stream->set_keep_alive(false);
            if (auto* head = dynamic_cast<headers*>(frame.get())) head->set_keep_alive(false);
        }
    }
}

void server_connection::send_continue(http_stream& stream) {
    if (stream.get_continue() != http_stream::continue_state::pending || !running_) return;
    stream.set_continue(http_stream::continue_state::sent);
    frame_watch_ &= ~watch_continue;
    // Queued as the first frame of the stream, before its response: written once the
    // responses of the previous requests are
    stream.add_interim_frame(std::make_shared<continue_frame>());
    if (!request_queue_.empty() && request_queue_.front().get() == &stream) {
        process_output_queue();
    }
}

void server_connection::shutdown_drain() {
    if (!tracked() || (frame_watch_ & watch_draining)) return;
    frame_watch_ |= watch_draining;
    if (!running_) return;

    // Handling a request: the connection is closed after its response (whose head tells the
    // client unless it already started), and the read loop stops after it
    if (in_progress_) {
        in_progress_->set_keep_alive(false);
        return;
    }

    // Waiting for a request with nothing to write: idle
    if (request_queue_.empty() && request_parser_.idle()) {
        LOG_DEBUG("closing an idle http server connection: the server shuts down");
        close();
        return;
    }

    // A response still being written (the read loop went on to the next request): the
    // connection is closed after it. Or part of a request arrived: its response closes it
    if (!request_queue_.empty()) request_queue_.back()->set_keep_alive(false);
}

void server_connection::shutdown_close() {
    if (tracked()) close();
}

void server_connection::handle_stream(std::shared_ptr<http_stream> stream,
                                       std::shared_ptr<http_frame> frame) {
    // Frames are queued in the order of the calls, from whatever threads they come (e.g.
    // chunks written from the IO thread after headers sent from another one): inline only
    // on the connection thread and with no frame of the stream still posted
    auto& io_context = socket_->get_io_context();
    if (io_context.get_executor().running_in_this_thread() && !stream->has_posted_frames()) {
        queue_frame(stream, std::move(frame));
        return;
    }
    stream->frame_posted();
    boost::asio::post(io_context,
        [this, self = shared_from_this(), stream = std::move(stream), frame = std::move(frame)]() mutable {
            stream->posted_frame_queued();
            queue_frame(stream, std::move(frame));
        });
}

void server_connection::take_over(std::shared_ptr<http_stream> stream, std::shared_ptr<http_frame> response,
                                  takeover_handler handler) {
    boost::asio::dispatch(socket_->get_io_context(),
        [this, self = shared_from_this(), stream = std::move(stream), response = std::move(response),
         handler = std::move(handler)]() mutable {
            // The read loop already stopped (the client left, the connection timed out...):
            // the connection is gone and cannot be handed over
            if (reader_stopped_) {
                LOG_WARNING("connection closed before it could be taken over: the takeover handler is not called");
                return;
            }
            stream->set_takeover(std::move(handler));
            queue_frame(stream, std::move(response));
        });
}

void server_connection::handle_stock_error(std::shared_ptr<http_stream> stream,
                                            http_response::status status) {
    auto http_error = http_response::stock_http_reply(status);
    http_error->set_keep_alive(stream->keep_alive());
    handle_stream(stream, http_error);
}

std::shared_ptr<asio::socket> server_connection::release_socket() {
    // The socket belongs to someone else from now on: the server neither drains nor closes it
    untrack();
    running_ = false;
    socket_->cancel();
    cancel_timeout();
    return socket_;
}

void server_connection::complete_takeover(http_stream& stream) {
    // Both the response must be written and the read loop stopped, in either order
    auto [handler, buffered] = stream.release_takeover();
    if (!handler) return;

    // From now on the connection counts as taken over, while its socket lives. Tracked before
    // this one leaves, so a shutdown never sees the server without connections meanwhile
    std::shared_ptr<taken_over_connection> taken_over;
    if (tracked()) {
        if (tracker()->current_phase() == connection_tracker::phase::closing) {
            LOG_WARNING("connection closed before it could be taken over: the server shuts down");
            close();
            return;
        }
        taken_over = taken_over_connection::track(tracker(), socket_);
    }
    auto socket = release_socket();
    handler(std::move(socket), std::move(buffered));
    // Told now if the server started shutting down meanwhile (the handler may have set what
    // to do then)
    if (taken_over) taken_over->handed_over();
}

void server_connection::release() {
    boost::asio::dispatch(socket_->get_io_context(),
        [this, self = shared_from_this()] {
            close();
        });
}

void server_connection::extend_timeout() {
    if (running_) reset_timeout();
}

void server_connection::hold_until(std::chrono::steady_clock::time_point time) {
    // A running timer checks the hold when it expires
    hold_ = std::max(hold_, time);
}

std::shared_ptr<asio::socket> server_connection::get_socket() {
    return socket_;
}

void server_connection::update_connection_timeout(std::chrono::seconds timeout) {
    boost::asio::dispatch(socket_->get_io_context(),
        [this, self = shared_from_this(), timeout] {
            timeout_ = timeout;
            reset_timeout();
        });
}

}

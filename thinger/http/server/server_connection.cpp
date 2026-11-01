#include "server_connection.hpp"
#include "server_base.hpp"
#include "request.hpp"
#include "response_observer.hpp"
#include "../../asio/sockets/tcp_socket.hpp"
#include "../../asio/sockets/unix_socket.hpp"
#include "../../util/logger.hpp"
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/defer.hpp>
#include <boost/asio/post.hpp>

namespace thinger::http {

namespace {

    // The Connection header of a response head, written as the head is serialized: whatever
    // decided the connection closes after the response, the head says so. A protocol switch
    // keeps the Connection header set for it (Upgrade)
    void announce_disposition(const http_stream& stream, http_response& head) {
        if (head.get_status() == http_response::status::switching_protocols) return;
        head.set_keep_alive(!stream.reason());
    }

    // Lingering close once the server shuts down: brief, not to hold it
    constexpr std::chrono::milliseconds shutdown_linger_timeout{500};

    // Stop sending on the connection, reading on: TCP (TLS included) and Unix sockets
    bool shutdown_send(asio::socket& socket) {
        if (auto* tcp = dynamic_cast<asio::tcp_socket*>(&socket)) return tcp->shutdown_send();
        if (auto* local = dynamic_cast<asio::unix_socket*>(&socket)) return local->shutdown_send();
        return false;
    }

    // Read what arrives to drop it: from the TCP stream itself under TLS, whose records are
    // not decrypted (nor read by the TLS stream, whose reads were cancelled)
    awaitable<io_result> read_to_drop(asio::socket& socket, uint8_t* buffer, size_t size) {
        if (auto* tcp = dynamic_cast<asio::tcp_socket*>(&socket)) {
            co_return co_await tcp->get_socket().async_read_some(boost::asio::buffer(buffer, size),
                                                                 use_nothrow_awaitable);
        }
        co_return co_await socket.read_some(buffer, size);
    }

}

server_connection::server_connection(std::shared_ptr<asio::socket> socket,
                                     std::shared_ptr<const connection_options> options)
    : socket_(std::move(socket))
    // A member, called only while this lives. It does not keep the connection alive: once
    // neither the read loop nor a write holds it (e.g. the read loop stopped with a response
    // left unfinished), it is released and the socket closed
    , timeout_timer_(socket_->get_io_context().get_executor(), [this] {
        if (auto self = weak_from_this().lock()) self->deadline_expired();
    })
    , options_(options ? std::move(options) : std::make_shared<const connection_options>()) {
    timeout_ = options_->idle_timeout;
    LOG_DEBUG("created http server connection");
}

server_connection::~server_connection() {
    LOG_DEBUG("releasing http server connection");
    // The requests may outlive the connection, with their streams
    for (; !request_queue_.empty(); request_queue_.pop_front()) request_queue_.front()->detach();
}

void server_connection::track(const std::shared_ptr<connection_tracker>& tracker) {
    tracked_connection::track(tracker, weak_from_this(), socket_->get_io_context(), connection_type::http);
}

void server_connection::start() {
    if (running_) return;
    // Accepted while the server shuts down (e.g. it was in a TLS handshake): nothing is
    // waiting for it yet
    if (auto tracker = this->tracker(); tracker && tracker->current_phase() != connection_tracker::phase::serving) {
        untrack();
        socket_->close();
        return;
    }
    running_ = true;

    // Its first request starts now: a client that sends nothing is bounded by its header deadline
    last_activity_ = request_start_ = clock::now();
    reading_head_ = true;
    arm_deadline();

    // The read loop runs the handlers of the requests: bound to the signal their scopes emit
    // (see start_request())
    co_spawn(socket_->get_io_context(),
        [self = shared_from_this()]() -> awaitable<void> {
            co_await self->read_loop();
        },
        boost::asio::bind_cancellation_slot(handler_signal_.slot(), detached));
    // The peer watcher starts once a handler yields first (see wake_watcher())
}

void server_connection::close() {
    running_ = false;
    timeout_timer_.cancel();
    if (reader_wake_) reader_wake_->set();
    stop_watcher();
    socket_->close();
    // Nothing left to drain, even if the connection is still referenced
    untrack();
}

void server_connection::close_after(const http_stream& stream) {
    if (stream.input_pending()) {
        linger();
    } else {
        close();
    }
}

void server_connection::linger() {
    if (lingering_) return;
    size_t max_bytes = options_->linger_max_bytes;
    std::chrono::milliseconds timeout = options_->linger_timeout;
    // The server shuts down: briefly, so it does not hold the shutdown
    if (frame_watch_ & watch_draining) timeout = std::min(timeout, shutdown_linger_timeout);
    if (max_bytes == 0 || timeout.count() <= 0 || !socket_->is_open() || !shutdown_send(*socket_)) {
        close();
        return;
    }
    // The connection stops serving: no more requests, nor its timeout. Whatever reads it ends
    // now (the read loop, or a handler still reading the body, which reads nothing more), so
    // only the graceful close reads it from now on
    lingering_ = true;
    running_ = false;
    timeout_timer_.cancel();
    if (reader_wake_) reader_wake_->set();
    stop_watcher();
    socket_->cancel();
    co_spawn(socket_->get_io_context(),
        [self = shared_from_this(), max_bytes, timeout]() -> awaitable<void> {
            co_await self->drop_input(max_bytes, timeout);
        },
        detached);
}

awaitable<void> server_connection::drop_input(size_t max_bytes, std::chrono::milliseconds timeout) {
    // Into the connection buffer, which the read loop no longer uses: the memory of a
    // connection does not grow while it drops input
    size_t dropped = 0;
    auto drop = [&]() -> awaitable<void> {
        while (dropped < max_bytes) {
            auto [ec, bytes] = co_await read_to_drop(*socket_, buffer_, std::min(MAX_BUFFER_SIZE, max_bytes - dropped));
            if (ec) co_return;  // the client closed (or reset) the connection
            dropped += bytes;
        }
    };
    boost::asio::steady_timer timer(socket_->get_io_context(), timeout);
    auto result = co_await (timer.async_wait(use_nothrow_awaitable) || drop());
    LOG_DEBUG("http server connection closed gracefully: {} bytes dropped{}", dropped,
              result.index() == 0 ? ", until the timeout" : dropped >= max_bytes ? ", up to the limit" : "");
    close();
}

awaitable<void> server_connection::read_loop() {
    auto self = shared_from_this();

    // Headers only: the handler layer reads the body
    request_parser_.set_headers_only(true);

    size_t buffered = 0; // bytes of valid data in buffer_

    while (running_ && socket_->is_open()) {
        // A pipelined request, already read with the one before it. The request before it ended
        // (its stream may be gone): none is in progress until this one starts
        bool pipelined = buffered > 0;
        in_progress_ = nullptr;
        // Not while the responses back up (the client does not read them): they would pile up.
        // Then from the top: the loop ends if the connection stopped meanwhile
        if (reading_paused()) [[unlikely]] {
            co_await wait_output();
            continue;
        }
        if (buffered == 0) {
            auto [ec, bytes] = co_await socket_->read_some(buffer_, MAX_BUFFER_SIZE);
            // (or stopped meanwhile: its head was answered at its deadline)
            if (ec || !running_) break;
            extend_timeout();
            buffered = bytes;
        }
        head_started(pipelined);

        uint8_t* begin = buffer_;
        uint8_t* end = buffer_ + buffered;
        boost::tribool result = request_parser_.parse(begin, end);
        if (boost::indeterminate(result)) {
            // Every byte went to the parser: more are needed, within the header deadline
            buffered = 0;
            head_incomplete();
            continue;
        }
        reading_head_ = false;
        if (!result) {
            reject_request();
            break;
        }

        auto [stream, req] = start_request(self, begin, static_cast<size_t>(end - begin));

        // The client is watched while the request is handled, and until its response starts
        watch(stream);

        // The handler decides how to read the body
        if (handler_) {
            co_await handler_(req);
            // Not once the response closed the connection (or it was taken over): the
            // timer would be armed just to be cancelled
            extend_timeout();
        }

        // A response sent later, from a copy of the response, may still take over the
        // connection: wait for it to start before reading what follows this request
        if (!stream->responded()) co_await wait_response(*stream);

        // The read loop reads the socket again, alone: a read of the watcher in progress
        // (over TLS) ends first
        unwatch();
        while (watch_reading_) {
            watch_idle_->reset();
            co_await watch_idle_->wait();
        }

        // Read-ahead bytes the request did not consume belong to whatever follows it (they
        // may come from later socket reads, e.g. after a chunked body)
        if (stream->takes_over()) {
            hand_over_reader(*stream, *req);
            co_return;
        }

        // Without keep-alive, reading stops after this request. The leftover stays with the
        // request: its handler may still be reading the body (e.g. the request was answered
        // by its timeout)
        if (stream->disposition() == detail::after_response::close || !keep_pipelined(*req, buffered)) {
            // The response ended before the connection was to close (e.g. a body left unread
            // turned out too large to drop): close it now, gracefully if the body still comes.
            // A response not ended yet closes it once it ends
            linger_unread(*stream);
            break;
        }
    }

    stop_reading();
}

void server_connection::linger_unread(const http_stream& stream) {
    if (request_queue_.empty() && stream.input_pending()) linger();
}

void server_connection::stop_reading() {
    running_ = false;
    reader_stopped_ = true;
    in_progress_ = nullptr;
    reading_head_ = false;
    // A response still waiting for the socket stays bounded by its write deadline: the client
    // may never read it (see frame_written() for those written afterwards)
    if (write_waits_ && !lingering_ && socket_->is_open()) {
        arm_deadline();
    } else {
        timeout_timer_.cancel();
    }
    stop_watcher();

    // Draining, with every response written: done now, even if something still holds the
    // connection (e.g. a copy of the request). Unless it closes gracefully already
    if ((frame_watch_ & watch_draining) && request_queue_.empty() && !lingering_) close();
}

std::pair<std::shared_ptr<http_stream>, std::shared_ptr<request>> server_connection::start_request(
    const std::shared_ptr<server_connection>& self, const uint8_t* read_ahead, size_t read_ahead_size) {
    auto http_req = request_parser_.consume_request();
    http_req->set_ssl(socket_->is_secure());

    auto stream = std::make_shared<http_stream>(++request_id_, *http_req, socket_->get_io_context().get_executor());
    // Its handler runs on the read loop (which awaits handler_), one request at a time: its
    // scope interrupts it through the signal of the read loop
    stream->bind_handler(&handler_signal_);
    in_progress_ = stream.get();
    // Its body is on the connection until read to its end (where it ends may be unknown),
    // unless it arrived whole with the head, maybe with what follows it
    bool input_pending = !http_req->has_valid_framing() || http_req->is_chunked_transfer() ||
                         read_ahead_size < http_req->pending_body_size();
    stream->set_input_pending(input_pending, !input_pending && read_ahead_size > http_req->pending_body_size());
    // Read within the request deadline, which its route may change (see request_timed_out()); none
    // for a request received with its head
    request_deadline_ = input_pending ? options_->request_timeout : std::chrono::milliseconds{0};
    if (input_pending) arm_deadline();

    // The client may wait for 100 Continue before sending the body: sent once the body is
    // read, unless the client already started sending it
    if (http_req->get_expectation() == http_request::expectation::continue_100 &&
        http_req->has_pending_body() && read_ahead_size == 0) [[unlikely]] {
        stream->set_continue(http_stream::continue_state::pending);
        frame_watch_ |= watch_continue;
    }

    // Queued in order, for pipelining
    request_queue_.push_back(stream);

    http_req->log("SERVER REQUEST", options_->logging);

    auto req = std::make_shared<request>(make_source(*stream), stream, std::move(http_req));
    if (read_ahead_size > 0) {
        req->set_read_ahead(read_ahead, read_ahead_size);
    }
    return {std::move(stream), std::move(req)};
}

void server_connection::reject_request() {
    // Bad request, or a header section over the limits
    auto status = http_response::status::bad_request;
    if (request_parser_.header_section_too_large()) {
        LOG_DEBUG("http request header section too large");
        status = http_response::status::request_header_fields_too_large;
    } else {
        LOG_DEBUG("invalid http request");
    }
    // The rest of a header section too large may still be arriving (big cookies, say), and its
    // client is worth an answer; not that of a request that is not HTTP, as Go does
    answer_and_close(status, detail::close_reason::body_error,
                     status == http_response::status::request_header_fields_too_large);
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
    // The watcher still watches the client meanwhile (see watch_peer()): a client resetting
    // the connection before a late response closes it now, not at the connection timeout
    frame_watch_ |= watch_response_start;
    if (!reader_wake_) reader_wake_.emplace(socket_->get_io_context().get_executor());
    while (!stream.responded() && running_ && socket_->is_open()) {
        reader_wake_->reset();   // woken by the next frame, not by one before
        co_await reader_wake_->wait();
    }
    frame_watch_ &= ~watch_response_start;
}

awaitable<void> server_connection::wait_output() {
    // Within the deadlines of the connection: the write deadline while the front response waits
    // for the socket, the idle one otherwise. Its close (or a stop) wakes it
    output_waited_ = true;
    if (!reader_wake_) reader_wake_.emplace(socket_->get_io_context().get_executor());
    while (output_backed_up() && running_ && socket_->is_open()) {
        reader_wake_->reset();
        co_await reader_wake_->wait();
    }
    output_waited_ = false;
}

void server_connection::watch(const std::shared_ptr<http_stream>& stream) {
    watched_ = stream;
    // The watcher wakes once the handler yields, unless the response ended meanwhile: a
    // handler answering at once costs a deferred call (on this thread, no lock), not a round
    // of the watcher
    boost::asio::defer(socket_->get_io_context(), [self = shared_from_this(), watched = stream.get()] {
        if (self->watched_.get() == watched && !watched->response_ended()) self->wake_watcher();
    });
}

void server_connection::wake_watcher() {
    if (watch_wake_) {
        watch_wake_->set();
        return;
    }
    // Its first need: a connection whose handlers all answer at once never starts it. It runs
    // while the read loop runs (stopped through its slot, see unwatch())
    auto executor = socket_->get_io_context().get_executor();
    watch_wake_.emplace(executor);
    watch_idle_.emplace(executor);
    co_spawn(socket_->get_io_context(),
        [self = shared_from_this()]() -> awaitable<void> {
            co_await self->watch_loop();
        },
        boost::asio::bind_cancellation_slot(watch_stop_.slot(), detached));
}

void server_connection::unwatch() {
    watched_ = nullptr;
    if (watch_waiting_) watch_stop_.emit(boost::asio::cancellation_type::terminal);
}

void server_connection::stop_watcher() {
    unwatch();
    if (watch_wake_) watch_wake_->set();
}

bool server_connection::watching(const http_stream& stream) const {
    // Not a stream taking over the connection: its socket goes to a new owner
    return watched_.get() == &stream && running_ && !lingering_ && socket_->is_open() && !stream.takes_over();
}

awaitable<void> server_connection::watch_loop() {
    // Stopped through its slot (see unwatch()), which ends the wait in progress, and nothing else
    co_await boost::asio::this_coro::throw_if_cancelled(false);
    while (running_) {
        co_await boost::asio::this_coro::reset_cancellation_state();
        if (auto stream = watched_) {
            co_await watch_peer(*stream);
            if (watched_ == stream) watched_ = nullptr;
            continue;
        }
        watch_wake_->reset();
        co_await watch_wake_->wait();
    }
}

awaitable<void> server_connection::watch_peer(http_stream& stream) {
    // (woken after the handler yields, see watch())
    if (!watching(stream) || stream.response_ended()) co_return;

    // The body is read by the request: the socket is read here only once it was read to its
    // end (or dropped), never meanwhile
    if (stream.input_pending()) {
        watch_waiting_ = true;
        bool read = co_await stream.wait_input_read();
        watch_waiting_ = false;
        if (!read || !watching(stream)) co_return;
    }
    // What follows the request was read along with it (a pipelined request): it stays for
    // the next request
    if (stream.input_follows()) co_return;

    watch_waiting_ = true;
    auto ec = co_await socket_->wait(boost::asio::socket_base::wait_read);
    watch_waiting_ = false;
    // Stopped, or a socket that cannot wait (e.g. one defined by an application)
    if (ec || !watching(stream)) co_return;

    watch_reading_ = true;
    ec = co_await socket_->peer_closed();
    watch_reading_ = false;
    watch_idle_->set();

    // Data (the next request, or data for a takeover) stays readable from the socket. A
    // half-close after the request is no leaving (RFC 9112 section 9.6): its response is
    // written, and the connection closed after it. Anything else is the client gone (a
    // reset): unless the connection closed meanwhile
    if (!ec || ec == boost::asio::error::operation_not_supported || ec == boost::asio::error::operation_aborted) {
        co_return;
    }
    if (ec == boost::asio::error::eof) {
        stream.must_close(detail::close_reason::client_asked);
        co_return;
    }
    LOG_DEBUG("http client left while its request was handled: {}", ec.message());
    stream.cancel(cancel_reason::client_gone);
    if (!lingering_) close();
}

void server_connection::observe_frame(http_stream& stream, http_frame& frame) {
    // The head of the response (an interim response is not one): on the server, a response
    if (frame.kind() == frame_kind::head) {
        auto& head = static_cast<http_response&>(frame);
        stream.observer()->head(head.get_status(), !frame.end_stream() && !stream.takes_over());
    }
}

void server_connection::frame_written(http_stream& stream, const std::shared_ptr<http_frame>& frame,
                                      const boost::system::error_code& ec) {
    bool written = !ec;
    // A write the client reset (or whose connection broke) tells that the client is gone
    if (ec && (ec == boost::asio::error::connection_reset || ec == boost::asio::error::broken_pipe)) [[unlikely]] {
        stream.cancel(cancel_reason::client_gone);
    }

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
    last_activity_ = clock::now();
    arm_deadline();

    if (frame->end_stream()) {
        stream.set_response_ended();
        // Response of a request taking over the connection: the socket may now belong to
        // someone else, never close it from here
        if (stream.takes_over()) {
            stream.takeover_response_written();
            complete_takeover(stream);
            return;
        }

        if (stream.disposition() == detail::after_response::close) {
            if (written) {
                close_after(stream);
            } else {
                close();
            }
        } else if (!written) [[unlikely]] {
            // The connection broke (the client reset it, say): closed, so the requests it read
            // ahead (pipelined) are not handled for a client gone, as Go and Node do
            close();
        } else {
            if (!request_queue_.empty()) {
                stream.detach();
                request_queue_.pop_front();
                output_progressed();
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

        // The only writer of the Connection header of a response (see announce_disposition())
        if (frame->kind() == frame_kind::head) announce_disposition(*stream, static_cast<http_response&>(*frame));

        frame->log("SERVER RESPONSE", options_->logging);
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
            frame_written(*stream, frame, {});
            continue;
        }
        if (ec && ec != boost::asio::error::would_block && ec != boost::asio::error::operation_not_supported) {
            frame_written(*stream, frame, ec);
            continue;
        }

        // Write the rest once the socket accepts it, within the write deadline
        asio::socket::consume(buffers, written);
        write_started();
        co_spawn(socket_->get_io_context(),
            [this, self = shared_from_this(), stream = std::move(stream), frame = std::move(frame),
             buffers = std::move(buffers)]() mutable -> awaitable<void> {
                auto [ec, bytes] = write_in_slices(buffers)
                    ? co_await socket_->write_slices(buffers, [this]{ write_progress_ = clock::now(); })
                    : co_await socket_->write(buffers);
                write_waits_ = false;
                if (auto* observer = stream->observer()) [[unlikely]] observer->add_bytes(bytes);
                frame_written(*stream, frame, ec);
                writing_ = false;

                process_output_queue();
                output_progressed();
            },
            detached);
    }
}

void server_connection::queue_frame(const std::shared_ptr<http_stream>& stream, std::shared_ptr<http_frame> frame) {
    if (frame_watch_) [[unlikely]] watch_frame(*stream);
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

void server_connection::watch_frame(http_stream& stream) {
    // The waiter wakes up once this call returns, with the frame queued
    if (frame_watch_ & watch_response_start) reader_wake_->set();

    // A response starting while the connection drains is the last one: its head tells the client
    if ((frame_watch_ & watch_draining) && !stream.responded() && !stream.takes_over()) {
        stream.must_close(detail::close_reason::draining);
    }

    // A response starting before the 100 Continue its client waits for: the client may send
    // the body or not, so where the next request starts is unknown. Not sent anymore, and the
    // connection is closed after the response, which tells the client (as Go and Node do)
    if ((frame_watch_ & watch_continue) && stream.get_continue() == http_stream::continue_state::pending) {
        frame_watch_ &= ~watch_continue;
        stream.set_continue(http_stream::continue_state::skipped);
        if (!stream.takes_over()) stream.must_close(detail::close_reason::continue_skipped);
    }
}

void server_connection::send_continue(http_stream& stream) {
    if (stream.get_continue() != http_stream::continue_state::pending || !running_) return;
    stream.set_continue(http_stream::continue_state::sent);
    frame_watch_ &= ~watch_continue;
    // Queued as the first frame of the stream, before its response: written once the
    // responses of the previous requests are
    stream.add_interim_frame(std::make_shared<detail::continue_frame>());
    if (!request_queue_.empty() && request_queue_.front().get() == &stream) {
        process_output_queue();
    }
}

void server_connection::shutdown_drain() {
    if (!tracked() || (frame_watch_ & watch_draining)) return;
    frame_watch_ |= watch_draining;
    // Closing gracefully: its response was written whole, so it does not hold the shutdown
    if (lingering_) {
        close();
        return;
    }
    if (!running_) return;

    // Handling a request: the connection is closed after its response (whose head tells the
    // client unless it already started), and the read loop stops after it
    if (in_progress_) {
        in_progress_->must_close(detail::close_reason::draining);
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
    if (!request_queue_.empty()) request_queue_.back()->must_close(detail::close_reason::draining);
}

void server_connection::shutdown_cancel() {
    // The handlers still running: that of the request being read, and those of the requests
    // waiting for their responses (cancelled once, see http_stream::cancel())
    if (in_progress_ && in_progress_->handler_running()) in_progress_->cancel(cancel_reason::shutdown);
    for (const auto& stream : request_queue_) {
        if (stream->handler_running()) stream->cancel(cancel_reason::shutdown);
    }
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
    // Its head tells what happens to the connection, as any other (see process_output_queue())
    handle_stream(stream, http_response::stock_http_reply(status));
}

std::shared_ptr<asio::socket> server_connection::release_socket() {
    // The socket belongs to someone else from now on: the server neither drains nor closes it
    untrack();
    running_ = false;
    stop_watcher();
    socket_->cancel();
    timeout_timer_.cancel();
    return socket_;
}

void server_connection::complete_takeover(http_stream& stream) {
    // Both the response must be written and the read loop stopped, in either order
    auto [handler, buffered] = stream.release_takeover();
    if (!handler) return;

    // From now on the connection counts as taken over, while the socket handed out lives.
    // Tracked before this one leaves, so a shutdown never sees the server without connections
    // meanwhile
    std::shared_ptr<detail::taken_over_tracking> taken_over;
    if (auto tracker = this->tracker()) {
        if (tracker->current_phase() == connection_tracker::phase::closing) {
            LOG_WARNING("connection closed before it could be taken over: the server shuts down");
            close();
            return;
        }
        taken_over = detail::taken_over_tracking::track(std::move(tracker), socket_);
    }
    auto socket = release_socket();
    // The handler gets the socket owning the tracking, and its scope (none if not tracked)
    handler(taken_over ? taken_over->hand_out(std::move(buffered))
                       : upgraded_connection{std::move(socket), std::move(buffered), {}});
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
    if (!running_) return;
    last_activity_ = clock::now();
    arm_deadline();
}

void server_connection::hold_until(std::chrono::steady_clock::time_point time) {
    // Of the idle deadline only (see next_deadline()): not of those of the request
    hold_ = std::max(hold_, time);
}

std::shared_ptr<asio::socket> server_connection::get_socket() {
    return socket_;
}

void server_connection::update_connection_timeout(std::chrono::seconds timeout) {
    boost::asio::dispatch(socket_->get_io_context(),
        [this, self = shared_from_this(), timeout] {
            timeout_ = timeout;
            last_activity_ = clock::now();
            arm_deadline();
        });
}

}

#include "server_connection.hpp"
#include "server_base.hpp"
#include "../../asio/sockets/socket.hpp"
#include "../../util/logger.hpp"
#include <algorithm>

// The deadlines of a server connection, on its one watchdog (timeout_timer_): no timer, spawn or
// allocation of their own. The watchdog is armed at the earliest deadline that applies, and on
// expiry the connection works out which one passed: it acts for it, or arms the watchdog again
// for the next one (a head completed, activity moved the idle deadline, or a write went on,
// meanwhile)

namespace thinger::http {

void server_connection::arm_deadline() {
    timeout_timer_.expires_at(next_deadline());
}

server_connection::clock::time_point server_connection::next_deadline() const {
    auto deadline = idle_deadline();
    if (clock::time_point head; head_deadline(head)) deadline = std::min(deadline, head);
    if (clock::time_point body; body_deadline(body)) deadline = std::min(deadline, body);
    return deadline;
}

server_connection::clock::time_point server_connection::idle_deadline() const {
    // A response the client still takes is no idle connection, however long it lasts: only a
    // write without progress for its deadline is. Not floored by the hold, as the request ones
    if (write_waits_) return write_progress_ + options_->write_timeout;
    return std::max(last_activity_ + timeout_, hold_);
}

void server_connection::write_started() {
    writing_ = true;
    if (options_->write_timeout.count() <= 0) return;
    write_waits_ = true;
    write_progress_ = clock::now();
    arm_deadline();
}

bool server_connection::write_in_slices(const std::vector<boost::asio::const_buffer>& buffers) const {
    // Without the deadline, or within one slice, written at once
    return options_->write_timeout.count() > 0 && boost::asio::buffer_size(buffers) > asio::socket::WRITE_SLICE_SIZE;
}

bool server_connection::head_deadline(clock::time_point& deadline) const {
    // (not once the connection stopped reading)
    if (!reading_head_ || !running_) return false;
    // Capped by the request deadline, as Node does; zero disables either
    auto limit = options_->header_timeout;
    auto request = options_->request_timeout;
    if (request.count() > 0 && (limit.count() <= 0 || request < limit)) limit = request;
    if (limit.count() <= 0) return false;
    deadline = request_start_ + limit;
    return true;
}

void server_connection::head_started(bool pipelined) {
    if (reading_head_) return;
    request_start_ = pipelined ? clock::now() : last_activity_;
}

void server_connection::head_incomplete() {
    if (reading_head_) return;
    reading_head_ = true;
    arm_deadline();
}

void server_connection::deadline_expired() {
    auto now = clock::now();
    if (clock::time_point head; head_deadline(head) && now >= head) {
        head_timed_out();
        return;
    }
    if (clock::time_point body; body_deadline(body) && now >= body) {
        request_timed_out(*in_progress_);
        return;
    }
    if (now >= idle_deadline()) {
        // Nor is a request received whole whose handler runs idle, as in Node and Go: looked at
        // again an idle timeout later (the watchdog is never left unarmed, whatever ends the
        // handler). Checked as it expires, so that a request costs nothing more. A body still
        // arriving is bounded by it, as every byte moves it
        if (!write_waits_ && in_progress_ && in_progress_->handler_running() && !in_progress_->input_pending()) {
            timeout_timer_.expires_at(now + std::max(timeout_, std::chrono::seconds{1}));
            return;
        }
        if (write_waits_) {
            LOG_DEBUG("http server connection closed: the client took no byte of its response within its write deadline");
        } else {
            LOG_DEBUG("http server connection timed out after {} seconds", timeout_.count());
        }
        close();
        return;
    }
    // None passed: the earliest that applies now
    arm_deadline();
}

void server_connection::head_timed_out() {
    reading_head_ = false;
    // No byte of a request arrived (only the first request of a connection is timed so): closed
    // without a response, which the client could take for that of a request it just sent, as Go
    // and nginx do. Logged at debug level only, as the other expiries: the client decides how often
    if (request_parser_.idle()) {
        LOG_DEBUG("http server connection closed: no request arrived within its header deadline");
        close();
        return;
    }
    LOG_DEBUG("http request head not complete within its deadline: answered 408");
    // The read loop stops reading (whatever its pending read returns), and the connection closes
    // once the 408 is written, at once: what the client still sends is not waited for
    running_ = false;
    answer_and_close(http_response::status::timed_out, detail::close_reason::request_timeout, false);
}

bool server_connection::body_deadline(clock::time_point& deadline) const {
    // Received once its body was read to its end: told by the body reader, checked here
    if (request_deadline_.count() <= 0 || !in_progress_ || !in_progress_->input_pending() || !running_) return false;
    deadline = request_start_ + request_deadline_;
    return true;
}

void server_connection::set_request_deadline(const http_stream& stream, std::chrono::milliseconds deadline) {
    // Only while its body is read: a request without one was received with its head
    if (in_progress_ != &stream || !stream.input_pending()) return;
    request_deadline_ = deadline;
    // An earlier deadline arms the watchdog again (at once if it passed already)
    arm_deadline();
}

void server_connection::request_timed_out(http_stream& stream) {
    request_deadline_ = std::chrono::milliseconds{0};
    stream.cancel(cancel_reason::timeout);
    // Its response not started, nor another one being written (pipelined): answered 408 here,
    // and the connection closed after it, gracefully as the body may still come (as Node
    // does). Otherwise closed now: a write in progress is never cut short to make room for a
    // 408. Either way, what its handler reads or writes afterwards fails on the closed connection
    bool answerable = !stream.responded() && !stream.has_posted_frames() && !writing_ &&
                      !request_queue_.empty() && request_queue_.front().get() == &stream;
    if (!answerable) {
        LOG_DEBUG("http request not received within its deadline, its response started: closing the connection");
        close();
        return;
    }
    LOG_DEBUG("http request not received within its deadline: answered 408");
    // The body read pending ends (nothing else uses the socket meanwhile)
    socket_->cancel();
    stream.must_close(detail::close_reason::request_timeout);
    handle_stock_error(request_queue_.front(), http_response::status::timed_out);
}

void server_connection::answer_and_close(http_response::status status, detail::close_reason reason,
                                         bool input_pending) {
    // Where the request ends is unknown: the connection closes after the response
    auto stream = std::make_shared<http_stream>(++request_id_, reason, socket_->get_io_context().get_executor());
    stream->set_input_pending(input_pending);
    request_queue_.push_back(stream);
    handle_stock_error(stream, status);
}

}

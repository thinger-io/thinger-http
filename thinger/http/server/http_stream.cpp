#include "http_stream.hpp"
#include "request.hpp"
#include "../common/http_request.hpp"
#include "../../asio/io_context_of.hpp"
#include "../../util/logger.hpp"
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <string_view>

namespace thinger::http {

    void detail::continue_frame::to_buffer(std::vector<boost::asio::const_buffer>& buffer) const {
        static constexpr std::string_view head = "HTTP/1.1 100 Continue\r\n\r\n";
        buffer.emplace_back(boost::asio::buffer(head.data(), head.size()));
    }

    void detail::continue_frame::log(const char* scope, const body_logging&) const {
        LOG_DEBUG("[{}] HTTP/1.1 100 Continue", scope);
    }

    http_stream::http_stream(stream_id stream_id, const http_request& request, boost::asio::any_io_executor executor)
        : stream_id_(stream_id), executor_(std::move(executor)), cancelled_(cancel_reason::none) {
        // A client that does not keep the connection: by its Connection header, or by default
        // before HTTP/1.1
        if (!request.keep_alive()) {
            bool http11 = request.get_http_version_major() > 1 ||
                          (request.get_http_version_major() == 1 && request.get_http_version_minor() >= 1);
            must_close(http11 ? detail::close_reason::client_asked : detail::close_reason::http10);
        }
    }

    http_stream::http_stream(stream_id stream_id, detail::close_reason reason, boost::asio::any_io_executor executor)
        : stream_id_(stream_id), close_reason_(reason), executor_(std::move(executor)),
          cancelled_(cancel_reason::none) {}

    void http_stream::cancel(cancel_reason reason) {
        // The first reason wins, whatever thread gives it
        auto none = cancel_reason::none;
        if (reason == cancel_reason::none || !cancelled_.compare_exchange_strong(none, reason, std::memory_order_acq_rel)) {
            return;
        }
        boost::asio::dispatch(executor_, [self = shared_from_this(), reason] {
            if (self->cancel_event_) self->cancel_event_->set();
            if (!self->handler_running_) return;
            self->observer_signal_.emit(boost::asio::cancellation_type::terminal);
            // A drain deadline interrupts every handler; another reason, those declaring it
            if (reason == cancel_reason::shutdown || (self->cancel_on_ & (1u << static_cast<unsigned>(reason)))) {
                self->handler_interrupted_ = true;
                auto& signal = self->bound_signal_ ? *self->bound_signal_ : self->handler_signal_;
                signal.emit(boost::asio::cancellation_type::terminal);
            }
        });
    }

    void http_stream::end_input(bool follows) {
        if (asio::running_in_this_thread(executor_)) [[likely]] {
            set_input_pending(false, follows);
            return;
        }
        boost::asio::post(executor_, [self = shared_from_this(), follows] { self->set_input_pending(false, follows); });
    }

    awaitable<cancel_reason> http_stream::wait_cancelled() {
        // Created cancelled if the reason is there: cancel() may have come from another thread,
        // with its dispatched half (which sets the event, if any) still to run here
        if (!cancel_event_) {
            cancel_event_.emplace(executor_);
            if (cancelled() != cancel_reason::none) cancel_event_->set();
        }
        if (!co_await cancel_event_->wait()) co_return cancel_reason::none;
        co_return cancelled();
    }

    size_t http_stream::get_queue_size() const {
        return queue_.size();
    }

    bool http_stream::empty_queue() const {
        return queue_.empty();
    }

    std::shared_ptr<http_frame> http_stream::current_frame() const {
        return queue_.front();
    }

    void http_stream::pop_frame() {
        queue_.pop();
    }

    void http_stream::add_frame(std::shared_ptr<http_frame> frame) {
        queue_.push(std::move(frame));
        responded_ = true;
    }

    void http_stream::add_interim_frame(std::shared_ptr<http_frame> frame) {
        queue_.push(std::move(frame));
    }

    size_t http_stream::get_queued_frames() const {
        return queue_.size();
    }

    stream_id http_stream::id() const {
        return stream_id_;
    }
}
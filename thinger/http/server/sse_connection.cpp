#include "sse_connection.hpp"
#include <algorithm>
#include <string_view>

namespace thinger::http {

    void sse_connection::close(){
        if(closed_) return;
        closed_ = true;
        timer_.cancel();
        socket_->close();
        on_shutdown_callback_ = nullptr;
        // nothing else is written: closed first (nobody is told the queue drained, producers
        // waiting are told it closed), then emptied
        flow_.close();
        out_queue_.clear();
        keepalive_pending_ = false;
        flow_.clear();
        // no longer counts for the server
        if(tracking_) tracking_->closed();
        // told last, once, posted (the callback is released here: it may own this connection)
        on_close_.notify();
    }

    awaitable<void> sse_connection::read_loop([[maybe_unused]] std::shared_ptr<sse_connection> self){
        // The callbacks are released however the loop ends, also when its frame is destroyed
        // with the io_context: a callback holding the connection would keep it forever
        struct cycle_guard {
            sse_connection& connection;
            ~cycle_guard() {
                connection.on_shutdown_callback_ = nullptr;
                connection.on_close_.release();
                connection.flow_.close();
            }
        } guard{*this};

        uint8_t buffer[512];
        while(!closed_){
            auto [ec, bytes] = co_await socket_->read_some(buffer, sizeof(buffer));
            if(ec) break;
        }
        if(!closed_){
            LOG_DEBUG("sse client left");
            close();
        }
    }

    void sse_connection::track_shutdown(){
        tracking_ = taken_over_connection::of(*socket_);
        if(!tracking_) return;
        tracking_->set_type(connection_type::sse);
        tracking_->on_shutdown([weak = weak_from_this()]{
            auto self = weak.lock();
            if(!self) return;
            if(self->on_shutdown_callback_) self->on_shutdown_callback_();
            else self->close();
        });
    }

    namespace {
        /// Whether `value` holds a character that would end a field
        bool breaks_field(std::string_view value, bool nul = false){
            return value.find_first_of(nul ? std::string_view("\r\n\0", 3) : std::string_view("\r\n")) != std::string_view::npos;
        }
    }

    bool sse_connection::handle_write(const std::string& type, const std::string& value){
        // a field name is a name: no line breaks, nor the colon separating its value
        if(type.empty() || breaks_field(type) || type.find(':') != std::string::npos){
            LOG_ERROR("sse field refused: invalid field name");
            return false;
        }

        out_field field;
        // a data field ends its event
        field.ends_event = type == "data";
        if(field.ends_event){
            // one data field per line: CRLF, CR or LF end a line (WHATWG HTML, 9.2.6)
            field.data.reserve(value.size() + 8);
            std::string_view rest(value);
            for(;;){
                auto end = rest.find_first_of("\r\n");
                field.data.append(type).append(misc_strings::name_value_separator)
                     .append(rest.substr(0, end)).append(misc_strings::lf);
                if(end == std::string_view::npos) break;
                auto next = end + 1;
                if(rest[end] == '\r' && next < rest.size() && rest[next] == '\n') ++next;
                rest.remove_prefix(next);
            }
            field.data.append(misc_strings::lf);
        }else{
            // anything else is one line: a line break would end it and inject fields or events
            if(breaks_field(value, type == "id")){
                LOG_ERROR("sse field refused: the value of '{}' holds a line break{}", type,
                          type == "id" ? " or NUL" : "");
                return false;
            }
            field.data.reserve(type.size() + value.size() + 3);
            field.data.append(type).append(misc_strings::name_value_separator).append(value).append(misc_strings::lf);
        }

        boost::asio::dispatch(socket_->get_io_context(), [this, self = shared_from_this(), field = std::move(field)]() mutable {
            push_field(std::move(field));
        });
        return true;
    }

    void sse_connection::set_keepalive(std::chrono::milliseconds interval){
        boost::asio::dispatch(socket_->get_io_context(), [this, self = shared_from_this(), interval]{
            keepalive_ = interval;
            arm_keepalive();
        });
    }

    void sse_connection::arm_keepalive(){
        // a previous timer, if any, is stale
        auto generation = ++keepalive_generation_;
        timer_.cancel();
        if(closed_ || keepalive_.count() <= 0) return;
        idle_ = true;
        timer_.expires_after(keepalive_);
        timer_.async_wait([this, self = shared_from_this(), generation](const boost::system::error_code& ec){
            if(ec || closed_ || generation != keepalive_generation_) return;
            // only when the stream was quiet, and nothing is waiting to be written
            if(idle_ && !writing_ && out_queue_.empty()){
                keepalive_pending_ = true;
                process_out_queue();
            }
            arm_keepalive();
        });
    }

    void sse_connection::push_field(out_field field){
        if(flow_.closed()) return;
        switch(flow_.admit(field.data.size(), [this](size_t size){ return drop_oldest(size); })){
            case write_flow::admission::queue:
                flow_.add(field.data.size());
                out_queue_.push_back(std::move(field));
                process_out_queue();
                break;
            case write_flow::admission::drop:
                LOG_DEBUG("sse connection is congested. discarding field");
                break;
            case write_flow::admission::close:
                LOG_WARNING("sse connection is congested. closing it");
                close();
                break;
        }
    }

    bool sse_connection::drop_oldest(size_t size){
        auto it = out_queue_.begin();
        // the rest of the event being written is not dropped
        if(event_open_) it = next_event(it);
        size_t bytes = 0;
        size_t fields = 0;
        while(it != out_queue_.end() && flow_.overflows(size, bytes, fields)){
            // whole events only: an event still without its data stays
            auto end = next_event(it);
            if(!std::prev(end)->ends_event) break;
            LOG_DEBUG("sse connection is congested. discarding the oldest event");
            for(auto field = it; field != end; ++field) bytes += field->data.size();
            fields += static_cast<size_t>(std::distance(it, end));
            it = out_queue_.erase(it, end);
        }
        // accounted for once the queue is consistent
        flow_.remove(bytes, fields);
        return !flow_.overflows(size);
    }

    std::deque<sse_connection::out_field>::iterator sse_connection::next_event(std::deque<out_field>::iterator it){
        it = std::find_if(it, out_queue_.end(), [](const out_field& field){ return field.ends_event; });
        return it == out_queue_.end() ? it : std::next(it);
    }

    void sse_connection::process_out_queue(){
        if(writing_ || (out_queue_.empty() && !keepalive_pending_)) return;
        writing_ = true;
        co_spawn(socket_->get_io_context(), write_loop(shared_from_this()), detached);
    }

    awaitable<void> sse_connection::write_loop([[maybe_unused]] std::shared_ptr<sse_connection> self){
        while(!out_queue_.empty() || keepalive_pending_){
            out_field field;
            if(keepalive_pending_){
                // never queued, so neither dropped nor counted against the limits
                keepalive_pending_ = false;
                field.data = ": keep-alive\n";
                field.comment = true;
            }else{
                field = std::move(out_queue_.front());
                out_queue_.pop_front();
            }
            // a comment is written between fields: the event being written stays as it was
            if(!field.comment) event_open_ = !field.ends_event;

            flow_.write_started(*this, [](sse_connection& self){
                LOG_WARNING("closing sse connection: its write timed out");
                self.close();
            });
            auto [write_ec, write_bytes] = co_await socket_->write(std::string_view(field.data));
            flow_.write_finished();

            if(!field.comment) flow_.remove(field.data.size());
            if(write_ec){
                // nothing else can be sent
                close();
                break;
            }
            if(!field.comment) idle_ = false;
        }
        writing_ = false;
    }

    awaitable<bool> sse_connection::wait_writable(){
        return flow_.wait_writable(shared_from_this());
    }

    void sse_connection::set_write_limits(const write_limits& limits){
        flow_.set_limits(shared_from_this(), limits);
    }

    void sse_connection::on_high_watermark(sync_callback<void()> callback){
        flow_.on_high_watermark(shared_from_this(), std::move(callback));
    }

    void sse_connection::on_low_watermark(sync_callback<void()> callback){
        flow_.on_low_watermark(shared_from_this(), std::move(callback));
    }

}

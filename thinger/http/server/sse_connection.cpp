#include "sse_connection.hpp"
#include <algorithm>

namespace thinger::http {

    void sse_connection::close(){
        timer_.cancel();
        socket_->close();
        on_shutdown_callback_ = nullptr;
        // nothing else is written: the watermark callbacks are released, and producers told
        for(auto& field : out_queue_) flow_.remove(field.data.size());
        out_queue_.clear();
        flow_.close();
        // no longer counts for the server
        if(tracking_) tracking_->closed();
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

    void sse_connection::handle_write(const std::string& type, const std::string& value){
        out_field field;
        field.data.reserve(type.size() + value.size() + 4);
        field.data.append(type).append(misc_strings::name_value_separator).append(value).append(misc_strings::lf);
        // a data field ends its event
        field.ends_event = type == "data";
        if(field.ends_event) field.data.append(misc_strings::lf);

        boost::asio::dispatch(socket_->get_io_context(), [this, self = shared_from_this(), field = std::move(field)]() mutable {
            push_field(std::move(field));
        });
    }

    void sse_connection::push_field(out_field field){
        if(flow_.closed()) return;
        // a full queue: the overflow policy decides
        if(flow_.overflows(queued_messages(), field.data.size()) && !make_room(field.data.size())) return;
        flow_.add(field.data.size());
        out_queue_.push_back(std::move(field));
        process_out_queue();
    }

    bool sse_connection::make_room(size_t size){
        switch(flow_.limits().policy){
            case overflow_policy::drop_oldest:
                drop_oldest(size);
                return true;
            case overflow_policy::close:
                LOG_WARNING("sse connection is congested. closing it");
                close();
                return false;
            case overflow_policy::drop_newest:
                break;
        }
        LOG_DEBUG("sse connection is congested. discarding field");
        return false;
    }

    void sse_connection::drop_oldest(size_t size){
        auto ends_event = [](const out_field& field){ return field.ends_event; };
        auto it = out_queue_.begin();
        // the rest of the event being written is not dropped
        if(event_open_){
            it = std::find_if(it, out_queue_.end(), ends_event);
            if(it == out_queue_.end()) return;
            ++it;
        }
        while(flow_.overflows(queued_messages(), size)){
            // whole events only: an event still without its data stays
            auto last = std::find_if(it, out_queue_.end(), ends_event);
            if(last == out_queue_.end()) return;
            LOG_DEBUG("sse connection is congested. discarding the oldest event");
            auto end = std::next(last);
            std::for_each(it, end, [this](const out_field& field){ flow_.remove(field.data.size()); });
            it = out_queue_.erase(it, end);
        }
    }

    void sse_connection::process_out_queue(){
        if(writing_ || out_queue_.empty()) return;
        writing_ = true;
        co_spawn(socket_->get_io_context(), write_loop(shared_from_this()), detached);
    }

    awaitable<void> sse_connection::write_loop([[maybe_unused]] std::shared_ptr<sse_connection> self){
        while(!out_queue_.empty()){
            auto field = std::move(out_queue_.front());
            out_queue_.pop_front();
            field_in_flight_ = true;
            event_open_ = !field.ends_event;

            auto [write_ec, write_bytes] = co_await socket_->write(std::string_view(field.data));

            field_in_flight_ = false;
            flow_.remove(field.data.size());
            if(write_ec){
                // nothing else can be sent
                close();
                break;
            }
            idle_ = false;
        }
        writing_ = false;
    }

    awaitable<bool> sse_connection::wait_writable(){
        auto self = shared_from_this();
        co_return co_await on_io_context(socket_->get_io_context(), flow_.wait_writable());
    }

}

#include "http_stream.hpp"

namespace thinger::http {

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
        queue_.push(frame);
        responded_ = true;
    }

    size_t http_stream::get_queued_frames() const {
        return queue_.size();
    }

    stream_id http_stream::id() const {
        return stream_id_;
    }
}
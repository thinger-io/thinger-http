#include "sse_connection.hpp"

namespace thinger::http {

    void sse_connection::close(){
        timer_.cancel();
        socket_->close();
        on_shutdown_callback_ = nullptr;
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

}

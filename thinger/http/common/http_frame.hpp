#ifndef THINGER_HTTP_FRAME_HPP
#define THINGER_HTTP_FRAME_HPP

#include <functional>
#include "../data/out_data.hpp"
#include "body_logging.hpp"

namespace thinger::http {

    class http_frame : public data::out_data {
    public:
        // Called once with whether the frame was written to its connection: false if the
        // write failed, or if the frame is dropped without being written
        using written_callback = std::function<void(bool written)>;

        // constructors
        http_frame() = default;
        ~http_frame() override;

        // A copy is another frame: it is not written when this one is (no written callback)
        http_frame(const http_frame& other) : data::out_data(other), last_frame_(other.last_frame_) {}
        http_frame& operator=(const http_frame& other) {
            data::out_data::operator=(other);
            last_frame_ = other.last_frame_;
            return *this;
        }

        // stream information
        void set_last_frame(bool last_frame);
        virtual bool end_stream();

        // Notify `callback` once the frame is written (see written_callback)
        void on_written(written_callback callback);

        // Report the frame written (or failed) to the callback set with on_written(), once
        void notify_written(bool written);

        // Log the frame (see body_logging): nothing by default, as for body data
        virtual void log(const char* scope, const body_logging& options = {}) const;

    private:
        bool last_frame_ = true;
        written_callback on_written_;

    };

}

#endif

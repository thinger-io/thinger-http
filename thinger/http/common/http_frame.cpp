#include "../../util/logger.hpp"
#include "http_frame.hpp"
#include <utility>

namespace thinger::http {

    http_frame::~http_frame() {
        // dropped without being written (e.g. its connection is gone)
        notify_written(false);
    }

    void http_frame::set_last_frame(bool last_frame) {
        last_frame_ = last_frame;
    }

    bool http_frame::end_stream() {
        return last_frame_;
    }

    void http_frame::on_written(written_callback callback) {
        on_written_ = std::move(callback);
    }

    void http_frame::notify_written(bool written) {
        if (!on_written_) return;
        auto callback = std::exchange(on_written_, nullptr);
        try {
            callback(written);
        } catch (const std::exception& e) {
            LOG_ERROR("frame written callback failed: {}", e.what());
        }
    }

    void http_frame::log(const char*, const body_logging&) const {}

}

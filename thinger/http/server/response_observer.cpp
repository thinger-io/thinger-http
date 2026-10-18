#include "response_observer.hpp"
#include "request.hpp"
#include "../../util/logger.hpp"

namespace thinger::http {

response_observer::response_observer(std::shared_ptr<request> req, std::shared_ptr<const response_hook> hook)
    : request_(std::move(req)), hook_(std::move(hook)) {
    request_->set_peer_ip(request_->get_peer_ip());
}

response_observer::~response_observer() {
    if (reported_) return;
    if (!ended_) {
        info_.outcome = response_outcome::aborted;
        info_.duration = std::chrono::steady_clock::now() - start_;
    }
    report();
}

void response_observer::head(http_response::status status, bool streamed) {
    // The last head is the response that ends (e.g. the 502 replacing a stream aborted in memory)
    info_.status = status;
    info_.streamed = streamed;
}

void response_observer::ended(response_outcome outcome) {
    if (ended_) return;
    ended_ = true;
    info_.outcome = aborted_.load(std::memory_order_relaxed) ? response_outcome::aborted : outcome;
    info_.duration = std::chrono::steady_clock::now() - start_;
    done();
}

void response_observer::handled() {
    if (handled_) return;
    handled_ = true;
    done();
}

void response_observer::done() {
    // What each side wrote is seen by the one reporting
    if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) report();
}

void response_observer::report() noexcept {
    reported_ = true;
    try {
        (*hook_)(*request_, info_);
    } catch (const std::exception& e) {
        LOG_ERROR("error in the response hook: {}", e.what());
    } catch (...) {
        LOG_ERROR("unknown error in the response hook");
    }
}

} // namespace thinger::http

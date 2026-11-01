#include "watchdog.hpp"
#include <algorithm>

namespace thinger::asio {

watchdog::watchdog(boost::asio::any_io_executor executor, std::function<void()> on_expired)
    : state_(std::make_shared<state>(boost::asio::steady_timer{std::move(executor)}, std::move(on_expired))) {}

watchdog::~watchdog() {
    cancel();
}

void watchdog::expires_at(clock::time_point time) {
    state_->deadline = time;
    // A running timer checks the new deadline when it expires, unless it is earlier
    if (!state_->armed || std::max(state_->deadline, state_->hold) < state_->timer.expiry()) arm(state_);
}

void watchdog::hold_until(clock::time_point time) {
    // A running timer checks the hold when it expires
    state_->hold = std::max(state_->hold, time);
}

void watchdog::cancel() {
    state_->armed = false;
    state_->timer.cancel();
}

void watchdog::arm(const std::shared_ptr<state>& s) {
    s->armed = true;
    // Re-arming cancels the wait in progress, whose handler then returns
    s->timer.expires_at(std::max(s->deadline, s->hold));
    s->timer.async_wait([weak = std::weak_ptr<state>(s)](const boost::system::error_code& ec) {
        on_timer(weak, ec);
    });
}

void watchdog::on_timer(const std::weak_ptr<state>& weak, const boost::system::error_code& ec) {
    if (ec) return;  // cancelled or re-armed
    auto s = weak.lock();
    if (!s || !s->armed) return;  // gone, or cancelled once the timer had expired
    auto now = clock::now();
    // The timer was set again after this wait had expired: the wait of the new expiry checks
    if (now < s->timer.expiry()) return;
    // Some activity moved the deadline meanwhile, or a hold keeps it
    if (now < std::max(s->deadline, s->hold)) {
        arm(s);
        return;
    }
    s->armed = false;
    // `s` keeps the callback alive if it destroys the watchdog; nothing touches it afterwards
    s->on_expired();
}

}

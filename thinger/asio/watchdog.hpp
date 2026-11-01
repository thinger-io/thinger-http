#ifndef THINGER_ASIO_WATCHDOG_HPP
#define THINGER_ASIO_WATCHDOG_HPP

#include <chrono>
#include <functional>
#include <memory>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>

namespace thinger::asio {

    // A deadline that activity moves later: an idle timeout, a keep-alive, a write timeout. One
    // timer, checked lazily on expiry: moving the deadline later does not touch it (it re-arms
    // itself when it expires before the deadline), only an earlier one re-arms it. Deadlines
    // that activity can move use it, never a generation counter.
    //
    // It belongs to its executor, as the object that owns it: its functions are called there,
    // and on_expired runs there, from the timer, never inside a call. on_expired runs once per
    // expiry, never after cancel() or the watchdog's destruction (even if the timer had expired
    // already, its handler not run yet). The watchdog never keeps its owner alive: on_expired
    // holds a weak reference to it, and may close or destroy it, the watchdog included
    class watchdog {
    public:
        using clock = std::chrono::steady_clock;

        watchdog(boost::asio::any_io_executor executor, std::function<void()> on_expired);
        ~watchdog();

        watchdog(const watchdog&) = delete;
        watchdog& operator=(const watchdog&) = delete;

        // Expire at `time` (or at the hold, if later). Later than the armed deadline, the timer
        // is left as it is; earlier, or with no deadline armed (new, expired or cancelled), it is
        // armed. A time already past expires on a next run of the executor
        void expires_at(clock::time_point time);

        // Never expire before `time`: a later hold replaces an earlier one, and lasts across
        // expires_at() and cancel(). It arms nothing by itself
        void hold_until(clock::time_point time);

        // Disarm it: on_expired is not called until expires_at() arms it again
        void cancel();

    private:
        struct state {
            boost::asio::steady_timer timer;
            std::function<void()> on_expired;
            clock::time_point deadline{};
            clock::time_point hold{};
            bool armed = false;
        };

        static void arm(const std::shared_ptr<state>& s);
        static void on_timer(const std::weak_ptr<state>& weak, const boost::system::error_code& ec);

        // Shared with the timer's handler, which holds it weakly: a handler run after the
        // watchdog is gone finds nothing
        std::shared_ptr<state> state_;
    };

}

#endif

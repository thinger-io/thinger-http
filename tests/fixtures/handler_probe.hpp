#ifndef THINGER_HTTP_TEST_HANDLER_PROBE_HPP
#define THINGER_HTTP_TEST_HANDLER_PROBE_HPP

#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/routing/route.hpp>
#include <thinger/util/types.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <atomic>
#include <chrono>
#include <thread>

// What a handler under test did about the cancellation scope of its request (see
// route::cancel_on): when it started and ended, whether its wait was interrupted, and the reason
// in request::cancelled() at its end. Written on a thread of the server, read by the test
namespace thinger::http::test {

    // Wait `duration` on a timer, bound to `slot` if connected (else to the cancellation state
    // of the calling coroutine): whether the wait was interrupted, with operation_aborted, or by
    // the throw of a coroutine already cancelled (asio's throw_if_cancelled)
    inline awaitable<bool> interrupted_wait(std::chrono::milliseconds duration,
                                            boost::asio::cancellation_slot slot = {}) {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, duration);
        boost::system::error_code ec;
        try {
            if (slot.is_connected()) {
                std::tie(ec) = co_await timer.async_wait(boost::asio::bind_cancellation_slot(slot, use_nothrow_awaitable));
            } else {
                std::tie(ec) = co_await timer.async_wait(use_nothrow_awaitable);
            }
        } catch (const boost::system::system_error& e) {
            ec = e.code();
        }
        co_return ec == boost::asio::error::operation_aborted;
    }

    struct handler_probe {
        using clock = std::chrono::steady_clock;

        std::atomic<bool> started{false};
        std::atomic<bool> ended{false};
        std::atomic<bool> interrupted{false};
        std::atomic<cancel_reason> reason{cancel_reason::none};
        std::atomic<clock::rep> started_at{0};
        std::atomic<clock::rep> ended_at{0};

        void start() {
            started_at = clock::now().time_since_epoch().count();
            started = true;
        }

        // At the end of the handler: whether its wait was interrupted, and the reason of its scope
        void end(const request& req, bool was_interrupted) {
            ended_at = clock::now().time_since_epoch().count();
            reason = req.cancelled();
            interrupted = was_interrupted;
            ended = true;
        }

        bool wait_started(std::chrono::milliseconds timeout = std::chrono::seconds(3)) const {
            return wait_for(started, timeout);
        }

        bool wait_ended(std::chrono::milliseconds timeout = std::chrono::seconds(3)) const {
            return wait_for(ended, timeout);
        }

        clock::time_point start_time() const { return clock::time_point(clock::duration(started_at.load())); }
        clock::time_point end_time() const { return clock::time_point(clock::duration(ended_at.load())); }

        // How long after `when` the handler ended (negative if before)
        std::chrono::milliseconds ended_after(clock::time_point when) const {
            return std::chrono::duration_cast<std::chrono::milliseconds>(end_time() - when);
        }

    private:
        static bool wait_for(const std::atomic<bool>& flag, std::chrono::milliseconds timeout) {
            auto deadline = clock::now() + timeout;
            while (!flag) {
                if (clock::now() > deadline) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        }
    };

    // A coroutine handler waiting `duration` on a timer (bound to request::cancellation_slot()
    // if `bound`), then answering "done", told to `probe`
    template<typename Server>
    route& waiting_route(Server& server, const std::string& path, handler_probe& probe,
                         std::chrono::milliseconds duration, bool bound = false) {
        return server.get(path, [&probe, duration, bound](request& req, response& res) -> awaitable<void> {
            probe.start();
            bool interrupted = co_await interrupted_wait(duration, bound ? req.cancellation_slot()
                                                                         : boost::asio::cancellation_slot{});
            probe.end(req, interrupted);
            res.send("done");
        });
    }

}

#endif // THINGER_HTTP_TEST_HANDLER_PROBE_HPP

// Graceful shutdown of the servers (see server_base::shutdown()), apart from the code
// serving requests
#include "server_base.hpp"
#include "../../asio/async_event.hpp"
#include "../../asio/with_timeout.hpp"
#include "../../util/logger.hpp"
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <algorithm>
#include <condition_variable>
#include <thread>
#include <utility>

namespace thinger::http {

// A graceful shutdown, until its deadline. What runs it and waits for it owns it, and knows the
// server only while it runs: stop() (from the destructors too) ends the shutdown first, waiting
// for one stopping the server on another thread.
//
// It ends in two steps. Its result is decided first: every connection finished, or the
// deadline came (the connections left are closed). Those waiting are told at once. Then the
// server stops, once the connections are closed and the coroutines awaiting the result resumed:
// a server stopping its own io_context would drop them otherwise.
struct server_base::shutdown_state : std::enable_shared_from_this<shutdown_state> {
    // A coroutine awaiting the result
    struct waiter {
        boost::asio::any_io_executor executor;
        std::shared_ptr<asio::async_event> woken;   // set on `executor`
    };

    std::mutex mutex;
    std::condition_variable ended;   // decided or done
    server_base* server = nullptr;   // none once stopped
    std::thread::id finishing;       // the thread stopping the server at the end
    bool expired = false;            // the deadline came with connections open
    bool timing = false;             // the deadline is waited for on io_context
    bool decided = false;
    bool clean = false;              // every connection finished in time
    bool done = false;               // the server stopped
    // What the server stops after: the connections (finished, or closed at the deadline), and
    // every coroutine awaiting the result
    size_t holds = 1;
    std::vector<waiter> waiters;

    // Set once when it starts
    std::string name;   // the server, for the logs
    std::shared_ptr<connection_tracker> connections;
    boost::asio::io_context* io_context = nullptr;   // where the connections are checked
    std::chrono::steady_clock::time_point deadline;
    // While waiting: the timer of the deadline, owned by its wait (on io_context)
    std::weak_ptr<boost::asio::steady_timer> deadline_timer;

    bool result() {
        std::scoped_lock lock(mutex);
        return clean;
    }

    // On io_context: whether the connections are gone. Checked again once the last one leaves
    // (the tracker tells, see wait()), or at the deadline
    void check() {
        {
            std::scoped_lock lock(mutex);
            if (decided || expired) return;
        }
        if (connections->active() == 0) {
            // After what the connections posted as they closed (their close callbacks, see
            // websocket_connection::on_close()), which a server stopping its io_context would drop
            boost::asio::post(*io_context, [self = shared_from_this()] {
                // (its wait would keep an io_context run until out of work busy until then)
                if (auto timer = self->deadline_timer.lock()) timer->cancel();
                if (self->decide(true)) self->release();
            });
        } else if (std::chrono::steady_clock::now() >= deadline) {
            expire();
        } else {
            wait();
        }
    }

    // On io_context: check() again when the last connection leaves, or at the deadline. Neither
    // keeps this alive (the server, or those awaiting the result, do)
    void wait() {
        bool timed;
        {
            std::scoped_lock lock(mutex);
            timed = std::exchange(timing, true);
        }
        if (!timed) {
            auto timer = std::make_shared<boost::asio::steady_timer>(*io_context, deadline);
            deadline_timer = timer;
            timer->async_wait([weak = weak_from_this(), timer](const boost::system::error_code& ec) {
                if (auto self = weak.lock(); self && !ec) self->check();
            });
        }
        // Again after it told (a connection accepted late may have joined, and left, since):
        // right away if the last one left since it was counted
        connections->notify_when_empty([weak = weak_from_this()] {
            if (auto self = weak.lock()) {
                boost::asio::post(*self->io_context, [self] { self->check(); });
            }
        });
    }

    // The deadline came, on any thread (also one waiting, as the threads of the server may be
    // stuck): the streams still in progress are cancelled, and the connections closed on their
    // threads, without waiting for them
    void expire() {
        auto open = connections->active();
        if (open == 0) {
            if (decide(true)) release();
            return;
        }
        {
            std::scoped_lock lock(mutex);
            if (decided || expired) return;
            expired = true;
        }
        LOG_WARNING("closing {} connections still open at the end of the grace period", open);
        connections->set_phase(connection_tracker::phase::closing);
        decide(false);
        // The requests still in progress are cancelled with the shutdown as the reason, which
        // interrupts every handler still running (see route::cancel_on), right before their
        // connections close, in the same call on the io_context of each: the handlers resume
        // on a closed connection, whose response is never written. They are not waited for
        connections->cancel_and_close_all([self = shared_from_this()] { self->release(); });
    }

    // The result, if not decided yet (a clean one only before the deadline came): the waiters
    // are told
    bool decide(bool result) {
        std::vector<waiter> woken;
        {
            std::scoped_lock lock(mutex);
            if (decided || (result && expired)) return false;
            decided = true;
            clean = result;
            woken.swap(waiters);
        }
        // Not told of the last connection anymore
        connections->notify_when_empty(nullptr);
        ended.notify_all();
        for (auto& waiter : woken) {
            boost::asio::post(waiter.executor, [woken = std::move(waiter.woken)] { woken->set(); });
        }
        return true;
    }

    // A coroutine on `executor` awaits the result: false if it is known already
    bool await(boost::asio::any_io_executor executor, std::shared_ptr<asio::async_event> woken) {
        std::scoped_lock lock(mutex);
        if (decided) return false;
        waiters.push_back({std::move(executor), std::move(woken)});
        ++holds;
        return true;
    }

    // The coroutine awaiting with `woken` resumed (or is gone)
    void leave(const std::shared_ptr<asio::async_event>& woken) {
        {
            std::scoped_lock lock(mutex);
            std::erase_if(waiters, [&](const waiter& waiter) { return waiter.woken == woken; });
        }
        release();
    }

    // What the server stops after ended: once nothing else holds it, it stops
    void release() {
        server_base* stopping;
        {
            std::scoped_lock lock(mutex);
            if (--holds > 0 || done || finishing != std::thread::id()) return;
            finishing = std::this_thread::get_id();
            stopping = server;
        }
        // Anything arriving late is closed
        connections->set_phase(connection_tracker::phase::closing);
        // Not stop(), which a subclass being destroyed meanwhile may override (see
        // set_shutdown_stop())
        if (stopping) {
            stopping->stop_listening();
            if (stopping->shutdown_stop_) stopping->shutdown_stop_();
        }
        bool result;
        {
            std::scoped_lock lock(mutex);
            done = true;
            finishing = std::thread::id();
            result = clean;
        }
        ended.notify_all();
        LOG_INFO("{} stopped{}", name, result ? "" : " (connections closed before they finished)");
    }
};

server_base::~server_base() {
    // Not virtual: a subclass's override already ran, if its destructor called it
    server_base::stop();
}

bool server_base::reset_shutdown() {
    if (shutdown_) {
        std::scoped_lock state_lock(shutdown_->mutex);
        if (!shutdown_->done) return false;
    }
    // Serving again after a shutdown that ended
    shutdown_.reset();
    connections_->set_phase(connection_tracker::phase::serving);
    return true;
}

void server_base::on_shutdown(sync_callback<void()> callback) {
    on_shutdown_ = std::move(callback);
}

void server_base::set_shutdown_stop(sync_callback<void()> stop) {
    shutdown_stop_ = std::move(stop);
}

server_stats server_base::stats() const {
    return connections_->stats();
}

awaitable<bool> server_base::shutdown(std::chrono::steady_clock::time_point by) {
    auto state = start_shutdown(by);
    auto executor = co_await boost::asio::this_coro::executor;
    auto woken = std::make_shared<asio::async_event>(executor);
    if (state->await(executor, woken)) {
        // Left however the wait ends, also with this coroutine destroyed before it resumes
        struct waiting {
            std::shared_ptr<shutdown_state> state;
            std::shared_ptr<asio::async_event> woken;
            ~waiting() { state->leave(woken); }
        } waiting{state, woken};
        if (!co_await asio::with_timeout(woken->wait(), state->deadline - std::chrono::steady_clock::now())) {
            // Not decided at the deadline, on this executor: those of the server may be stuck
            state->expire();
        }
    }
    co_return state->result();
}

bool server_base::shutdown(std::chrono::milliseconds grace) {
    auto by = std::chrono::steady_clock::now() + grace;
    if (in_server_thread()) {
        LOG_ERROR("shutdown({} ms) cannot block a thread of the server, which the shutdown needs: "
                  "shutting down without waiting (co_await shutdown(by) there instead)", grace.count());
        start_shutdown(by);
        return false;
    }
    auto state = start_shutdown(by);
    std::unique_lock lock(state->mutex);
    if (!state->ended.wait_until(lock, state->deadline, [&] { return state->done; })) {
        // Not decided at the deadline: the threads of the server may be stuck
        lock.unlock();
        state->expire();
        lock.lock();
        state->ended.wait(lock, [&] { return state->decided; });
    }
    return state->clean;
}

std::shared_ptr<server_base::shutdown_state> server_base::start_shutdown(std::chrono::steady_clock::time_point by) {
    std::shared_ptr<shutdown_state> state;
    {
        std::scoped_lock lock(shutdown_mutex_);
        // In progress, or ended: the same end
        if (shutdown_) return shutdown_;
        state = std::make_shared<shutdown_state>();
        state->server = this;
        state->name = log_name();
        state->connections = connections_;
        state->deadline = by;
        shutdown_ = state;
    }

    // What is left of it, rounded up: the grace given to shutdown(grace), once it reaches here
    auto budget = std::chrono::ceil<std::chrono::milliseconds>(by - std::chrono::steady_clock::now());
    LOG_INFO("shutting down the {}: {} connections open, grace period of {} ms",
             state->name, connections_->active(), std::max<int64_t>(budget.count(), 0));

    // New connections are refused, and those accepted meanwhile closed
    connections_->set_phase(connection_tracker::phase::draining);
    stop_listening();

    if (on_shutdown_) {
        try {
            on_shutdown_();
        } catch (const std::exception& e) {
            LOG_ERROR("error in the shutdown callback: {}", e.what());
        }
    }

    state->io_context = shutdown_context();
    if (!state->io_context) {
        state->decide(connections_->active() == 0);
        state->release();
        return state;
    }
    connections_->drain_all();
    boost::asio::post(*state->io_context, [state] { state->check(); });
    return state;
}

void server_base::end_shutdown() {
    std::shared_ptr<shutdown_state> state;
    {
        std::scoped_lock lock(shutdown_mutex_);
        state = shutdown_;
    }
    if (!state) return;
    {
        std::unique_lock lock(state->mutex);
        state->server = nullptr;
        if (state->done || state->finishing == std::this_thread::get_id()) return;
        // Ending on another thread, which is stopping this server: wait for it
        if (state->finishing != std::thread::id()) {
            state->ended.wait(lock, [&] { return state->done; });
            return;
        }
    }
    state->decide(false);
    {
        std::scoped_lock lock(state->mutex);
        state->done = true;
    }
    state->ended.notify_all();
}

} // namespace thinger::http

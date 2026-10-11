// Graceful shutdown of the servers (see http_server_base::shutdown()), apart from the code
// serving requests
#include "http_server_base.hpp"
#include "../../util/logger.hpp"
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <condition_variable>
#include <thread>

namespace thinger::http {

namespace {

// How often a shutdown checks whether the connections are gone, and how long it waits for the
// connections to be closed once the grace period ends
constexpr auto shutdown_check_interval = std::chrono::milliseconds(10);
constexpr auto shutdown_close_timeout = std::chrono::seconds(1);

}

// A graceful shutdown. Its handlers own it, and know the server only while it runs: stop()
// (from the destructors too) ends the shutdown first, waiting for one stopping the server on
// another thread
struct http_server_base::shutdown_state {
    std::mutex mutex;
    std::condition_variable ended;
    http_server_base* server = nullptr;   // none once stopped
    std::thread::id finishing;            // the thread stopping the server at the end
    bool done = false;
    bool clean = false;                   // every connection finished in time
    std::vector<std::function<void(bool)>> callbacks;

    // Set once when it starts
    std::shared_ptr<connection_tracker> connections;
    boost::asio::io_context* io_context = nullptr;
    std::chrono::steady_clock::time_point deadline;
};

http_server_base::~http_server_base() {
    end_shutdown();
}

void http_server_base::reset_shutdown() {
    // Serving again after a shutdown that ended
    std::scoped_lock lock(shutdown_mutex_);
    if (shutdown_) {
        std::scoped_lock state_lock(shutdown_->mutex);
        if (!shutdown_->done) return;
    }
    shutdown_.reset();
    connections_->set_phase(connection_tracker::phase::serving);
}

void http_server_base::on_shutdown(std::function<void()> callback) {
    on_shutdown_ = std::move(callback);
}

server_stats http_server_base::stats() const {
    return connections_->stats();
}

bool http_server_base::shutdown(std::chrono::milliseconds grace) {
    if (in_server_thread()) {
        LOG_ERROR("shutdown() cannot wait on a thread of the server: use begin_shutdown() there");
        begin_shutdown(grace);
        return false;
    }
    begin_shutdown(grace);
    std::shared_ptr<shutdown_state> state;
    {
        std::scoped_lock lock(shutdown_mutex_);
        state = shutdown_;
    }
    if (!state) return false;
    std::unique_lock lock(state->mutex);
    state->ended.wait(lock, [&] { return state->done; });
    return state->clean;
}

void http_server_base::begin_shutdown(std::chrono::milliseconds grace, std::function<void(bool)> on_done) {
    std::shared_ptr<shutdown_state> state;
    std::optional<bool> ended;   // the result of a shutdown that already ended
    {
        std::scoped_lock lock(shutdown_mutex_);
        if (shutdown_) {
            std::scoped_lock state_lock(shutdown_->mutex);
            if (!shutdown_->done) {
                if (on_done) shutdown_->callbacks.push_back(std::move(on_done));
                return;
            }
            ended = shutdown_->clean;
        } else {
            state = std::make_shared<shutdown_state>();
            state->server = this;
            state->connections = connections_;
            state->deadline = std::chrono::steady_clock::now() + grace;
            if (on_done) state->callbacks.push_back(std::move(on_done));
            shutdown_ = state;
        }
    }
    if (ended) {
        if (on_done) on_done(*ended);
        return;
    }

    LOG_INFO("shutting down the server: {} connections open, grace period of {} ms",
             connections_->active(), grace.count());

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
        finish_shutdown(state, connections_->active() == 0);
        return;
    }
    connections_->drain_all();
    boost::asio::post(*state->io_context, [state] { check_shutdown(state); });
}

void http_server_base::check_shutdown(const std::shared_ptr<shutdown_state>& state) {
    {
        std::scoped_lock lock(state->mutex);
        if (state->done || state->finishing != std::thread::id()) return;
    }
    if (state->connections->active() == 0) {
        finish_shutdown(state, true);
    } else if (std::chrono::steady_clock::now() >= state->deadline) {
        force_shutdown(state);
    } else {
        wait_shutdown(state);
    }
}

void http_server_base::wait_shutdown(const std::shared_ptr<shutdown_state>& state) {
    auto next = std::min(std::chrono::steady_clock::now() + shutdown_check_interval, state->deadline);
    auto timer = std::make_shared<boost::asio::steady_timer>(*state->io_context, next);
    timer->async_wait([state, timer](const boost::system::error_code& ec) {
        if (!ec) check_shutdown(state);
    });
}

void http_server_base::force_shutdown(const std::shared_ptr<shutdown_state>& state) {
    LOG_WARNING("closing {} connections still open at the end of the grace period", state->connections->active());
    state->connections->set_phase(connection_tracker::phase::closing);

    // Done once every connection was closed on its thread; or anyway after a while, as a
    // thread that is stuck never closes its connections
    auto timer = std::make_shared<boost::asio::steady_timer>(*state->io_context, shutdown_close_timeout);
    timer->async_wait([state, timer](const boost::system::error_code& ec) {
        if (!ec) finish_shutdown(state, false);
    });
    state->connections->close_all([state] { finish_shutdown(state, false); });
}

void http_server_base::finish_shutdown(const std::shared_ptr<shutdown_state>& state, bool clean) {
    http_server_base* server;
    {
        std::scoped_lock lock(state->mutex);
        if (state->done || state->finishing != std::thread::id()) return;
        state->finishing = std::this_thread::get_id();
        server = state->server;
    }
    // Anything arriving late is closed
    state->connections->set_phase(connection_tracker::phase::closing);
    if (server) server->stop();
    complete_shutdown(state, clean);
}

void http_server_base::complete_shutdown(const std::shared_ptr<shutdown_state>& state, bool clean) {
    std::vector<std::function<void(bool)>> callbacks;
    {
        std::scoped_lock lock(state->mutex);
        if (state->done) return;
        state->done = true;
        state->clean = clean;
        state->finishing = std::thread::id();
        callbacks.swap(state->callbacks);
    }
    state->ended.notify_all();
    LOG_INFO("server shut down{}", clean ? "" : " (connections closed before they finished)");
    for (auto& callback : callbacks) {
        try {
            callback(clean);
        } catch (const std::exception& e) {
            LOG_ERROR("error in a shutdown callback: {}", e.what());
        }
    }
}

void http_server_base::end_shutdown() {
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
    complete_shutdown(state, false);
}

} // namespace thinger::http

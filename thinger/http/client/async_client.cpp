#include "async_client.hpp"
#include "../../util/logger.hpp"
#include <algorithm>
#include <condition_variable>
#include <list>
#include <mutex>
#include <thread>
#include <vector>

namespace thinger::http {

namespace detail {

// How long stop() and the destructor wait for the cancelled requests to end, before leaving
// behind what remains of them: the cancellation reaches every operation of a request at once,
// unless one ignores it
constexpr auto end_grace = std::chrono::seconds{1};

// The HTTP client the requests of an async_client use, and the requests in flight. The client
// holds it, and each request as long as any of its frames exists
class client_state final : public http_client_base {
public:
    explicit client_state(asio::workers& workers) : workers(workers) {}

    asio::workers& workers;

    std::mutex mutex;
    std::condition_variable changed;

    // Guarded by the mutex: the requests in flight, the threads running a synchronous callback
    // of one of them, and how many calls of stop() or the destructor run (or the destructor
    // ran), which start no request
    std::list<std::shared_ptr<client_request>> requests;
    std::vector<std::thread::id> callbacks;
    unsigned closers = 0;
    // The requests in flight not cancelled (and those the application tracks), written under
    // the mutex
    std::atomic<size_t> active{0};

    // Whether a thread other than the caller runs a callback (lock held)
    bool callbacks_elsewhere() const {
        return std::any_of(callbacks.begin(), callbacks.end(),
                           [this_thread = std::this_thread::get_id()](auto id) { return id != this_thread; });
    }

protected:
    // The worker of the calling thread, or else one of the pool
    boost::asio::io_context& get_io_context() override { return workers.get_thread_io_context(); }
};

// A request in flight: where it runs, what cancels it, and its place among the requests
class client_request {
public:
    client_request(std::shared_ptr<client_state> state, boost::asio::io_context::executor_type executor)
        : state(std::move(state)), executor(executor) {}

    std::shared_ptr<client_state> state;
    boost::asio::io_context::executor_type executor;
    // Connected by co_spawn to the coroutine; emitted on its thread
    boost::asio::cancellation_signal signal;
    std::list<std::shared_ptr<client_request>>::iterator position;
    // Guarded by the mutex of the state: its coroutine is spawned (the signal may be emitted), it
    // was cancelled, and stop() or the destructor gave up waiting for it
    bool spawned = false;
    bool cancelled = false;
    bool left_behind = false;
    // Set under the mutex, read too on its thread, where it is set
    std::atomic<bool> ended{false};

    // Cancel it, on its thread: whatever it waits for ends with operation_aborted
    static void cancel(const std::shared_ptr<client_request>& request) {
        boost::asio::dispatch(request->executor, [request] {
            if (!request->ended) request->signal.emit(boost::asio::cancellation_type::terminal);
        });
    }

    void end() {
        std::lock_guard lock(state->mutex);
        ended = true;
        if (!cancelled && state->active > 0) --state->active;
        state->requests.erase(position);
        state->changed.notify_all();
    }
};

} // namespace detail

async_client::async_client(asio::workers& workers)
    : state_(std::make_shared<detail::client_state>(workers)), client_(*state_) {
    LOG_DEBUG("Created HTTP async client");
}

async_client::~async_client() {
    LOG_DEBUG("Destroying HTTP async client");
    running_ = false;
    // The idle connections close now, those in use when their requests give them back: they
    // belong to the io_contexts of the workers, which the requests may outlive as frames
    client_.max_idle_connections_per_host(0);
    close_requests(true);
}

async_client::request_end::~request_end() {
    if (request_) request_->end();
}

async_client::callback_scope::callback_scope(detail::client_request& request) : request_(request) {
    auto& state = *request.state;
    std::lock_guard lock(state.mutex);
    entered_ = !request.cancelled;
    if (entered_) state.callbacks.push_back(std::this_thread::get_id());
}

async_client::callback_scope::~callback_scope() {
    if (!entered_) return;
    auto& state = *request_.state;
    std::lock_guard lock(state.mutex);
    state.callbacks.erase(std::find(state.callbacks.begin(), state.callbacks.end(), std::this_thread::get_id()));
    state.changed.notify_all();
}

async_client::request_context::request_context(detail::client_request& request)
    : client(*request.state), request_(request) {}

stream_callback async_client::request_context::chunks(stream_callback callback) const {
    return [&request = request_, callback = std::move(callback)](const stream_info& info) {
        callback_scope running(request);
        return running && callback(info);
    };
}

std::shared_ptr<detail::client_request> async_client::start_request() {
    auto& io_context = state_->workers.get_thread_io_context();
    std::lock_guard lock(state_->mutex);
    if (state_->closers > 0) {
        LOG_DEBUG("Async client: stopping, a request started now does not run");
        return nullptr;
    }
    auto request = std::make_shared<detail::client_request>(state_, io_context.get_executor());
    request->position = state_->requests.insert(state_->requests.end(), request);
    ++state_->active;
    return request;
}

boost::asio::io_context::executor_type async_client::executor_of(const detail::client_request& request) {
    return request.executor;
}

boost::asio::cancellation_slot async_client::slot_of(detail::client_request& request) {
    return request.signal.slot();
}

void async_client::spawned(const std::shared_ptr<detail::client_request>& request) {
    bool cancel;
    {
        std::lock_guard lock(request->state->mutex);
        request->spawned = true;
        cancel = request->cancelled;
    }
    if (cancel) detail::client_request::cancel(request);
}

bool async_client::cancelled(detail::client_request& request) {
    std::lock_guard lock(request.state->mutex);
    return request.cancelled;
}

void async_client::close_requests(bool final) {
    auto& state = *state_;
    // The requests this call waits for (any cancelled before too, e.g. from a worker, which did
    // not wait for them), and those it cancels: one not spawned yet is cancelled by spawned()
    std::vector<std::shared_ptr<detail::client_request>> ending;
    std::vector<std::shared_ptr<detail::client_request>> cancelling;
    {
        std::lock_guard lock(state.mutex);
        ++state.closers;
        for (const auto& request : state.requests) {
            if (request->left_behind) continue;
            ending.push_back(request);
            if (request->cancelled) continue;
            request->cancelled = true;
            if (request->spawned) cancelling.push_back(request);
        }
        // Release wait() and wait_for(): the client no longer runs
        state.changed.notify_all();
    }
    for (const auto& request : cancelling) detail::client_request::cancel(request);

    // No request calls its callback from now on, so those running only end. They are waited for
    // unless stop() runs on a worker: the client lives on, and two callbacks stopping it at once
    // would wait for each other. The cancelled requests end then (their worker may have been
    // running a callback), unless this is one of the workers: those of its thread could not run
    // meanwhile, and those of other threads may use connections of this one
    bool on_worker = state.workers.in_worker_thread();
    std::unique_lock lock(state.mutex);
    if (final || !on_worker) state.changed.wait(lock, [&] { return !state.callbacks_elsewhere(); });
    if (!on_worker && state.workers.running()) {
        auto has_ended = [](const auto& request) { return request->ended.load(); };
        state.changed.wait_for(lock, detail::end_grace, [&] {
            return std::all_of(ending.begin(), ending.end(), has_ended);
        });
        for (const auto& request : ending) {
            if (!has_ended(request)) request->left_behind = true;
        }
    }

    state.active = 0;
    if (!final) --state.closers;
    state.changed.notify_all();
}

void async_client::track_request_start() {
    std::lock_guard lock(state_->mutex);
    ++state_->active;
}

void async_client::track_request_end() {
    std::lock_guard lock(state_->mutex);
    if (state_->active > 0 && --state_->active == 0) state_->changed.notify_all();
}

size_t async_client::pending_requests() const {
    return state_->active.load();
}

bool async_client::stop() {
    LOG_DEBUG("Stopping HTTP async client");
    bool result = running_.exchange(false);
    close_requests(false);
    // The idle connections belong to the io_contexts of the workers, which may go away
    clear_connections();
    return result;
}

void async_client::wait() {
    std::unique_lock lock(state_->mutex);
    state_->changed.wait(lock, [this] { return state_->active == 0 || !running_; });
}

bool async_client::wait_for(std::chrono::milliseconds timeout) {
    std::unique_lock lock(state_->mutex);
    bool completed = state_->changed.wait_for(lock, timeout, [this] { return state_->active == 0 || !running_; });
    return completed && state_->active == 0;
}

} // namespace thinger::http

#include "async_client.hpp"
#include "../../util/logger.hpp"
#include <algorithm>
#include <list>
#include <vector>

namespace thinger::http {

namespace detail {

namespace {

    // How long stop() and the destructor wait for the cancelled requests to end, before closing
    // what remains of them: the cancellation reaches every operation of a request at once,
    // unless one ignores it
    constexpr auto cancel_grace = std::chrono::seconds{1};

    // The resumptions of requests the caller thread is running, innermost first
    struct running_slice {
        const request_scope* scope;
        const running_slice* outer;
    };
    thread_local const running_slice* current_slice = nullptr;

}

// What the requests of a client share with it. The requests keep it (their executors and
// completions hold it), so it outlives the client while any of them is pending.
//
// The requests are spawned in epochs: stop() closes the current one and opens the next. Each
// request runs on a gated_executor of its epoch, through which every resumption of its coroutine
// passes: once the epoch is closed, they are dropped, and the coroutine frames are destroyed
// without running any of their code
class request_scope {
public:
    std::mutex mutex;
    std::condition_variable changed;

    // Guarded by the mutex: the requests in flight, of every epoch, the epoch of those started
    // now, and how many calls of stop() or the destructor run (or the destructor ran), which
    // start none
    std::list<std::shared_ptr<client_request>> requests;
    uint64_t epoch = 0;
    unsigned closers = 0;
    // The requests of the current epoch in flight (and those the application tracks), written
    // under the mutex
    std::atomic<size_t> active{0};

    // The resumptions of the requests of an epoch below this one are dropped
    std::atomic<uint64_t> open_from{0};

    // Whether a resumption of a request of `epoch` runs: counted, as running, if it does
    bool enter(uint64_t request_epoch) {
        executing_.fetch_add(1);
        if (request_epoch >= open_from.load()) return true;
        leave();
        return false;
    }

    void leave() {
        executing_.fetch_sub(1);
        if (slice_waiters_.load() > 0) {
            std::lock_guard lock(mutex);
            changed.notify_all();
        }
    }

    // Wait until no resumption runs but those of the caller thread (it may be running one)
    void wait_slices() {
        size_t own = 0;
        for (auto* slice = current_slice; slice; slice = slice->outer) {
            if (slice->scope == this) ++own;
        }
        std::unique_lock lock(mutex);
        slice_waiters_.fetch_add(1);
        changed.wait(lock, [&] { return executing_.load() == own; });
        slice_waiters_.fetch_sub(1);
    }

private:
    // Resumptions running now, on any thread, and the threads waiting for them to end
    std::atomic<size_t> executing_{0};
    std::atomic<size_t> slice_waiters_{0};
};

// A request in flight: where it runs, what cancels it, and its place among the requests
class client_request {
public:
    client_request(std::shared_ptr<request_scope> scope, boost::asio::io_context::executor_type executor,
                   uint64_t epoch)
        : scope(std::move(scope)), executor(executor), epoch(epoch) {}

    std::shared_ptr<request_scope> scope;
    boost::asio::io_context::executor_type executor;
    uint64_t epoch;
    // Connected by co_spawn to the coroutine; emitted on its thread
    boost::asio::cancellation_signal signal;
    std::list<std::shared_ptr<client_request>>::iterator position;
    // Guarded by the mutex of the scope: its coroutine is spawned (the signal may be emitted),
    // or it was cancelled before
    bool spawned = false;
    bool cancel_pending = false;
    // Set on its thread when it ends
    std::atomic<bool> ended{false};

    // Cancel it, on its thread: whatever it waits for ends with operation_aborted
    static void cancel(const std::shared_ptr<client_request>& request) {
        boost::asio::dispatch(request->executor, [request] {
            if (!request->ended) request->signal.emit(boost::asio::cancellation_type::terminal);
        });
    }

    void end() {
        ended = true;
        std::lock_guard lock(scope->mutex);
        if (epoch == scope->epoch && scope->active > 0) --scope->active;
        scope->requests.erase(position);
        scope->changed.notify_all();
    }
};

// A resumption of a request, run only while its epoch is open (dropped otherwise)
template<typename Function>
class gated_call {
public:
    gated_call(std::shared_ptr<request_scope> scope, uint64_t epoch, Function function)
        : scope_(std::move(scope)), epoch_(epoch), function_(std::move(function)) {}

    void operator()() {
        if (!scope_->enter(epoch_)) return;
        running_slice slice{scope_.get(), current_slice};
        current_slice = &slice;
        struct on_exit {
            request_scope& scope;
            const running_slice& slice;
            ~on_exit() {
                current_slice = slice.outer;
                scope.leave();
            }
        } exit{*scope_, slice};
        std::move(function_)();
    }

private:
    std::shared_ptr<request_scope> scope_;
    uint64_t epoch_;
    Function function_;
};

// The executor of the requests of an epoch: that of their worker, gated (see request_scope)
template<typename Inner>
class gated_executor {
public:
    gated_executor(Inner inner, std::shared_ptr<request_scope> scope, uint64_t epoch) noexcept
        : inner_(std::move(inner)), scope_(std::move(scope)), epoch_(epoch) {}

    const Inner& inner() const noexcept { return inner_; }

    template<typename Property>
    auto query(const Property& property) const noexcept
        -> decltype(boost::asio::query(std::declval<const Inner&>(), property)) {
        return boost::asio::query(inner_, property);
    }

    template<typename Property>
    auto require(const Property& property) const
        -> gated_executor<std::decay_t<decltype(boost::asio::require(std::declval<const Inner&>(), property))>> {
        return {boost::asio::require(inner_, property), scope_, epoch_};
    }

    template<typename Function>
    void execute(Function&& function) const {
        inner_.execute(gated_call<std::decay_t<Function>>(scope_, epoch_, std::forward<Function>(function)));
    }

    friend bool operator==(const gated_executor& a, const gated_executor& b) noexcept {
        return a.inner_ == b.inner_ && a.scope_ == b.scope_ && a.epoch_ == b.epoch_;
    }
    friend bool operator!=(const gated_executor& a, const gated_executor& b) noexcept { return !(a == b); }

private:
    Inner inner_;
    std::shared_ptr<request_scope> scope_;
    uint64_t epoch_;
};

using request_executor = gated_executor<boost::asio::io_context::executor_type>;

boost::asio::io_context* request_io_context(const boost::asio::any_io_executor& executor) {
    if (auto* gated = executor.target<request_executor>()) return &gated->inner().context();
    return nullptr;
}

} // namespace detail

async_client::async_client(asio::workers& workers)
    : workers_(workers), scope_(std::make_shared<detail::request_scope>()) {
    LOG_DEBUG("Created HTTP async client");
}

async_client::~async_client() {
    LOG_DEBUG("Destroying HTTP async client");
    running_ = false;
    close_requests(true);
    // While the io_contexts of the connections live: those given back to the pool after stop()
    // too
    clear_connections();
}

async_client::request_end::request_end(std::shared_ptr<detail::client_request> request) noexcept
    : request_(std::move(request)) {}

async_client::request_end::~request_end() {
    if (request_) request_->end();
}

std::shared_ptr<detail::client_request> async_client::start_request() {
    auto& io_context = get_io_context();
    std::lock_guard lock(scope_->mutex);
    if (scope_->closers > 0) {
        LOG_DEBUG("Async client: stopping, a request started now does not run");
        return nullptr;
    }
    auto request = std::make_shared<detail::client_request>(scope_, io_context.get_executor(), scope_->epoch);
    request->position = scope_->requests.insert(scope_->requests.end(), request);
    ++scope_->active;
    return request;
}

boost::asio::any_io_executor async_client::executor_of(const detail::client_request& request) {
    return detail::request_executor(request.executor, request.scope, request.epoch);
}

boost::asio::cancellation_slot async_client::slot_of(detail::client_request& request) {
    return request.signal.slot();
}

void async_client::spawned(const std::shared_ptr<detail::client_request>& request) {
    bool cancel;
    {
        std::lock_guard lock(request->scope->mutex);
        request->spawned = true;
        cancel = request->cancel_pending;
    }
    if (cancel) detail::client_request::cancel(request);
}

boost::asio::any_io_executor async_client::websocket_executor(const boost::asio::any_io_executor& executor) {
    if (auto* gated = executor.target<detail::request_executor>()) return gated->inner();
    return executor;
}

void async_client::close_requests(bool final) {
    auto& scope = *scope_;
    uint64_t epoch;
    std::vector<std::shared_ptr<detail::client_request>> cancelled;
    {
        std::lock_guard lock(scope.mutex);
        ++scope.closers;
        epoch = scope.epoch;
        for (const auto& request : scope.requests) {
            if (request->spawned) {
                cancelled.push_back(request);
            } else {
                request->cancel_pending = true;
            }
        }
        // Release wait() and wait_for(): the client no longer runs
        scope.changed.notify_all();
    }
    for (const auto& request : cancelled) detail::client_request::cancel(request);

    // Let the cancelled requests end (one that completed meanwhile may still call its
    // callback), unless this is one of the workers: the requests of its thread could not run
    // meanwhile, and those of other threads may use connections of this one
    bool on_worker = workers_.in_worker_thread();
    if (!on_worker && workers_.running()) {
        std::unique_lock lock(scope.mutex);
        scope.changed.wait_for(lock, detail::cancel_grace, [&] {
            return std::none_of(scope.requests.begin(), scope.requests.end(),
                                [&](const auto& request) { return request->epoch == epoch; });
        });
    }

    // Close the epoch: what remains of its requests never runs. Then the resumptions running
    // now on other threads end (a callback), unless stop() runs on a worker: the client lives
    // on, and two callbacks stopping it at once would wait for each other
    {
        std::lock_guard lock(scope.mutex);
        if (scope.open_from < epoch + 1) scope.open_from = epoch + 1;
    }
    if (final || !on_worker) scope.wait_slices();

    std::lock_guard lock(scope.mutex);
    if (scope.epoch == epoch) {
        ++scope.epoch;
        scope.active = 0;
    }
    if (!final) --scope.closers;
    scope.changed.notify_all();
}

void async_client::track_request_start() {
    std::lock_guard lock(scope_->mutex);
    ++scope_->active;
}

void async_client::track_request_end() {
    std::lock_guard lock(scope_->mutex);
    if (scope_->active > 0 && --scope_->active == 0) scope_->changed.notify_all();
}

size_t async_client::pending_requests() const {
    return scope_->active.load();
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
    std::unique_lock lock(scope_->mutex);
    scope_->changed.wait(lock, [this] { return scope_->active == 0 || !running_; });
}

bool async_client::wait_for(std::chrono::milliseconds timeout) {
    std::unique_lock lock(scope_->mutex);
    bool completed = scope_->changed.wait_for(lock, timeout, [this] { return scope_->active == 0 || !running_; });
    return completed && scope_->active == 0;
}

} // namespace thinger::http

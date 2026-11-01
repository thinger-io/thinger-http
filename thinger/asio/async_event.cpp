#include "async_event.hpp"
#include <boost/asio/append.hpp>
#include <boost/asio/execution_context.hpp>
#include <boost/asio/post.hpp>
#include <mutex>
#include <utility>
#include <vector>

namespace thinger::asio {

// Ends a wait whose cancellation slot was emitted. It stays in the slot until the waiter's next
// operation replaces it, so it acts once: a second emission finds nothing to cancel
struct async_event::canceller {
    async_event* event;
    std::list<waiter>::iterator it;

    void operator()(boost::asio::cancellation_type type) {
        if (type == boost::asio::cancellation_type::none || event == nullptr) return;
        auto* cancelled = std::exchange(event, nullptr);
        cancelled->complete(it, boost::asio::error::operation_aborted);
    }
};

// The events of one io_context. Its shutdown (the io_context destroyed) destroys the waits still
// pending on them, unresumed, as asio destroys the pending operations of a timer: a waiter parked
// here is no operation of the io_context, and a coroutine waiting on an event of an object it
// keeps alive (a connection) would otherwise never be freed
class async_event::registry : public boost::asio::execution_context::service {
public:
    static inline boost::asio::execution_context::id id;

    explicit registry(boost::asio::execution_context& context) : service(context) {}

    void add(async_event& event) {
        std::lock_guard lock(mutex_);
        event.next_ = head_;
        if (head_ != nullptr) head_->prev_ = &event;
        head_ = &event;
        event.linked_ = true;
    }

    void remove(async_event& event) {
        std::lock_guard lock(mutex_);
        if (event.prev_ != nullptr) event.prev_->next_ = event.next_;
        else head_ = event.next_;
        if (event.next_ != nullptr) event.next_->prev_ = event.prev_;
        event.prev_ = event.next_ = nullptr;
        event.linked_ = false;
    }

    void shutdown() override {
        std::vector<waiter> pending;
        {
            std::lock_guard lock(mutex_);
            for (auto* event = std::exchange(head_, nullptr); event != nullptr;) {
                for (auto& waiter : event->waiters_) {
                    if (waiter.slot.is_connected()) waiter.slot.clear();
                    pending.push_back(std::move(waiter));
                }
                event->waiters_.clear();
                // Without waits, it leaves the registry
                event->linked_ = false;
                event->prev_ = nullptr;
                event = std::exchange(event->next_, nullptr);
            }
        }
        // Destroyed here, out of the lock: a coroutine destroyed may own events of this
        // io_context, which are no longer linked
    }

private:
    std::mutex mutex_;
    async_event* head_ = nullptr;
};

async_event::async_event(boost::asio::any_io_executor executor)
    : executor_(std::move(executor)) {}

async_event::~async_event() {
    // The last wait completed leaves the registry
    complete_all(boost::asio::error::operation_aborted);
}

void async_event::set() {
    set_ = true;
    complete_all({});
}

void async_event::reset() {
    set_ = false;
}

bool async_event::is_set() const {
    return set_;
}

awaitable<bool> async_event::wait() {
    if (set_) co_return true;
    auto [ec] = co_await boost::asio::async_initiate<decltype(use_nothrow_awaitable), void(boost::system::error_code)>(
        [this](auto handler) { add_waiter(std::move(handler)); }, use_nothrow_awaitable);
    // The event may be gone (destroyed with this wait pending): only the result is used
    co_return !ec;
}

void async_event::add_waiter(boost::asio::any_completion_handler<void(boost::system::error_code)> handler) {
    auto slot = boost::asio::get_associated_cancellation_slot(handler);
    auto work = boost::asio::prefer(boost::asio::get_associated_executor(handler, executor_),
                                    boost::asio::execution::outstanding_work.tracked);
    auto it = waiters_.insert(waiters_.end(), waiter{std::move(handler), std::move(work), slot});
    if (slot.is_connected()) slot.emplace<canceller>(this, it);
    if (!linked_) {
        if (registry_ == nullptr) {
            registry_ = &boost::asio::use_service<registry>(boost::asio::query(executor_, boost::asio::execution::context));
        }
        registry_->add(*this);
    }
}

void async_event::complete(std::list<waiter>::iterator it, boost::system::error_code ec) {
    auto completed = std::move(*it);
    waiters_.erase(it);
    if (waiters_.empty() && linked_) registry_->remove(*this);
    // Posted, then run on the waiter's executor: it never resumes inside set() or the destructor
    boost::asio::post(executor_, boost::asio::append(std::move(completed.handler), ec));
}

void async_event::complete_all(boost::system::error_code ec) {
    while (!waiters_.empty()) {
        // Its canceller would act on an event that no longer knows the wait
        if (waiters_.front().slot.is_connected()) waiters_.front().slot.clear();
        complete(waiters_.begin(), ec);
    }
}

}

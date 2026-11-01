#ifndef THINGER_ASIO_ASYNC_EVENT_HPP
#define THINGER_ASIO_ASYNC_EVENT_HPP

#include <list>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include "../util/types.hpp"

namespace thinger::asio {

    // A signal coroutines wait on: set() wakes every waiter, and the event stays set (a wait
    // returns at once) until reset(). Signals use it, never a timer set to the end of time
    // and cancelled.
    //
    // It belongs to its executor, as the object that owns it: set(), reset() and wait() are
    // called there (an owner signalled from another thread dispatches to it first). Waiters
    // resume on their own executor, always posted, never inside set(). A pending wait keeps
    // its executor's io_context running, as a timer does. Destroying the event ends the
    // pending waits, which then return false. Destroying its io_context with waits pending
    // destroys them, unresumed, as it destroys a pending timer's: a coroutine waiting on an
    // event of an object it keeps alive is freed with the io_context. Like a timer, an event
    // does not outlive its io_context
    class async_event {
    public:
        explicit async_event(boost::asio::any_io_executor executor);
        ~async_event();

        async_event(const async_event&) = delete;
        async_event& operator=(const async_event&) = delete;

        // Wake every waiter; the event stays set until reset()
        void set();
        void reset();
        bool is_set() const;

        // true once the event is set (at once if it is set already); false if the wait was
        // cancelled through its cancellation slot (e.g. by `||` or with_timeout()) or the event
        // destroyed. A cancelled wait leaves the event as it was
        awaitable<bool> wait();

    private:
        struct waiter {
            boost::asio::any_completion_handler<void(boost::system::error_code)> handler;
            boost::asio::any_completion_executor work;   // keeps the waiter's io_context running
            boost::asio::cancellation_slot slot;
        };

        struct canceller;
        class registry;

        void add_waiter(boost::asio::any_completion_handler<void(boost::system::error_code)> handler);
        // Post the waiter's completion and forget it (its cancellation handler, if any, is left
        // to the caller)
        void complete(std::list<waiter>::iterator it, boost::system::error_code ec);
        void complete_all(boost::system::error_code ec);

        boost::asio::any_io_executor executor_;
        std::list<waiter> waiters_;
        bool set_ = false;
        // The events of the io_context with waits pending, linked: its shutdown destroys those
        // waits. Linked by the first wait parked, unlinked once none is left: an event set
        // before it is waited, or never waited, never touches the registry (resolved then)
        bool linked_ = false;
        registry* registry_ = nullptr;
        async_event* prev_ = nullptr;
        async_event* next_ = nullptr;
    };

}

#endif

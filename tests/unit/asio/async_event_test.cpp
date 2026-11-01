#include <catch2/catch_test_macros.hpp>
#include <thinger/asio/async_event.hpp>
#include <thinger/asio/with_timeout.hpp>
#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

// async_event: a signal coroutines wait on. set() wakes every waiter and the event stays set
// until reset(); a wait cancelled (its slot, `||`, with_timeout()) or whose event is destroyed
// returns false, and leaves the event as it was

using namespace thinger;
using namespace std::chrono_literals;

namespace {

    // Wait on `event`, storing the result of the wait in `result`
    void spawn_wait(boost::asio::io_context& ioc, asio::async_event& event, std::optional<bool>& result) {
        co_spawn(ioc, event.wait(), [&result](std::exception_ptr error, bool woken) {
            REQUIRE_FALSE(error);
            result = woken;
        });
    }

    // The same, with a cancellation slot
    void spawn_wait(boost::asio::io_context& ioc, asio::async_event& event, std::optional<bool>& result,
                    boost::asio::cancellation_signal& signal) {
        co_spawn(ioc, event.wait(), boost::asio::bind_cancellation_slot(signal.slot(),
            [&result](std::exception_ptr error, bool woken) {
                REQUIRE_FALSE(error);
                result = woken;
            }));
    }

    // An object owning an event, and a wait on it that keeps the object alive (a connection
    // and its own coroutine)
    struct event_owner {
        explicit event_owner(boost::asio::any_io_executor executor) : event(std::move(executor)) {}
        asio::async_event event;
    };

    awaitable<void> wait_holding(std::shared_ptr<event_owner> self) {
        co_await self->event.wait();
    }

}

TEST_CASE("async_event wakes a waiter on set()", "[asio][async_event]") {
    boost::asio::io_context ioc;
    asio::async_event event(ioc.get_executor());
    std::optional<bool> result;

    spawn_wait(ioc, event, result);
    ioc.poll();
    REQUIRE_FALSE(result);
    REQUIRE_FALSE(event.is_set());

    // From another coroutine, later
    co_spawn(ioc, [&]() -> awaitable<void> {
        boost::asio::steady_timer timer(ioc, 10ms);
        co_await timer.async_wait(use_awaitable);
        event.set();
    }, detached);
    ioc.run();

    REQUIRE(result == true);
    REQUIRE(event.is_set());
}

TEST_CASE("async_event wakes every waiter with one set()", "[asio][async_event]") {
    boost::asio::io_context ioc;
    asio::async_event event(ioc.get_executor());
    std::optional<bool> results[3];

    for (auto& result : results) spawn_wait(ioc, event, result);
    ioc.poll();
    for (auto& result : results) REQUIRE_FALSE(result);

    event.set();
    // Waiters resume posted, never inside set()
    for (auto& result : results) REQUIRE_FALSE(result);
    ioc.run();
    for (auto& result : results) REQUIRE(result == true);
}

TEST_CASE("async_event stays set until reset()", "[asio][async_event]") {
    boost::asio::io_context ioc;
    asio::async_event event(ioc.get_executor());
    event.set();
    REQUIRE(event.is_set());

    SECTION("a wait on a set event returns true without suspending") {
        bool posted_ran = false;
        std::optional<bool> ran_before_wait_returned;
        std::optional<bool> result;
        co_spawn(ioc, [&]() -> awaitable<void> {
            boost::asio::post(ioc, [&] { posted_ran = true; });
            result = co_await event.wait();
            // Had the wait suspended, the handler posted before it would have run
            ran_before_wait_returned = posted_ran;
        }, detached);
        ioc.run();
        REQUIRE(result == true);
        REQUIRE(ran_before_wait_returned == false);
    }

    SECTION("after reset() a wait suspends until the next set()") {
        event.reset();
        REQUIRE_FALSE(event.is_set());
        std::optional<bool> result;
        spawn_wait(ioc, event, result);
        ioc.poll();
        REQUIRE_FALSE(result);

        event.set();
        ioc.run();
        REQUIRE(result == true);
    }

    SECTION("set() repeated wakes each waiter once") {
        event.reset();
        int woken = 0;
        for (int i = 0; i < 2; ++i) {
            co_spawn(ioc, [&]() -> awaitable<void> {
                if (co_await event.wait()) ++woken;
            }, detached);
        }
        ioc.poll();
        event.set();
        event.set();
        ioc.run();
        event.set();
        ioc.run();
        REQUIRE(woken == 2);
    }
}

TEST_CASE("async_event wait cancelled returns false", "[asio][async_event]") {
    boost::asio::io_context ioc;
    asio::async_event event(ioc.get_executor());

    SECTION("through its cancellation slot") {
        boost::asio::cancellation_signal signal;
        std::optional<bool> cancelled;
        std::optional<bool> other;
        spawn_wait(ioc, event, cancelled, signal);
        spawn_wait(ioc, event, other);
        ioc.poll();

        signal.emit(boost::asio::cancellation_type::terminal);
        // A second emission finds nothing to cancel
        signal.emit(boost::asio::cancellation_type::terminal);
        ioc.poll();
        REQUIRE(cancelled == false);
        REQUIRE_FALSE(event.is_set());
        REQUIRE_FALSE(other);

        // The other waiters still wake
        event.set();
        ioc.run();
        REQUIRE(cancelled == false);
        REQUIRE(other == true);
    }

    SECTION("by with_timeout()") {
        std::optional<std::optional<bool>> result;
        co_spawn(ioc, [&]() -> awaitable<void> {
            result = co_await asio::with_timeout(event.wait(), 20ms);
        }, detached);
        ioc.run();
        REQUIRE(result.has_value());
        REQUIRE_FALSE(result->has_value());
        REQUIRE_FALSE(event.is_set());
    }

    SECTION("by ||, which returns the other operation") {
        std::optional<size_t> winner;
        co_spawn(ioc, [&]() -> awaitable<void> {
            boost::asio::steady_timer timer(ioc, 20ms);
            auto result = co_await (event.wait() || timer.async_wait(use_nothrow_awaitable));
            winner = result.index();
        }, detached);
        ioc.run();
        REQUIRE(winner == 1);
        REQUIRE_FALSE(event.is_set());
    }

    SECTION("set() then a cancellation: the wait was woken") {
        boost::asio::cancellation_signal signal;
        std::optional<bool> result;
        spawn_wait(ioc, event, result, signal);
        ioc.poll();
        event.set();
        signal.emit(boost::asio::cancellation_type::terminal);
        ioc.run();
        REQUIRE(result == true);
    }

    SECTION("a cancellation then set(): the wait was cancelled, the event is set") {
        boost::asio::cancellation_signal signal;
        std::optional<bool> result;
        spawn_wait(ioc, event, result, signal);
        ioc.poll();
        signal.emit(boost::asio::cancellation_type::terminal);
        event.set();
        ioc.run();
        REQUIRE(result == false);
        REQUIRE(event.is_set());
    }
}

TEST_CASE("async_event destroyed with waits pending ends them", "[asio][async_event]") {
    boost::asio::io_context ioc;
    auto owned = std::make_unique<asio::async_event>(ioc.get_executor());
    boost::asio::cancellation_signal signal;
    std::optional<bool> first;
    std::optional<bool> second;
    spawn_wait(ioc, *owned, first, signal);
    spawn_wait(ioc, *owned, second);
    ioc.poll();

    owned.reset();
    // Its canceller is gone with it: emitting the slot touches nothing
    signal.emit(boost::asio::cancellation_type::terminal);
    ioc.run();
    REQUIRE(first == false);
    REQUIRE(second == false);
}

TEST_CASE("async_event wait pending keeps the io_context running", "[asio][async_event]") {
    boost::asio::io_context ioc;
    asio::async_event event(ioc.get_executor());
    std::optional<bool> result;
    spawn_wait(ioc, event, result);

    ioc.run_for(30ms);
    REQUIRE_FALSE(ioc.stopped());
    REQUIRE_FALSE(result);

    boost::asio::post(ioc, [&] { event.set(); });
    ioc.run();
    REQUIRE(result == true);
}

TEST_CASE("async_event waits pending are destroyed with their io_context", "[asio][async_event]") {
    // As a timer's: the coroutine parked on the event of the object it keeps alive is destroyed,
    // unresumed, and frees the object (it leaked: a cycle no one would break)
    std::weak_ptr<event_owner> watched;
    {
        boost::asio::io_context ioc;
        auto owner = std::make_shared<event_owner>(ioc.get_executor());
        watched = owner;
        co_spawn(ioc, wait_holding(std::move(owner)), detached);
        ioc.poll();
        REQUIRE_FALSE(watched.expired());
    }
    REQUIRE(watched.expired());
}

TEST_CASE("async_event waits pending are destroyed with their io_context, whatever waits came before",
          "[asio][async_event]") {
    // An event joins the waits its io_context destroys only while one is parked: one set before
    // its wait, and one whose waits ended, still has the next wait destroyed with the io_context
    std::weak_ptr<event_owner> watched;
    {
        boost::asio::io_context ioc;
        auto never_waited = std::make_shared<event_owner>(ioc.get_executor());
        never_waited->event.set();
        auto owner = std::make_shared<event_owner>(ioc.get_executor());
        watched = owner;

        // Woken, then cancelled: no wait left (the io_context stops each time it is out of work)
        std::optional<bool> woken;
        std::optional<bool> cancelled;
        boost::asio::cancellation_signal signal;
        spawn_wait(ioc, owner->event, woken);
        ioc.poll();
        owner->event.set();
        ioc.poll();
        owner->event.reset();
        ioc.restart();
        spawn_wait(ioc, owner->event, cancelled, signal);
        ioc.poll();
        signal.emit(boost::asio::cancellation_type::terminal);
        ioc.poll();
        REQUIRE(woken == true);
        REQUIRE(cancelled == false);

        // A wait set at once parks nothing
        std::optional<bool> at_once;
        ioc.restart();
        spawn_wait(ioc, never_waited->event, at_once);
        ioc.poll();
        REQUIRE(at_once == true);

        ioc.restart();
        co_spawn(ioc, wait_holding(std::move(owner)), detached);
        ioc.poll();
        REQUIRE_FALSE(watched.expired());
    }
    REQUIRE(watched.expired());
}

TEST_CASE("async_event waits on an io_context run by several threads", "[asio][async_event]") {
    // Events of one io_context, each on its strand, waited and woken from four threads at once:
    // their waits join and leave the waits the io_context destroys concurrently
    boost::asio::io_context ioc;
    constexpr int events = 8;
    constexpr int rounds = 200;
    std::atomic<int> woken{0};
    for (int e = 0; e < events; ++e) {
        auto strand = boost::asio::make_strand(ioc);
        co_spawn(strand, [strand, &woken]() -> awaitable<void> {
            for (int i = 0; i < rounds; ++i) {
                asio::async_event event(strand);
                boost::asio::post(strand, [&event] { event.set(); });
                if (co_await event.wait()) ++woken;
            }
        }, detached);
    }
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back([&ioc] { ioc.run(); });
    for (auto& thread : threads) thread.join();
    REQUIRE(woken == events * rounds);
}

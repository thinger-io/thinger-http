#include <catch2/catch_test_macros.hpp>
#include <thinger/asio/watchdog.hpp>
#include <boost/asio.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

// watchdog: a deadline that activity moves, one timer checked lazily on expiry. Its callback
// runs once per expiry, never after cancel() or its destruction, never before the deadline or
// the hold, and it never keeps its owner alive. Deadlines of tens of milliseconds

using namespace thinger;
using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;

namespace {

    // The times on_expired was called at
    struct expiries {
        std::vector<clock_type::time_point> at;
        std::function<void()> recorder() { return [this] { at.push_back(clock_type::now()); }; }
    };

    // Run `action` on the io_context `after` from now
    void run_after(boost::asio::io_context& ioc, clock_type::duration after, std::function<void()> action) {
        auto timer = std::make_shared<boost::asio::steady_timer>(ioc, after);
        timer->async_wait([timer, action = std::move(action)](const boost::system::error_code& ec) {
            if (!ec) action();
        });
    }

}

TEST_CASE("watchdog expires once at its deadline", "[asio][watchdog]") {
    boost::asio::io_context ioc;
    expiries expired;
    asio::watchdog watchdog(ioc.get_executor(), expired.recorder());

    auto start = clock_type::now();
    watchdog.expires_at(start + 50ms);
    ioc.run();

    REQUIRE(expired.at.size() == 1);
    REQUIRE(expired.at[0] >= start + 50ms);
}

TEST_CASE("watchdog deadline moved later is not reached at the first one", "[asio][watchdog]") {
    boost::asio::io_context ioc;
    expiries expired;
    asio::watchdog watchdog(ioc.get_executor(), expired.recorder());

    auto start = clock_type::now();
    watchdog.expires_at(start + 30ms);
    // Activity before it expires, twice
    run_after(ioc, 10ms, [&] { watchdog.expires_at(start + 60ms); });
    run_after(ioc, 20ms, [&] { watchdog.expires_at(start + 90ms); });
    ioc.run();

    REQUIRE(expired.at.size() == 1);
    REQUIRE(expired.at[0] >= start + 90ms);
}

TEST_CASE("watchdog deadline moved earlier re-arms it", "[asio][watchdog]") {
    boost::asio::io_context ioc;
    expiries expired;
    asio::watchdog watchdog(ioc.get_executor(), expired.recorder());

    auto start = clock_type::now();
    watchdog.expires_at(start + 2s);
    watchdog.expires_at(start + 30ms);
    ioc.run();

    REQUIRE(expired.at.size() == 1);
    REQUIRE(expired.at[0] >= start + 30ms);
    REQUIRE(expired.at[0] < start + 1s);
}

TEST_CASE("watchdog does not expire before its hold", "[asio][watchdog]") {
    boost::asio::io_context ioc;
    expiries expired;
    asio::watchdog watchdog(ioc.get_executor(), expired.recorder());
    auto start = clock_type::now();

    SECTION("a hold later than the deadline") {
        watchdog.expires_at(start + 20ms);
        watchdog.hold_until(start + 80ms);
        ioc.run();
        REQUIRE(expired.at.size() == 1);
        REQUIRE(expired.at[0] >= start + 80ms);
    }

    SECTION("an earlier hold does not replace a later one") {
        watchdog.hold_until(start + 80ms);
        watchdog.hold_until(start + 10ms);
        watchdog.expires_at(start + 20ms);
        ioc.run();
        REQUIRE(expired.at.size() == 1);
        REQUIRE(expired.at[0] >= start + 80ms);
    }

    SECTION("a hold arms nothing by itself") {
        watchdog.hold_until(start + 10ms);
        ioc.run();
        REQUIRE(expired.at.empty());
    }
}

TEST_CASE("watchdog deadline in the past expires on a next run of the executor", "[asio][watchdog]") {
    boost::asio::io_context ioc;
    expiries expired;
    asio::watchdog watchdog(ioc.get_executor(), expired.recorder());

    watchdog.expires_at(clock_type::now() - 1s);
    REQUIRE(expired.at.empty());
    ioc.run();
    REQUIRE(expired.at.size() == 1);
}

TEST_CASE("watchdog armed again from its callback expires again", "[asio][watchdog]") {
    boost::asio::io_context ioc;
    int calls = 0;
    std::unique_ptr<asio::watchdog> watchdog;
    watchdog = std::make_unique<asio::watchdog>(ioc.get_executor(), [&] {
        // A keep-alive: one interval after each expiry
        if (++calls < 3) watchdog->expires_at(clock_type::now() + 10ms);
    });
    watchdog->expires_at(clock_type::now() + 10ms);
    ioc.run();
    REQUIRE(calls == 3);
}

TEST_CASE("watchdog cancelled never expires", "[asio][watchdog]") {
    boost::asio::io_context ioc;
    expiries expired;

    SECTION("cancelled before its deadline") {
        asio::watchdog watchdog(ioc.get_executor(), expired.recorder());
        watchdog.expires_at(clock_type::now() + 20ms);
        run_after(ioc, 5ms, [&] { watchdog.cancel(); });
        ioc.run();
        REQUIRE(expired.at.empty());
    }

    SECTION("cancelled and armed again expires at the new deadline only") {
        asio::watchdog watchdog(ioc.get_executor(), expired.recorder());
        auto start = clock_type::now();
        watchdog.expires_at(start + 20ms);
        watchdog.cancel();
        watchdog.expires_at(start + 40ms);
        ioc.run();
        REQUIRE(expired.at.size() == 1);
        REQUIRE(expired.at[0] >= start + 40ms);
    }

    SECTION("cancelled once its timer expired, its handler queued") {
        // The watchdog runs on a strand kept busy past its deadline: the timer expires and its
        // handler waits in the strand's queue behind the handler that cancels it
        auto strand = boost::asio::make_strand(ioc);
        asio::watchdog watchdog(strand, expired.recorder());
        boost::asio::post(strand, [&] {
            watchdog.expires_at(clock_type::now() + 10ms);
            std::this_thread::sleep_for(100ms);
            watchdog.cancel();
        });
        std::thread other([&] { ioc.run(); });
        ioc.run();
        other.join();
        REQUIRE(expired.at.empty());
    }
}

TEST_CASE("watchdog never calls a dead owner", "[asio][watchdog]") {
    boost::asio::io_context ioc;

    struct owner : std::enable_shared_from_this<owner> {
        int& calls;
        std::shared_ptr<owner>* holder;
        std::unique_ptr<asio::watchdog> watchdog;

        owner(int& calls, std::shared_ptr<owner>* holder) : calls(calls), holder(holder) {}

        void start(boost::asio::io_context& ioc, clock_type::time_point deadline) {
            // The callback holds the owner weakly
            watchdog = std::make_unique<asio::watchdog>(ioc.get_executor(), [weak = weak_from_this()] {
                auto self = weak.lock();
                if (!self) return;
                ++self->calls;
                // Release the owner (and its watchdog) from the callback
                if (self->holder) self->holder->reset();
            });
            watchdog->expires_at(deadline);
        }
    };

    int calls = 0;

    SECTION("the owner destroyed before the deadline") {
        auto subject = std::make_shared<owner>(calls, nullptr);
        subject->start(ioc, clock_type::now() + 20ms);
        std::weak_ptr<owner> weak = subject;
        subject.reset();
        REQUIRE(weak.expired());
        ioc.run();
        REQUIRE(calls == 0);
    }

    SECTION("the owner destroyed by its callback") {
        std::shared_ptr<owner> subject;
        subject = std::make_shared<owner>(calls, &subject);
        subject->start(ioc, clock_type::now() + 20ms);
        std::weak_ptr<owner> weak = subject;
        // Only `subject` holds it: the watchdog does not keep it alive
        REQUIRE(weak.use_count() == 1);
        ioc.run();
        REQUIRE(calls == 1);
        REQUIRE(weak.expired());
    }
}

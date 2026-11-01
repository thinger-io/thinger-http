#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/flow_control.hpp>
#include <boost/asio.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

// write_flow's write deadline: it measures a stall, the time a pending write makes no progress,
// not the time a write takes. On by default (60 s); limits set during a write apply from the
// call. Deadlines of tens of milliseconds

using namespace thinger;
using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;

namespace {

    // The times the write timed out at
    struct timeouts {
        std::vector<clock_type::time_point> at;
        sync_callback<void()> recorder() { return [this] { at.push_back(clock_type::now()); }; }
    };

    // Run `action` on the io_context `after` from now
    void run_after(boost::asio::io_context& ioc, clock_type::duration after, std::function<void()> action) {
        auto timer = std::make_shared<boost::asio::steady_timer>(ioc, after);
        timer->async_wait([timer, action = std::move(action)](const boost::system::error_code& ec) {
            if (!ec) action();
        });
    }

}

TEST_CASE("write_limits have a 60 s write deadline by default", "[flow][write_flow]") {
    STATIC_REQUIRE(http::write_limits{}.write_timeout == 60s);
    // limits that do not name it keep it, as the README's examples
    STATIC_REQUIRE(http::write_limits{.max_messages = 0, .high_watermark = 1 << 20}.write_timeout == 60s);

    boost::asio::io_context ioc;
    http::write_flow flow(ioc.get_executor());
    REQUIRE(flow.limits().write_timeout == 60s);
}

TEST_CASE("write_flow times out a write without progress at its deadline", "[flow][write_flow]") {
    boost::asio::io_context ioc;
    http::write_flow flow(ioc.get_executor(), {.write_timeout = 50ms});
    timeouts timed_out;

    auto start = clock_type::now();
    flow.write_started(timed_out.recorder());
    ioc.run();

    REQUIRE(timed_out.at.size() == 1);
    REQUIRE(timed_out.at[0] >= start + 50ms);
}

TEST_CASE("write_flow progress moves the write deadline", "[flow][write_flow]") {
    boost::asio::io_context ioc;
    http::write_flow flow(ioc.get_executor(), {.write_timeout = 50ms});
    timeouts timed_out;

    auto start = clock_type::now();
    flow.write_started(timed_out.recorder());
    // a slow write that keeps going: 30 ms between progress, under the deadline, past it in all
    clock_type::time_point last;
    for (auto at : {30ms, 60ms, 90ms}) {
        run_after(ioc, at, [&] {
            flow.write_progressed();
            last = clock_type::now();
        });
    }
    ioc.run();

    REQUIRE(timed_out.at.size() == 1);
    REQUIRE(timed_out.at[0] >= start + 140ms);
    REQUIRE(timed_out.at[0] >= last + 50ms);
}

TEST_CASE("write_flow finished write does not time out", "[flow][write_flow]") {
    boost::asio::io_context ioc;
    http::write_flow flow(ioc.get_executor(), {.write_timeout = 30ms});
    timeouts timed_out;

    flow.write_started(timed_out.recorder());
    run_after(ioc, 10ms, [&] { flow.write_finished(); });
    run_after(ioc, 80ms, [] {});
    ioc.run();

    REQUIRE(timed_out.at.empty());
}

TEST_CASE("write_flow write after a finished one times out from its own start", "[flow][write_flow]") {
    // the first write's deadline, left on the timer, neither times the second out early nor
    // keeps it from timing out
    boost::asio::io_context ioc;
    http::write_flow flow(ioc.get_executor(), {.write_timeout = 50ms});
    timeouts first, second;

    flow.write_started(first.recorder());
    clock_type::time_point start;
    run_after(ioc, 10ms, [&] { flow.write_finished(); });
    run_after(ioc, 30ms, [&] {
        start = clock_type::now();
        flow.write_started(second.recorder());
    });
    ioc.run();

    REQUIRE(first.at.empty());
    REQUIRE(second.at.size() == 1);
    REQUIRE(second.at[0] >= start + 50ms);
    REQUIRE(second.at[0] < start + 50ms + 1s);
}

TEST_CASE("write_flow without a write deadline never times out", "[flow][write_flow]") {
    boost::asio::io_context ioc;
    http::write_flow flow(ioc.get_executor(), {.write_timeout = 0ms});
    timeouts timed_out;

    flow.write_started(timed_out.recorder());
    run_after(ioc, 80ms, [] {});
    ioc.run();

    REQUIRE(timed_out.at.empty());
}

TEST_CASE("write_flow limits set during a pending write apply from the call", "[flow][write_flow]") {
    boost::asio::io_context ioc;
    timeouts timed_out;

    SECTION("0 cancels the deadline, and a later one counts from its call") {
        http::write_flow flow(ioc.get_executor(), {.write_timeout = 50ms});
        auto start = clock_type::now();
        flow.write_started(timed_out.recorder());
        run_after(ioc, 20ms, [&] { flow.set_limits({.write_timeout = 0ms}); });
        clock_type::time_point set;
        run_after(ioc, 100ms, [&] {
            set = clock_type::now();
            flow.set_limits({.write_timeout = 50ms});
        });
        ioc.run();

        REQUIRE(timed_out.at.size() == 1);
        REQUIRE(timed_out.at[0] >= start + 150ms);
        REQUIRE(timed_out.at[0] >= set + 50ms);
    }

    SECTION("a shorter one already exceeded times out at once") {
        http::write_flow flow(ioc.get_executor(), {.write_timeout = 10s});
        flow.write_started(timed_out.recorder());
        clock_type::time_point set;
        run_after(ioc, 60ms, [&] {
            set = clock_type::now();
            flow.set_limits({.write_timeout = 30ms});
        });
        ioc.run();

        REQUIRE(timed_out.at.size() == 1);
        REQUIRE(timed_out.at[0] >= set);
        REQUIRE(timed_out.at[0] < set + 30ms);
    }

    SECTION("a longer one moves it later") {
        http::write_flow flow(ioc.get_executor(), {.write_timeout = 30ms});
        auto start = clock_type::now();
        flow.write_started(timed_out.recorder());
        run_after(ioc, 10ms, [&] { flow.set_limits({.write_timeout = 100ms}); });
        ioc.run();

        REQUIRE(timed_out.at.size() == 1);
        REQUIRE(timed_out.at[0] >= start + 100ms);
    }
}

// The peer taking bytes the socket holds is progress too, seen by the checks of a stalled write:
// a slow reader whose socket takes nothing for a while, as the system wakes the writer only once
// much of the buffer is free. The first check comes once the socket took nothing for the
// deadline: a backlog that does not fall within 1/BACKLOG_WATCH of it more times the write out;
// one that falls is checked every 1/BACKLOG_CHECKS of it from then. A write that progresses
// within its deadline is never checked, so it costs no timer operation
TEST_CASE("write_flow counts the peer taking bytes as progress", "[flow][write_flow]") {
    boost::asio::io_context ioc;
    http::write_flow flow(ioc.get_executor(), {.write_timeout = 80ms});
    constexpr auto watch = 80ms / http::write_flow::BACKLOG_WATCH;
    timeouts timed_out;
    size_t backlog = 1000;
    size_t checks = 0;
    // A reader taking bytes until `reading_until`: the last check that saw the backlog fall
    auto reading_until = clock_type::time_point::min();
    clock_type::time_point last_fall{};
    flow.set_send_backlog([&] {
        ++checks;
        auto now = clock_type::now();
        if (now < reading_until) {
            backlog -= 10;
            last_fall = now;
        }
        return backlog;
    });
    auto start = clock_type::now();

    SECTION("a backlog falling until 200 ms") {
        reading_until = start + 200ms;
        flow.write_started(timed_out.recorder());
        ioc.run();

        REQUIRE(timed_out.at.size() == 1);
        // it fell past the first deadline, and the write timed out a deadline after it stopped
        REQUIRE(last_fall >= start + 80ms + watch);
        REQUIRE(timed_out.at[0] >= last_fall + 80ms);
    }

    SECTION("a backlog that does not fall") {
        flow.write_started(timed_out.recorder());
        size_t checks_before_deadline = 0;
        run_after(ioc, 60ms, [&] { checks_before_deadline = checks; });
        ioc.run();

        REQUIRE(checks_before_deadline == 0);
        REQUIRE(timed_out.at.size() == 1);
        REQUIRE(timed_out.at[0] >= start + 80ms + watch);
        REQUIRE(timed_out.at[0] < start + 80ms + watch + 40ms);
        REQUIRE(checks == 2);
    }

    SECTION("a write that progresses within its deadline is not checked") {
        flow.write_started(timed_out.recorder());
        for (auto at = 30ms; at <= 180ms; at += 30ms) {
            run_after(ioc, at, [&] { flow.write_progressed(); });
        }
        run_after(ioc, 200ms, [&] { flow.write_finished(); });
        ioc.run();

        REQUIRE(timed_out.at.empty());
        REQUIRE(checks == 0);
    }

    SECTION("a write that finishes is not checked") {
        flow.write_started(timed_out.recorder());
        flow.write_finished();
        run_after(ioc, 100ms, [] {});
        ioc.run();

        REQUIRE(timed_out.at.empty());
        REQUIRE(checks == 0);
    }
}

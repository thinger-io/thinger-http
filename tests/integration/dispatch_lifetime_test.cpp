// An application destroyed with requests dispatched in memory in flight: the handlers are
// interrupted and not waited for, nothing starts afterwards, and each dispatch ends once, at once,
// without touching the application (the tests run under ASan and TSan)
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <thinger/http/server/http_application.hpp>
#include <thinger/http/server/pool_server.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/response_observer.hpp>
#include <thinger/http/server/server_standalone.hpp>

#include "shutdown_test_helpers.hpp"
#include "../fixtures/handler_probe.hpp"
#include "../fixtures/running_workers.hpp"
#include "../fixtures/watchdog.hpp"

#include <boost/asio.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <utility>

using namespace thinger;
using namespace std::chrono_literals;
using http::test::timed;
using http::test::watchdog;
using shutdown_test::eventually;

namespace {

// An io_context running on a thread of its own, as the caller of a dispatch
class caller_thread {
public:
    caller_thread() : guard_(boost::asio::make_work_guard(ioc_)), thread_([this] { ioc_.run(); }) {}

    ~caller_thread() {
        ioc_.stop();
        thread_.join();
    }

    boost::asio::any_io_executor executor() { return ioc_.get_executor(); }

private:
    boost::asio::io_context ioc_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard_;
    std::thread thread_;
};

// What a dispatch ended with, for its caller, and how many times. Written on the thread of the
// caller, and read by the test once `results` tells so
struct dispatch_outcome {
    std::atomic<int> results{0};
    std::exception_ptr error;
    std::shared_ptr<http::http_response> response;

    void set(std::exception_ptr failure, std::shared_ptr<http::http_response> answer) {
        error = std::move(failure);
        response = std::move(answer);
        ++results;
    }

    bool ended_within(std::chrono::milliseconds limit) const {
        return eventually([this] { return results.load() > 0; }, limit);
    }

    // It failed with what a caller cancelling the dispatch gets
    bool aborted() const {
        if (!error) return false;
        try {
            std::rethrow_exception(error);
        } catch (const boost::system::system_error& e) {
            return e.code() == boost::asio::error::operation_aborted;
        } catch (...) {
            return false;
        }
    }

    bool answered(http::http_response::status status) const {
        return !error && response && response->get_status() == status;
    }

    // It got what the destruction of the application gives a dispatch without a response
    bool destroyed(bool awaited) const {
        return awaited ? aborted() : answered(http::http_response::status::service_unavailable);
    }
};

std::shared_ptr<http::http_request> get_request(const std::string& path) {
    return http::http_request::create_http_request(http::method::GET, "http://localhost" + path);
}

http::dispatch_options within(std::chrono::milliseconds timeout) {
    http::dispatch_options options;
    options.timeout = timeout;
    return options;
}

// co_await application.dispatch(...) on `executor`
void dispatch_awaited(const boost::asio::any_io_executor& executor, http::http_application& application,
                      dispatch_outcome& outcome, const std::string& path, std::chrono::milliseconds timeout = 10s) {
    co_spawn(executor, application.dispatch(get_request(path), within(timeout)),
             [&outcome](std::exception_ptr error, std::shared_ptr<http::http_response> response) {
                 outcome.set(std::move(error), std::move(response));
             });
}

// application.dispatch(executor, ..., callback)
void dispatch_with_callback(const boost::asio::any_io_executor& executor, http::http_application& application,
                            dispatch_outcome& outcome, const std::string& path,
                            std::chrono::milliseconds timeout = 10s) {
    application.dispatch(executor, get_request(path),
                         [&outcome](std::shared_ptr<http::http_response> response) {
                             outcome.set(nullptr, std::move(response));
                         },
                         within(timeout));
}

// One way or the other
void dispatch_either(bool awaited, const boost::asio::any_io_executor& executor, http::http_application& application,
                     dispatch_outcome& outcome, const std::string& path, std::chrono::milliseconds timeout = 10s) {
    if (awaited) {
        dispatch_awaited(executor, application, outcome, path, timeout);
    } else {
        dispatch_with_callback(executor, application, outcome, path, timeout);
    }
}

// An application counting what runs for its route /count: two middlewares that suspend, a handler
// that waits until interrupted, and the response hook
struct counted_application {
    std::atomic<int> middlewares{0};
    std::atomic<int> handlers{0};
    std::atomic<int> hooked{0};
    std::unique_ptr<http::http_application> application = std::make_unique<http::http_application>();

    counted_application() {
        for (int middleware = 0; middleware < 2; ++middleware) {
            application->use([this](http::request&, http::response&) -> awaitable<bool> {
                ++middlewares;
                co_await boost::asio::post(co_await boost::asio::this_coro::executor, use_awaitable);
                co_return true;
            });
        }
        application->get("/count", [this](http::response& res) -> awaitable<void> {
            ++handlers;
            co_await http::test::interrupted_wait(10s);
            res.send("done");
        });
        application->on_response([this](const http::request&, const http::response_info&) { ++hooked; });
    }
};

// Never runs anything inline, not even from a thread running its context: each step of a dispatch
// is a handler of its own for poll_one()
struct stepping_executor {
    boost::asio::io_context* context;

    template<typename Function>
    void execute(Function&& function) const {
        boost::asio::post(*context, std::forward<Function>(function));
    }
    boost::asio::execution_context& query(boost::asio::execution::context_t) const noexcept { return *context; }
    static constexpr boost::asio::execution::blocking_t query(boost::asio::execution::blocking_t) noexcept {
        return boost::asio::execution::blocking.never;
    }
    stepping_executor require(boost::asio::execution::blocking_t::never_t) const { return *this; }
    bool operator==(const stepping_executor&) const noexcept = default;
};

// A server is an application: destroying one with a dispatch in flight, on `executor`, goes as for
// any other. It listens, without a connection
void destroy_with_dispatch_in_flight(std::unique_ptr<http::server_base> server,
                                     const boost::asio::any_io_executor& executor, http::test::handler_probe& probe,
                                     dispatch_outcome& outcome, bool awaited) {
    http::test::waiting_route(*server, "/wait", probe, 10s);
    http::test::transport_listener listener(*server, http::test::transport::tcp);

    dispatch_either(awaited, executor, *server, outcome, "/wait");
    REQUIRE(probe.wait_started());

    REQUIRE(timed([&] { server.reset(); }) < 1s);

    REQUIRE(probe.wait_ended(1s));
    CHECK(probe.interrupted);
    CHECK(probe.reason == http::cancel_reason::shutdown);
    REQUIRE(outcome.ended_within(1s));
    CHECK(outcome.destroyed(awaited));
    std::this_thread::sleep_for(20ms);
    CHECK(outcome.results == 1);
}

} // namespace

// --- A dispatch waiting in its handler (US1) ---

TEST_CASE("Destroying an application interrupts the handler of a dispatch, without waiting for it",
          "[dispatch][lifetime]") {
    watchdog guard(30s);
    http::test::handler_probe probe;
    dispatch_outcome outcome;
    caller_thread caller;
    auto application = std::make_unique<http::http_application>();
    // A route not declaring cancel_on, on a coroutine of its own or under a route timeout
    auto& route = http::test::waiting_route(*application, "/wait", probe, 10s);
    auto route_timeout = GENERATE(false, true);
    INFO((route_timeout ? "under a route timeout" : "without a route timeout"));
    if (route_timeout) route.timeout(8s);

    dispatch_awaited(caller.executor(), *application, outcome, "/wait");
    REQUIRE(probe.wait_started());

    REQUIRE(timed([&] { application.reset(); }) < 1s);

    REQUIRE(probe.wait_ended(1s));
    CHECK(probe.interrupted);
    CHECK(probe.reason == http::cancel_reason::shutdown);
}

TEST_CASE("Destroying an application ends an awaited dispatch with operation_aborted", "[dispatch][lifetime]") {
    watchdog guard(30s);
    http::test::handler_probe probe;
    dispatch_outcome outcome;
    caller_thread caller;
    auto application = std::make_unique<http::http_application>();
    http::test::waiting_route(*application, "/wait", probe, 10s);

    dispatch_awaited(caller.executor(), *application, outcome, "/wait");
    REQUIRE(probe.wait_started());

    application.reset();

    REQUIRE(outcome.ended_within(1s));
    CHECK(outcome.aborted());
    // The answer of the interrupted handler is dropped: a single result
    REQUIRE(probe.wait_ended(1s));
    std::this_thread::sleep_for(20ms);
    CHECK(outcome.results == 1);
}

TEST_CASE("Destroying an application answers a dispatch with a callback with 503", "[dispatch][lifetime]") {
    watchdog guard(30s);
    http::test::handler_probe probe;
    dispatch_outcome outcome;
    caller_thread caller;
    auto application = std::make_unique<http::http_application>();
    http::test::waiting_route(*application, "/wait", probe, 10s);

    dispatch_with_callback(caller.executor(), *application, outcome, "/wait");
    REQUIRE(probe.wait_started());

    application.reset();

    REQUIRE(outcome.ended_within(1s));
    CHECK(outcome.answered(http::http_response::status::service_unavailable));
    REQUIRE(probe.wait_ended(1s));
    std::this_thread::sleep_for(20ms);
    CHECK(outcome.results == 1);
}

TEST_CASE("Destroying an application delivers a response already complete", "[dispatch][lifetime]") {
    watchdog guard(30s);
    std::atomic<bool> answered{false};
    std::atomic<int> hooked{0};
    dispatch_outcome outcome;
    // Run by hand: the caller is not resumed until the application is gone
    boost::asio::io_context context;
    auto application = std::make_unique<http::http_application>();
    // Answering after a suspension: the caller is waiting for the response by then
    application->get("/now", [&answered](http::response& res) -> awaitable<void> {
        co_await boost::asio::post(co_await boost::asio::this_coro::executor, use_awaitable);
        res.send("complete");
        answered = true;
    });
    application->on_response([&hooked](const http::request&, const http::response_info&) { ++hooked; });
    auto awaited = GENERATE(true, false);
    INFO((awaited ? "awaited" : "with a callback"));

    dispatch_either(awaited, context.get_executor(), *application, outcome, "/now");
    while (!answered) context.run_one();
    REQUIRE(outcome.results == 0);

    application.reset();
    context.run();

    REQUIRE(outcome.results == 1);
    REQUIRE(outcome.answered(http::http_response::status::ok));
    CHECK(outcome.response->get_content() == "complete");
    CHECK(hooked == 1);
}

TEST_CASE("The response hook is told once about a dispatch interrupted by the destruction", "[dispatch][lifetime]") {
    watchdog guard(30s);
    http::test::handler_probe probe;
    std::atomic<int> hooked{0};
    std::atomic<http::response_outcome> told{http::response_outcome::completed};
    dispatch_outcome outcome;
    caller_thread caller;
    auto application = std::make_unique<http::http_application>();
    http::test::waiting_route(*application, "/wait", probe, 10s);
    application->on_response([&](const http::request&, const http::response_info& info) {
        told = info.outcome;
        ++hooked;
    });
    auto awaited = GENERATE(true, false);
    INFO((awaited ? "awaited" : "with a callback"));

    dispatch_either(awaited, caller.executor(), *application, outcome, "/wait");
    REQUIRE(probe.wait_started());

    application.reset();

    REQUIRE(outcome.ended_within(1s));
    REQUIRE(probe.wait_ended(1s));
    REQUIRE(eventually([&] { return hooked > 0; }, 1s));
    std::this_thread::sleep_for(20ms);
    CHECK(hooked == 1);
    CHECK(told == http::response_outcome::aborted);
}

// --- A handler outliving the answer of its dispatch (US2) ---

TEST_CASE("Destroying an application interrupts a handler outliving the timeout of its dispatch",
          "[dispatch][lifetime]") {
    watchdog guard(30s);
    http::test::handler_probe probe;
    dispatch_outcome outcome;
    caller_thread caller;
    auto application = std::make_unique<http::http_application>();
    // Answers once interrupted. A route not declaring cancel_on, on a coroutine of its own or
    // under a route timeout
    auto& route = http::test::waiting_route(*application, "/wait", probe, 10s);
    auto route_timeout = GENERATE(false, true);
    if (route_timeout) route.timeout(8s);
    auto awaited = GENERATE(true, false);
    INFO((route_timeout ? "under a route timeout" : "without a route timeout") << ", "
         << (awaited ? "awaited" : "with a callback"));

    dispatch_either(awaited, caller.executor(), *application, outcome, "/wait", 100ms);
    REQUIRE(outcome.ended_within(3s));
    REQUIRE(outcome.answered(http::http_response::status::gateway_timeout));
    REQUIRE_FALSE(probe.ended);

    REQUIRE(timed([&] { application.reset(); }) < 1s);

    REQUIRE(probe.wait_ended(1s));
    CHECK(probe.interrupted);
    // Its scope keeps the first reason
    CHECK(probe.reason == http::cancel_reason::timeout);
    // Its answer is dropped: the caller has the 504, and nothing more
    std::this_thread::sleep_for(20ms);
    CHECK(outcome.results == 1);
    CHECK(outcome.answered(http::http_response::status::gateway_timeout));
}

TEST_CASE("Destroying an application interrupts a handler outliving the timeout of its route", "[dispatch][lifetime]") {
    watchdog guard(30s);
    http::test::handler_probe probe;
    dispatch_outcome outcome;
    caller_thread caller;
    auto application = std::make_unique<http::http_application>();
    http::test::waiting_route(*application, "/wait", probe, 10s).timeout(100ms);

    dispatch_awaited(caller.executor(), *application, outcome, "/wait");
    REQUIRE(outcome.ended_within(3s));
    REQUIRE(outcome.answered(http::http_response::status::gateway_timeout));
    REQUIRE_FALSE(probe.ended);

    application.reset();

    REQUIRE(probe.wait_ended(1s));
    CHECK(probe.interrupted);
    CHECK(probe.reason == http::cancel_reason::timeout);
}

TEST_CASE("Destroying an application interrupts a handler whose caller cancelled the dispatch", "[dispatch][lifetime]") {
    watchdog guard(30s);
    http::test::handler_probe probe;
    dispatch_outcome outcome;
    caller_thread caller;
    boost::asio::cancellation_signal signal;
    auto application = std::make_unique<http::http_application>();
    http::test::waiting_route(*application, "/wait", probe, 10s);

    co_spawn(caller.executor(), application->dispatch(get_request("/wait"), within(10s)),
             boost::asio::bind_cancellation_slot(signal.slot(),
                 [&outcome](std::exception_ptr error, std::shared_ptr<http::http_response> response) {
                     outcome.set(std::move(error), std::move(response));
                 }));
    REQUIRE(probe.wait_started());
    boost::asio::post(caller.executor(), [&signal] { signal.emit(boost::asio::cancellation_type::terminal); });
    REQUIRE(outcome.ended_within(3s));
    REQUIRE(outcome.aborted());
    REQUIRE_FALSE(probe.ended);

    application.reset();

    REQUIRE(probe.wait_ended(1s));
    CHECK(probe.interrupted);
    CHECK(probe.reason == http::cancel_reason::client_gone);
}

TEST_CASE("Destroying an application interrupts a wait bound to the scope after the timeout", "[dispatch][lifetime]") {
    watchdog guard(30s);
    http::test::handler_probe probe;
    std::atomic<bool> first_interrupted{false};
    dispatch_outcome outcome;
    caller_thread caller;
    auto application = std::make_unique<http::http_application>();
    // Abandons what it awaits at the timeout, and goes on with another wait bound to the scope
    application->get("/wait", [&](http::request& req, http::response& res) -> awaitable<void> {
        first_interrupted = co_await http::test::interrupted_wait(10s, req.cancellation_slot());
        probe.start();
        bool interrupted = co_await http::test::interrupted_wait(10s, req.cancellation_slot());
        probe.end(req, interrupted);
        res.send("late");
    });

    dispatch_awaited(caller.executor(), *application, outcome, "/wait", 100ms);
    REQUIRE(outcome.ended_within(3s));
    REQUIRE(probe.wait_started());
    REQUIRE(first_interrupted);
    REQUIRE_FALSE(probe.ended);

    application.reset();

    REQUIRE(probe.wait_ended(1s));
    CHECK(probe.interrupted);
    CHECK(probe.reason == http::cancel_reason::timeout);
}

// --- A dispatch not started yet (US3) ---

TEST_CASE("A dispatch with a callback not started when its application is destroyed runs nothing and gets a 503",
          "[dispatch][lifetime]") {
    watchdog guard(30s);
    counted_application counted;
    dispatch_outcome outcome;
    // Not running yet
    boost::asio::io_context context;

    dispatch_with_callback(context.get_executor(), *counted.application, outcome, "/count");
    counted.application.reset();
    REQUIRE(outcome.results == 0);
    context.run();

    REQUIRE(outcome.results == 1);
    CHECK(outcome.answered(http::http_response::status::service_unavailable));
    CHECK(counted.middlewares == 0);
    CHECK(counted.handlers == 0);
    CHECK(counted.hooked == 0);
}

TEST_CASE("A dispatch awaited after its application is destroyed runs nothing and fails with operation_aborted",
          "[dispatch][lifetime]") {
    watchdog guard(30s);
    counted_application counted;
    dispatch_outcome outcome;
    boost::asio::io_context context;

    auto pending = counted.application->dispatch(get_request("/count"), within(10s));
    counted.application.reset();
    co_spawn(context, std::move(pending), [&outcome](std::exception_ptr error, std::shared_ptr<http::http_response> response) {
        outcome.set(std::move(error), std::move(response));
    });
    context.run();

    REQUIRE(outcome.results == 1);
    CHECK(outcome.aborted());
    CHECK(counted.middlewares == 0);
    CHECK(counted.handlers == 0);
    CHECK(counted.hooked == 0);
}

TEST_CASE("Destroying an application at any step of a dispatch: nothing starts afterwards, and it ends once",
          "[dispatch][lifetime]") {
    watchdog guard(30s);
    auto awaited = GENERATE(true, false);
    // Up to the first step in which the handler runs
    for (int steps = 0;; ++steps) {
        INFO((awaited ? "awaited" : "with a callback") << ", destroyed after " << steps << " steps");
        REQUIRE(steps < 100);
        counted_application counted;
        dispatch_outcome outcome;
        boost::asio::io_context context;

        dispatch_either(awaited, stepping_executor{&context}, *counted.application, outcome, "/count");
        for (int step = 0; step < steps; ++step) context.poll_one();
        int middlewares = counted.middlewares;
        int handlers = counted.handlers;

        counted.application.reset();
        context.run();

        REQUIRE(outcome.results == 1);
        CHECK(outcome.destroyed(awaited));
        CHECK(counted.middlewares == middlewares);
        CHECK(counted.handlers == handlers);
        CHECK(counted.hooked <= 1);
        if (middlewares > 0) CHECK(counted.hooked == 1);
        if (handlers > 0) break;
    }
}

// --- Destroyed from a handler, and with dispatches on several threads (US4) ---

TEST_CASE("Destroying an application from the handler of a dispatch", "[dispatch][lifetime]") {
    watchdog guard(30s);
    http::test::handler_probe probe;
    std::atomic<long long> took{-1};
    std::atomic<int> hooked{0};
    dispatch_outcome outcome;
    caller_thread caller;
    auto application = std::make_unique<http::http_application>();
    // With the caller still on its way to wait, as the handler may run inline in its step, or
    // waiting already
    auto caller_waiting = GENERATE(false, true);
    auto& route = application->get("/quit", [&, caller_waiting](http::request& req, http::response& res) -> awaitable<void> {
        if (caller_waiting) co_await boost::asio::post(co_await boost::asio::this_coro::executor, use_awaitable);
        probe.start();
        took = timed([&] { application.reset(); }).count();
        // Its coroutine is cancelled already: the wait does not start, and asio throws
        boost::system::error_code ec;
        try {
            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 10s);
            co_await timer.async_wait(use_awaitable);
        } catch (const boost::system::system_error& e) {
            ec = e.code();
        }
        probe.end(req, ec == boost::asio::error::operation_aborted);
        res.send("late");
    });
    application->on_response([&hooked](const http::request&, const http::response_info&) { ++hooked; });
    // On a coroutine of its own, or under a route timeout
    auto route_timeout = GENERATE(false, true);
    if (route_timeout) route.timeout(8s);
    auto awaited = GENERATE(true, false);
    INFO((caller_waiting ? "the caller waiting" : "the caller on its way") << ", "
         << (route_timeout ? "under a route timeout" : "without a route timeout") << ", "
         << (awaited ? "awaited" : "with a callback"));

    dispatch_either(awaited, caller.executor(), *application, outcome, "/quit");
    REQUIRE(probe.wait_started());

    // The destructor returned, the step of the handler went on, and its next wait was interrupted
    REQUIRE(probe.wait_ended(1s));
    CHECK(took >= 0);
    CHECK(took < 1000);
    CHECK(probe.interrupted);
    CHECK(probe.reason == http::cancel_reason::shutdown);

    // Its answer is dropped: a single result
    REQUIRE(outcome.ended_within(1s));
    CHECK(outcome.destroyed(awaited));
    REQUIRE(eventually([&] { return hooked > 0; }, 1s));
    std::this_thread::sleep_for(20ms);
    CHECK(outcome.results == 1);
    CHECK(hooked == 1);
}

TEST_CASE("Destroying an application from a synchronous handler of a dispatch", "[dispatch][lifetime]") {
    watchdog guard(30s);
    std::atomic<long long> took{-1};
    dispatch_outcome outcome;
    caller_thread caller;
    auto application = std::make_unique<http::http_application>();
    auto sends = GENERATE(false, true);
    application->get("/quit", [&, sends](http::response& res) {
        took = timed([&] { application.reset(); }).count();
        if (sends) res.send("bye");
    });
    auto awaited = GENERATE(true, false);
    INFO((sends ? "the handler sends after it" : "the handler returns after it") << ", "
         << (awaited ? "awaited" : "with a callback"));

    dispatch_either(awaited, caller.executor(), *application, outcome, "/quit");

    REQUIRE(outcome.ended_within(3s));
    CHECK(took >= 0);
    CHECK(took < 1000);
    CHECK(outcome.destroyed(awaited));
    std::this_thread::sleep_for(20ms);
    CHECK(outcome.results == 1);
}

TEST_CASE("Destroying an application with dispatches on several threads: each one ends once", "[dispatch][lifetime]") {
    watchdog guard(60s);
    constexpr int dispatches = 32;
    const std::array<std::string, 3> paths{"/wait", "/now", "/soon"};
    for (int round = 0; round < 20; ++round) {
        // At this many dispatches made, with those still to come on the threads of the pool
        const int destroyed_at = 8 + round;
        INFO("destroyed at " << destroyed_at << " dispatches");
        std::array<dispatch_outcome, dispatches> outcomes;
        std::atomic<int> made{0};
        std::atomic<int> waiting{0};
        std::atomic<int> interrupted{0};
        boost::asio::thread_pool pool(4);
        // Calling dispatch() on an application destroyed is an error of the caller
        std::shared_mutex application_mutex;
        auto application = std::make_unique<http::http_application>();
        application->get("/wait", [&](http::response& res) -> awaitable<void> {
            ++waiting;
            if (co_await http::test::interrupted_wait(10s)) ++interrupted;
            res.send("late");
        });
        application->get("/now", [](http::response& res) { res.send("now"); });
        application->get("/soon", [](http::response& res) -> awaitable<void> {
            co_await boost::asio::post(co_await boost::asio::this_coro::executor, use_awaitable);
            res.send("soon");
        });

        // Each one a task of its own, so the threads of the pool run them mixed with what they start
        for (int i = 0; i < dispatches; ++i) {
            boost::asio::post(pool, [&, i] {
                std::shared_lock lock(application_mutex);
                if (!application) return;
                dispatch_either(i % 2 == 0, pool.get_executor(), *application, outcomes[i], paths[i % paths.size()]);
                ++made;
            });
        }
        // (under the watchdog)
        while (made < destroyed_at) std::this_thread::yield();
        {
            std::unique_lock lock(application_mutex);
            REQUIRE(timed([&] { application.reset(); }) < 1s);
        }

        // Nothing is left on the pool long before the 10 s of any wait, or of any dispatch
        REQUIRE(timed([&] { pool.join(); }) < 3s);

        int ended = 0;
        for (int i = 0; i < dispatches; ++i) {
            INFO("dispatch " << i << ", to " << paths[i % paths.size()]);
            const auto& outcome = outcomes[i];
            REQUIRE(outcome.results <= 1);
            if (outcome.results == 0) continue;
            ++ended;
            const bool awaited = i % 2 == 0;
            if (i % paths.size() == 0) {
                CHECK(outcome.destroyed(awaited));
            } else {
                CHECK((outcome.answered(http::http_response::status::ok) || outcome.destroyed(awaited)));
            }
        }
        CHECK(ended == made);
        CHECK(interrupted == waiting);
    }
}

TEST_CASE("Destroying an application while a handler of a dispatch blocks its thread does not wait for it",
          "[dispatch][lifetime]") {
    watchdog guard(30s);
    std::atomic<bool> blocking{false};
    std::atomic<bool> step_ended{false};
    dispatch_outcome outcome;
    boost::asio::thread_pool pool(4);
    auto application = std::make_unique<http::http_application>();
    const auto block = [&](http::response& res) {
        blocking = true;
        std::this_thread::sleep_for(500ms);   // synchronous: nothing interrupts it
        step_ended = true;
        res.send("late");
    };
    // At its first step, which may run inline in the step of the caller, or with the caller
    // waiting already, whom another thread wakes
    application->get("/block", [&block](http::response& res) { block(res); });
    application->get("/resumed", [&block](http::response& res) -> awaitable<void> {
        co_await boost::asio::post(co_await boost::asio::this_coro::executor, use_awaitable);
        block(res);
    });
    auto resumed = GENERATE(false, true);
    auto awaited = GENERATE(true, false);
    INFO((resumed ? "the caller waiting" : "at its first step") << ", " << (awaited ? "awaited" : "with a callback"));

    dispatch_either(awaited, pool.get_executor(), *application, outcome, resumed ? "/resumed" : "/block");
    REQUIRE(eventually([&] { return blocking.load(); }));

    auto took = timed([&] { application.reset(); });
    INFO("destroyed in " << took.count() << " ms");
    CHECK(took < 400ms);
    CHECK_FALSE(step_ended);

    REQUIRE(outcome.ended_within(1s));
    if (resumed) CHECK_FALSE(step_ended);
    CHECK(outcome.destroyed(awaited));

    // The step ends on its own, and its response is dropped: a single result
    REQUIRE(eventually([&] { return step_ended.load(); }, 2s));
    REQUIRE(timed([&] { pool.join(); }) < 3s);
    CHECK(outcome.results == 1);
    CHECK(outcome.destroyed(awaited));
}

TEST_CASE("Destroying a pool server with a dispatch in flight", "[dispatch][lifetime]") {
    watchdog guard(30s);
    http::test::handler_probe probe;
    dispatch_outcome outcome;
    http::test::running_workers workers;
    auto awaited = GENERATE(true, false);
    INFO((awaited ? "awaited" : "with a callback"));

    // On the workers of the server
    destroy_with_dispatch_in_flight(std::make_unique<http::pool_server>(workers), workers.next_executor(), probe, outcome,
                                    awaited);
}

TEST_CASE("Destroying a standalone server with a dispatch in flight", "[dispatch][lifetime]") {
    watchdog guard(30s);
    http::test::handler_probe probe;
    dispatch_outcome outcome;
    caller_thread caller;
    auto awaited = GENERATE(true, false);
    INFO((awaited ? "awaited" : "with a callback"));

    destroy_with_dispatch_in_flight(std::make_unique<http::server>(), caller.executor(), probe, outcome, awaited);
}

// --- Edge cases ---

TEST_CASE("What is sent from a copy of the response after the destruction is dropped", "[dispatch][lifetime]") {
    watchdog guard(30s);
    std::optional<http::response> kept;
    std::atomic<bool> keeping{false};
    dispatch_outcome outcome;
    caller_thread caller;
    auto application = std::make_unique<http::http_application>();
    application->get("/later", [&](http::response& res) {
        kept.emplace(res);
        keeping = true;
    });
    auto awaited = GENERATE(true, false);
    INFO((awaited ? "awaited" : "with a callback"));

    dispatch_either(awaited, caller.executor(), *application, outcome, "/later");
    REQUIRE(eventually([&] { return keeping.load(); }, 3s));

    application.reset();
    REQUIRE(outcome.ended_within(1s));

    // Another thread (this one), with the application gone
    kept->send("late");
    kept.reset();
    std::this_thread::sleep_for(20ms);
    CHECK(outcome.results == 1);
    CHECK(outcome.destroyed(awaited));
}

TEST_CASE("A response in parts not finished when the application is destroyed counts as none", "[dispatch][lifetime]") {
    watchdog guard(30s);
    http::test::handler_probe probe;
    std::atomic<bool> late_chunk{true};
    std::atomic<bool> late_end{true};
    dispatch_outcome outcome;
    caller_thread caller;
    auto application = std::make_unique<http::http_application>();
    application->get("/parts", [&](http::request& req, http::response& res) -> awaitable<void> {
        res.start_chunked("text/plain");
        res.write_chunk("started");
        probe.start();
        bool interrupted = co_await http::test::interrupted_wait(10s);
        late_chunk = res.write_chunk("late");
        late_end = res.end_chunked();
        probe.end(req, interrupted);
    });
    auto awaited = GENERATE(true, false);
    INFO((awaited ? "awaited" : "with a callback"));

    dispatch_either(awaited, caller.executor(), *application, outcome, "/parts");
    REQUIRE(probe.wait_started());

    application.reset();

    REQUIRE(outcome.ended_within(1s));
    CHECK(outcome.destroyed(awaited));
    REQUIRE(probe.wait_ended(1s));
    CHECK(probe.interrupted);
    // The rest of it is dropped
    CHECK_FALSE(late_chunk);
    CHECK_FALSE(late_end);
    std::this_thread::sleep_for(20ms);
    CHECK(outcome.results == 1);
}

TEST_CASE("A handler isolating its wait from the cancellation runs it to its end", "[dispatch][lifetime]") {
    watchdog guard(30s);
    http::test::handler_probe probe;
    std::atomic<int> hooked{0};
    dispatch_outcome outcome;
    caller_thread caller;
    auto application = std::make_unique<http::http_application>();
    constexpr auto isolated = 1s;
    application->get("/isolated", [&](http::request& req, http::response& res) -> awaitable<void> {
        probe.start();
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, isolated);
        auto [ec] = co_await timer.async_wait(
            boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, use_nothrow_awaitable));
        probe.end(req, ec == boost::asio::error::operation_aborted);
        res.send("late");
    });
    application->on_response([&hooked](const http::request&, const http::response_info&) { ++hooked; });

    dispatch_awaited(caller.executor(), *application, outcome, "/isolated");
    REQUIRE(probe.wait_started());

    REQUIRE(timed([&] { application.reset(); }) < isolated);

    // Neither the destructor nor the caller waited for it
    REQUIRE(outcome.ended_within(isolated));
    CHECK(outcome.aborted());
    CHECK_FALSE(probe.ended);

    // It goes on, against the state it keeps, and what it sends is dropped
    REQUIRE(probe.wait_ended(3s));
    CHECK_FALSE(probe.interrupted);
    CHECK(probe.ended_after(probe.start_time()) >= isolated);
    CHECK(probe.reason == http::cancel_reason::shutdown);
    REQUIRE(eventually([&] { return hooked > 0; }, 1s));
    CHECK(hooked == 1);
    CHECK(outcome.results == 1);
}

TEST_CASE("Destroying the io_context of a dispatch with requests pending", "[dispatch][lifetime]") {
    watchdog guard(30s);
    http::test::handler_probe probe;
    std::atomic<int> hooked{0};
    dispatch_outcome started;
    dispatch_outcome queued;
    auto context = std::make_unique<boost::asio::io_context>();
    auto application = std::make_unique<http::http_application>();
    http::test::waiting_route(*application, "/wait", probe, 10s);
    application->on_response([&hooked](const http::request&, const http::response_info&) { ++hooked; });
    auto context_first = GENERATE(true, false);
    INFO((context_first ? "the io_context first" : "the application first"));

    // One waiting in its handler, and one that never ran
    dispatch_awaited(context->get_executor(), *application, started, "/wait");
    while (!probe.started && context->poll_one() > 0) {}
    REQUIRE(probe.started);
    dispatch_with_callback(context->get_executor(), *application, queued, "/wait");

    if (context_first) context.reset();
    application.reset();
    context.reset();

    // Their callers are gone with the io_context; the request that started is reported once
    CHECK(started.results == 0);
    CHECK(queued.results == 0);
    CHECK(hooked == 1);
    CHECK_FALSE(probe.ended);
}

TEST_CASE("The timeout of a dispatch and the destruction of its application, at any step: a single result",
          "[dispatch][lifetime]") {
    watchdog guard(30s);
    auto awaited = GENERATE(true, false);
    // Up to the first step in which the caller has its 504
    for (int steps = 0;; ++steps) {
        INFO((awaited ? "awaited" : "with a callback") << ", destroyed after " << steps << " steps");
        REQUIRE(steps < 100);
        counted_application counted;
        dispatch_outcome outcome;
        boost::asio::io_context context;

        // Expired as soon as the caller waits
        dispatch_either(awaited, stepping_executor{&context}, *counted.application, outcome, "/count", 0ms);
        for (int step = 0; step < steps; ++step) context.poll_one();
        const bool answered_first = outcome.results > 0;

        counted.application.reset();
        // Nothing keeps it for the 10 s of the handler
        context.run();

        REQUIRE(outcome.results == 1);
        if (steps == 0) CHECK(outcome.destroyed(awaited));
        if (answered_first) {
            CHECK(outcome.answered(http::http_response::status::gateway_timeout));
            break;
        }
        CHECK((outcome.answered(http::http_response::status::gateway_timeout) || outcome.destroyed(awaited)));
        CHECK(counted.hooked <= 1);
    }
}

// On a caller of several threads: the timer and the destruction may wake it at once
TEST_CASE("The timeout of a dispatch and the destruction of its application, from two threads: a single result",
          "[dispatch][lifetime]") {
    watchdog guard(60s);
    auto awaited = GENERATE(true, false);
    for (int round = 0; round < 40; ++round) {
        // From before the timeout to after it
        const auto delay = 4ms + round * 50us;
        INFO((awaited ? "awaited" : "with a callback") << ", destroyed after " << delay.count() << " us");
        counted_application counted;
        dispatch_outcome outcome;
        boost::asio::thread_pool pool(2);

        dispatch_either(awaited, pool.get_executor(), *counted.application, outcome, "/count", 5ms);
        std::this_thread::sleep_for(delay);
        counted.application.reset();

        // Nothing is left on the pool long before the 10 s of the handler
        REQUIRE(timed([&] { pool.join(); }) < 3s);
        REQUIRE(outcome.results == 1);
        CHECK((outcome.answered(http::http_response::status::gateway_timeout) || outcome.destroyed(awaited)));
        CHECK(counted.hooked <= 1);
    }
}

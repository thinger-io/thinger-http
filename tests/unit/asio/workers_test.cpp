#include <catch2/catch_test_macros.hpp>
#include <thinger/asio/workers.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <pthread.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using thinger::asio::workers;

namespace {

    // The io_context an executor of the workers runs on
    boost::asio::io_context* context_of(const boost::asio::any_io_executor& executor) {
        auto* target = executor.target<boost::asio::io_context::executor_type>();
        return target ? &target->context() : nullptr;
    }

    // The result of `work`, run on `executor`
    template<typename Work>
    auto run_on(const boost::asio::any_io_executor& executor, Work work) {
        std::promise<decltype(work())> result;
        auto future = result.get_future();
        boost::asio::post(executor, [&] { result.set_value(work()); });
        REQUIRE(future.wait_for(5s) == std::future_status::ready);
        return future.get();
    }

    // The name of the caller thread, as the system shows it
    std::string this_thread_name() {
        char name[64] = {};
        pthread_getname_np(pthread_self(), name, sizeof(name));
        return name;
    }

    // Whether the process handles `signal` (a wait() is waiting for it)
    bool handling(int signal) {
        struct sigaction now {};
        ::sigaction(signal, nullptr, &now);
        return now.sa_handler != SIG_DFL && now.sa_handler != SIG_IGN;
    }

    template<typename Condition>
    bool eventually(Condition condition, std::chrono::milliseconds timeout = 3s) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!condition()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    // The distinct io_contexts of `calls` executors taken from the pool
    std::set<boost::asio::io_context*> pool_contexts(workers& pool, int calls) {
        std::set<boost::asio::io_context*> contexts;
        for (int i = 0; i < calls; ++i) contexts.insert(context_of(pool.next_executor()));
        return contexts;
    }

}

TEST_CASE("Workers start and stop", "[asio][workers]") {
    workers pool;
    REQUIRE_FALSE(pool.running());

    SECTION("Started and stopped") {
        REQUIRE_FALSE(pool.start(2));
        REQUIRE(pool.running());
        pool.stop();
        REQUIRE_FALSE(pool.running());
    }

    SECTION("A second start() reports it, and adds no thread") {
        REQUIRE_FALSE(pool.start(2));
        REQUIRE(pool.start(3) == boost::asio::error::already_started);
        REQUIRE(pool.running());
        REQUIRE(pool_contexts(pool, 12).size() == 2);
        pool.stop();
    }

    SECTION("No thread is an invalid argument") {
        REQUIRE(pool.start(0) == boost::system::errc::invalid_argument);
        REQUIRE_FALSE(pool.running());
    }

    SECTION("Started again after stop(), on threads of its own") {
        REQUIRE_FALSE(pool.start(2));
        auto first = pool_contexts(pool, 4);
        pool.stop();

        REQUIRE_FALSE(pool.start(2));
        auto second = pool_contexts(pool, 4);
        REQUIRE(second.size() == 2);
        for (auto* context : second) REQUIRE_FALSE(first.contains(context));
        REQUIRE(run_on(pool.next_executor(), [&] { return pool.in_worker_thread(); }));
        pool.stop();
    }

    SECTION("stop() twice") {
        REQUIRE_FALSE(pool.start(2));
        pool.stop();
        pool.stop();
        REQUIRE_FALSE(pool.running());
    }

    SECTION("stop() before start()") {
        pool.stop();
        REQUIRE_FALSE(pool.running());
    }
}

TEST_CASE("Workers that do not run hand out no executor", "[asio][workers]") {
    workers pool;

    SECTION("Never started") {}

    SECTION("Stopped") {
        REQUIRE_FALSE(pool.start(2));
        pool.stop();
    }

    // A null executor, and the io_context accessors throw: nothing divides by zero or reads a
    // table gone
    REQUIRE(!pool.next_executor());
    REQUIRE(!pool.provider()());
    REQUIRE(!pool.isolated_executor("isolated"));
    REQUIRE_THROWS_AS(pool.get_next_io_context(), std::logic_error);
    REQUIRE_THROWS_AS(pool.get_thread_io_context(), std::logic_error);
    REQUIRE_FALSE(pool.in_worker_thread());
}

TEST_CASE("Workers hand out executors round-robin", "[asio][workers]") {
    constexpr int pool_size = 4;
    workers pool;
    REQUIRE_FALSE(pool.start(pool_size));

    SECTION("One by one") {
        auto* ctx1 = context_of(pool.next_executor());
        auto* ctx2 = context_of(pool.next_executor());
        auto* ctx3 = context_of(pool.next_executor());
        auto* ctx4 = context_of(pool.next_executor());
        auto* ctx5 = context_of(pool.next_executor());
        REQUIRE(ctx1 != nullptr);
        REQUIRE(std::set{ctx1, ctx2, ctx3, ctx4}.size() == pool_size);
        REQUIRE(ctx5 == ctx1);
        // provider() is the same round-robin
        REQUIRE(context_of(pool.provider()()) == ctx2);
    }

    SECTION("2N calls from 2N threads at once: twice each") {
        std::vector<boost::asio::io_context*> taken(2 * pool_size);
        std::atomic<int> ready{0};
        std::vector<std::thread> callers;
        for (int t = 0; t < 2 * pool_size; ++t) {
            callers.emplace_back([&, t] {
                ++ready;
                while (ready < 2 * pool_size) std::this_thread::yield();
                taken[t] = context_of(pool.next_executor());
            });
        }
        for (auto& caller : callers) caller.join();

        std::map<boost::asio::io_context*, int> counts;
        for (auto* context : taken) ++counts[context];
        REQUIRE(counts.size() == pool_size);
        for (const auto& [context, count] : counts) {
            REQUIRE(context != nullptr);
            REQUIRE(count == 2);
        }
    }

    SECTION("Many calls from several threads: an even share each") {
        // Connections accepted on several threads at once (several listeners) take the next
        // executor concurrently: every context still gets its share, without a data race
        constexpr int threads = 8;
        constexpr int calls = 10000;
        std::vector<std::map<boost::asio::io_context*, int>> taken(threads);
        std::vector<std::thread> callers;
        for (int t = 0; t < threads; ++t) {
            callers.emplace_back([&, t] {
                for (int i = 0; i < calls; ++i) ++taken[t][context_of(pool.next_executor())];
            });
        }
        for (auto& caller : callers) caller.join();

        std::map<boost::asio::io_context*, int> total;
        for (const auto& counts : taken) {
            for (const auto& [context, count] : counts) total[context] += count;
        }
        REQUIRE(total.size() == pool_size);
        for (const auto& [context, count] : total) {
            REQUIRE(context != nullptr);
            REQUIRE(count == threads * calls / pool_size);
        }
    }

    pool.stop();
}

TEST_CASE("Workers run isolated executors on threads of their own", "[asio][workers]") {
    workers pool;
    REQUIRE_FALSE(pool.start(2));

    auto first = pool.isolated_executor("isolated-a");
    auto second = pool.isolated_executor("a-very-long-thread-name");
    auto* first_context = context_of(first);
    auto* second_context = context_of(second);
    auto pool_ctxs = pool_contexts(pool, 4);

    SECTION("Each, out of the pool") {
        REQUIRE(first_context != nullptr);
        REQUIRE(second_context != nullptr);
        REQUIRE(first_context != second_context);
        REQUIRE_FALSE(pool_ctxs.contains(first_context));
        REQUIRE_FALSE(pool_ctxs.contains(second_context));
    }

    SECTION("On a thread of its own, named after it, which is one of the workers") {
        auto pool_thread = run_on(pool.next_executor(), [] { return std::this_thread::get_id(); });
        auto first_thread = run_on(first, [] { return std::this_thread::get_id(); });
        auto second_thread = run_on(second, [] { return std::this_thread::get_id(); });
        REQUIRE(first_thread != second_thread);
        REQUIRE(first_thread != pool_thread);
        REQUIRE(first_thread != std::this_thread::get_id());

#if defined(__linux__) || defined(__APPLE__)
        REQUIRE(run_on(first, [] { return this_thread_name(); }) == "isolated-a");
        // the system keeps 15 characters
        REQUIRE(run_on(second, [] { return this_thread_name(); }) == "a-very-long-thr");
        REQUIRE(run_on(pool.next_executor(), [] { return this_thread_name(); }).starts_with("worker thread"));
#endif
        REQUIRE(run_on(first, [&] { return pool.in_worker_thread(); }));
        REQUIRE(run_on(second, [&] { return pool.in_worker_thread(); }));
    }

    SECTION("Asked for while the pool hands out executors") {
        std::atomic<bool> done{false};
        std::atomic<bool> null_taken{false};
        std::thread taker([&] {
            while (!done) if (!pool.next_executor()) null_taken = true;
        });
        std::vector<boost::asio::any_io_executor> isolated;
        for (int i = 0; i < 8; ++i) isolated.push_back(pool.isolated_executor("isolated-" + std::to_string(i)));
        done = true;
        taker.join();
        REQUIRE_FALSE(null_taken);
        for (const auto& executor : isolated) REQUIRE(run_on(executor, [&] { return pool.in_worker_thread(); }));
    }

    pool.stop();
}

TEST_CASE("Workers tell their own threads", "[asio][workers]") {
    workers pool;
    workers other;
    REQUIRE_FALSE(pool.start(2));
    REQUIRE_FALSE(other.start(1));

    SECTION("True on its threads, false on any other") {
        REQUIRE_FALSE(pool.in_worker_thread());
        REQUIRE(run_on(pool.next_executor(), [&] { return pool.in_worker_thread(); }));
        REQUIRE(run_on(pool.next_executor(), [&] { return pool.in_worker_thread(); }));
        REQUIRE_FALSE(run_on(pool.next_executor(), [&] { return other.in_worker_thread(); }));
        REQUIRE_FALSE(run_on(other.next_executor(), [&] { return pool.in_worker_thread(); }));

        bool foreign = true;
        std::thread([&] { foreign = pool.in_worker_thread(); }).join();
        REQUIRE_FALSE(foreign);
    }

    SECTION("False once stopped, also on its own thread") {
        std::promise<std::pair<bool, bool>> after;
        auto result = after.get_future();
        boost::asio::post(pool.next_executor(), [&] {
            bool before = pool.in_worker_thread();
            pool.stop();
            after.set_value({before, pool.in_worker_thread()});
        });
        REQUIRE(result.wait_for(5s) == std::future_status::ready);
        auto [before, stopped] = result.get();
        REQUIRE(before);
        REQUIRE_FALSE(stopped);
        REQUIRE_FALSE(pool.running());
    }

    if (pool.running()) pool.stop();
    other.stop();
}

TEST_CASE("Workers give the io_context of the caller thread", "[asio][workers]") {
    workers pool;
    REQUIRE_FALSE(pool.start(3));
    auto contexts = pool_contexts(pool, 6);

    SECTION("On one of its threads, that thread's") {
        for (int i = 0; i < 3; ++i) {
            auto executor = pool.next_executor();
            REQUIRE(run_on(executor, [&] { return &pool.get_thread_io_context(); }) == context_of(executor));
        }
        auto isolated = pool.isolated_executor("isolated");
        REQUIRE(run_on(isolated, [&] { return &pool.get_thread_io_context(); }) == context_of(isolated));
    }

    SECTION("On any other thread, always the same one of the pool") {
        auto* context = &pool.get_thread_io_context();
        REQUIRE(contexts.contains(context));
        boost::asio::io_context* foreign = nullptr;
        std::thread([&] { foreign = &pool.get_thread_io_context(); }).join();
        REQUIRE(foreign == context);
    }

    SECTION("On a thread of other workers, one of its pool") {
        workers other;
        REQUIRE_FALSE(other.start(1));
        auto* context = run_on(other.next_executor(), [&] { return &pool.get_thread_io_context(); });
        REQUIRE(contexts.contains(context));
        other.stop();
    }

    pool.stop();
}

TEST_CASE("Workers stopped from one of their threads", "[asio][workers]") {
    std::optional<workers> pool;
    pool.emplace();
    REQUIRE_FALSE(pool->start(3));

    std::promise<void> stopped;
    auto done = stopped.get_future();
    boost::asio::post(pool->next_executor(), [&] {
        pool->stop();
        // twice, from the same thread
        pool->stop();
        stopped.set_value();
    });
    REQUIRE(done.wait_for(5s) == std::future_status::ready);
    REQUIRE_FALSE(pool->running());

    SECTION("Then stopped again from another thread") {
        pool->stop();
        REQUIRE_FALSE(pool->running());
    }

    SECTION("Then started again") {
        REQUIRE_FALSE(pool->start(2));
        REQUIRE(run_on(pool->next_executor(), [&] { return pool->in_worker_thread(); }));
        pool->stop();
    }

    // the destructor joins the thread that stopped them
    pool.reset();
}

TEST_CASE("Workers destroyed while one of their threads stops them", "[asio][workers][signal]") {
    std::optional<workers> pool;
    pool.emplace();
    REQUIRE_FALSE(pool->start(2));
    auto busy = pool->next_executor();
    auto stopper = pool->next_executor();

    // The stop() joins a busy thread long after it released the wait()
    std::promise<void> busy_started;
    boost::asio::post(busy, [&] {
        busy_started.set_value();
        std::this_thread::sleep_for(200ms);
    });
    REQUIRE(busy_started.get_future().wait_for(5s) == std::future_status::ready);
    auto waiting = std::async(std::launch::async, [&] { return pool->wait({SIGUSR1}); });
    REQUIRE(eventually([] { return handling(SIGUSR1); }));
    boost::asio::post(stopper, [&] { pool->stop(); });
    REQUIRE(waiting.wait_for(3s) == std::future_status::ready);
    REQUIRE(waiting.get() == 0);

    // the destructor waits for that stop()
    pool.reset();
}

TEST_CASE("Workers destroyed while running stop", "[asio][workers]") {
    std::atomic<int> ran{0};
    std::atomic<bool> released{false};
    {
        workers pool;
        REQUIRE_FALSE(pool.start(2));
        auto isolated = pool.isolated_executor("isolated");
        REQUIRE(run_on(pool.next_executor(), [&] { return ++ran; }) == 1);
        REQUIRE(run_on(isolated, [&] { return ++ran; }) == 2);

        // the work left when they are destroyed is released, run or not
        auto guard = std::shared_ptr<void>(nullptr, [&](void*) { released = true; });
        boost::asio::post(pool.next_executor(), [guard] { std::this_thread::sleep_for(50ms); });
        boost::asio::post(pool.next_executor(), [&ran, guard] { ++ran; });
        boost::asio::post(isolated, [&ran, guard] { ++ran; });
        guard.reset();
    }
    REQUIRE(released);
    REQUIRE(ran >= 2);
}

TEST_CASE("Workers wait for a signal", "[asio][workers][signal]") {
    workers pool;
    REQUIRE_FALSE(pool.start(2));

    SECTION("It returns the signal, and the workers keep running") {
        auto waiting = std::async(std::launch::async, [&] { return pool.wait({SIGUSR1}); });
        REQUIRE(eventually([] { return handling(SIGUSR1); }));
        REQUIRE(waiting.wait_for(100ms) == std::future_status::timeout);

        ::kill(::getpid(), SIGUSR1);
        REQUIRE(waiting.wait_for(3s) == std::future_status::ready);
        REQUIRE(waiting.get() == SIGUSR1);
        REQUIRE(pool.running());
        REQUIRE(run_on(pool.next_executor(), [&] { return pool.in_worker_thread(); }));
        // the signal takes its default action again
        REQUIRE_FALSE(handling(SIGUSR1));
    }

    SECTION("Twice in a row, a signal each") {
        // Between the two, the signals take their default action: the second is raised once
        // the second wait() handles it
        std::promise<int> first;
        auto first_received = first.get_future();
        auto second = std::async(std::launch::async, [&] {
            first.set_value(pool.wait({SIGUSR1, SIGUSR2}));
            return pool.wait({SIGUSR1, SIGUSR2});
        });
        REQUIRE(eventually([] { return handling(SIGUSR2); }));
        ::kill(::getpid(), SIGUSR2);
        REQUIRE(first_received.wait_for(3s) == std::future_status::ready);
        REQUIRE(first_received.get() == SIGUSR2);
        REQUIRE(eventually([] { return handling(SIGUSR1); }));
        ::kill(::getpid(), SIGUSR1);
        REQUIRE(second.wait_for(3s) == std::future_status::ready);
        REQUIRE(second.get() == SIGUSR1);
        REQUIRE(pool.running());
    }

    SECTION("With the default signals, stop() releases it with 0") {
        auto waiting = std::async(std::launch::async, [&] { return pool.wait(); });
        REQUIRE(eventually([] { return handling(SIGTERM) && handling(SIGINT) && handling(SIGQUIT); }));
        REQUIRE(waiting.wait_for(100ms) == std::future_status::timeout);

        std::thread([&] { pool.stop(); }).join();
        REQUIRE(waiting.wait_for(3s) == std::future_status::ready);
        REQUIRE(waiting.get() == 0);
        REQUIRE_FALSE(pool.running());
    }

    SECTION("stop() from one of its threads releases it with 0") {
        auto waiting = std::async(std::launch::async, [&] { return pool.wait({SIGUSR1}); });
        REQUIRE(eventually([] { return handling(SIGUSR1); }));
        boost::asio::post(pool.next_executor(), [&] { pool.stop(); });
        REQUIRE(waiting.wait_for(3s) == std::future_status::ready);
        REQUIRE(waiting.get() == 0);
    }

    SECTION("Several waits, all released by stop()") {
        auto first = std::async(std::launch::async, [&] { return pool.wait({SIGUSR1}); });
        auto second = std::async(std::launch::async, [&] { return pool.wait({}); });
        REQUIRE(eventually([] { return handling(SIGUSR1); }));
        std::this_thread::sleep_for(50ms);
        pool.stop();
        REQUIRE(first.wait_for(3s) == std::future_status::ready);
        REQUIRE(second.wait_for(3s) == std::future_status::ready);
        REQUIRE(first.get() == 0);
        REQUIRE(second.get() == 0);
    }

    SECTION("An empty set waits for stop()") {
        auto waiting = std::async(std::launch::async, [&] { return pool.wait({}); });
        REQUIRE(waiting.wait_for(200ms) == std::future_status::timeout);
        pool.stop();
        REQUIRE(waiting.wait_for(3s) == std::future_status::ready);
        REQUIRE(waiting.get() == 0);
    }

    SECTION("Not running, it returns 0 at once") {
        pool.stop();
        auto waiting = std::async(std::launch::async, [&] { return pool.wait({SIGUSR1}); });
        REQUIRE(waiting.wait_for(3s) == std::future_status::ready);
        REQUIRE(waiting.get() == 0);
        REQUIRE_FALSE(handling(SIGUSR1));
    }

    if (pool.running()) pool.stop();
}

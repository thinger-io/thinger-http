#include <catch2/catch_test_macros.hpp>
#include <thinger/asio/workers.hpp>
#include <thread>
#include <chrono>
#include <map>
#include <vector>

TEST_CASE("Workers initialization", "[asio][workers]") {
    SECTION("Workers can be started and stopped") {
        thinger::asio::workers test_workers;
        
        REQUIRE(test_workers.start(2));
        
        // Give some time for threads to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        REQUIRE(test_workers.stop());
    }
    
    SECTION("Get next io_context returns valid context") {
        thinger::asio::workers test_workers;
        test_workers.start(2);
        
        auto& ctx1 = test_workers.get_next_io_context();
        auto& ctx2 = test_workers.get_next_io_context();
        auto& ctx3 = test_workers.get_next_io_context();
        
        // Should cycle through contexts
        REQUIRE(&ctx1 == &ctx3);
        
        test_workers.stop();
    }
    
    SECTION("Isolated io_context is unique") {
        thinger::asio::workers test_workers;
        test_workers.start(2);
        
        auto& isolated1 = test_workers.get_isolated_io_context("test1");
        auto& isolated2 = test_workers.get_isolated_io_context("test2");
        auto& pool_ctx = test_workers.get_next_io_context();
        
        REQUIRE(&isolated1 != &isolated2);
        REQUIRE(&isolated1 != &pool_ctx);
        REQUIRE(&isolated2 != &pool_ctx);
        
        test_workers.stop();
    }
}

TEST_CASE("Workers hand out io_contexts round-robin from several threads", "[asio][workers]") {
    // Connections accepted on several threads at once (several listeners) take the next
    // io_context concurrently: every context still gets its share, without a data race
    thinger::asio::workers test_workers;
    REQUIRE(test_workers.start(4));

    constexpr int threads = 8;
    constexpr int calls = 10000;
    std::vector<std::map<boost::asio::io_context*, int>> taken(threads);
    std::vector<std::thread> callers;
    for (int t = 0; t < threads; ++t) {
        callers.emplace_back([&, t] {
            for (int i = 0; i < calls; ++i) ++taken[t][&test_workers.get_next_io_context()];
        });
    }
    for (auto& caller : callers) caller.join();

    std::map<boost::asio::io_context*, int> total;
    for (const auto& counts : taken) {
        for (const auto& [context, count] : counts) total[context] += count;
    }
    REQUIRE(total.size() == 4);
    for (const auto& [context, count] : total) REQUIRE(count == threads * calls / 4);

    test_workers.stop();
}

#include <catch2/catch_test_macros.hpp>
#include <thinger/asio/workers.hpp>
#include <thread>
#include <chrono>

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
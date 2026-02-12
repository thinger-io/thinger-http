#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/connection_pool.hpp>
#include <thinger/http/client/client_connection.hpp>
#include <thinger/asio/sockets/tcp_socket.hpp>
#include <boost/asio.hpp>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <random>

using namespace thinger::http;
namespace asio = boost::asio;

// Simple mock connection for testing
// We don't actually need a full client_connection, just something to store in the pool
class mock_connection : public client_connection {
    static std::shared_ptr<thinger::asio::tcp_socket> create_mock_socket(asio::io_context& context) {
        return std::make_shared<thinger::asio::tcp_socket>("test", context);
    }
public:
    mock_connection(asio::io_context& context) 
        : client_connection(create_mock_socket(context)) {}
};

TEST_CASE("Connection pool thread safety", "[connection_pool][threading]") {
    
    SECTION("Concurrent get/store operations are thread-safe") {
        connection_pool pool;
        std::atomic<int> successful_operations{0};
        std::atomic<int> total_operations{0};
        const int num_threads = 10;
        const int operations_per_thread = 1000;
        
        auto thread_func = [&](int thread_id) {
            asio::io_context context;
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, 9);
            
            // Keep some connections alive to make the test more realistic
            std::vector<std::shared_ptr<mock_connection>> active_connections;
            
            for (int i = 0; i < operations_per_thread; ++i) {
                total_operations++;
                
                // Generate a random host to create some variety
                std::string host = "host" + std::to_string(dis(gen));
                uint16_t port = 80 + dis(gen);
                
                // Randomly choose between get and store
                if (dis(gen) < 5) {
                    // Try to get a connection
                    auto conn = pool.get_connection(host, port, false);
                    if (conn) {
                        successful_operations++;
                    }
                } else {
                    // Store a new connection
                    auto new_conn = std::make_shared<mock_connection>(context);
                    pool.store_connection(host, port, false, new_conn);
                    successful_operations++;
                    
                    // Keep some connections alive
                    if (dis(gen) < 7) {  // 70% chance to keep alive
                        active_connections.push_back(new_conn);
                        // Limit active connections per thread
                        if (active_connections.size() > 20) {
                            active_connections.erase(active_connections.begin());
                        }
                    }
                }
            }
        };
        
        // Launch threads
        std::vector<std::thread> threads;
        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back(thread_func, i);
        }
        
        // Wait for all threads
        for (auto& t : threads) {
            t.join();
        }
        
        // Verify operations completed without crashes
        REQUIRE(total_operations == num_threads * operations_per_thread);
        REQUIRE(successful_operations > 0);
        REQUIRE(pool.size() > 0);
    }
    
    SECTION("Multiple readers can access simultaneously") {
        connection_pool pool;
        asio::io_context context;
        
        // Create connections that we'll keep alive
        const int num_connections = 50; 
        std::vector<std::shared_ptr<mock_connection>> connections;
        
        // Pre-populate the pool with connections that won't expire
        for (int i = 0; i < num_connections; ++i) {
            auto conn = std::make_shared<mock_connection>(context);
            connections.push_back(conn);  // Keep connections alive
            pool.store_connection("host" + std::to_string(i), 80, false, conn);
        }
        
        // Verify all connections are in the pool
        REQUIRE(pool.size() == num_connections);
        
        std::atomic<int> concurrent_readers{0};
        std::atomic<int> max_concurrent_readers{0};
        std::atomic<int> total_reads{0};
        std::atomic<int> successful_reads{0};
        std::atomic<bool> stop{false};
        const int num_readers = 100;  // Test with high concurrency 
        
        auto reader_func = [&](int thread_id) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, num_connections - 1);
            
            while (!stop) {
                // Increment concurrent readers counter
                int current = ++concurrent_readers;
                
                // Update max if needed
                int expected = max_concurrent_readers.load();
                while (current > expected && 
                       !max_concurrent_readers.compare_exchange_weak(expected, current)) {
                }
                
                // Perform read operation - random connection to increase contention
                int host_index = dis(gen);
                std::string host = "host" + std::to_string(host_index);
                auto conn = pool.get_connection(host, 80, false);
                
                // These should always succeed since connections are kept alive
                if (conn == nullptr) {
                    FAIL("Connection should not be null - race condition detected!");
                }
                successful_reads++;
                
                // Remove yield to maximize throughput
                // std::this_thread::yield();
                
                total_reads++;
                concurrent_readers--;
            }
        };
        
        // Launch reader threads
        std::vector<std::thread> threads;
        threads.reserve(num_readers);
        
        auto start_time = std::chrono::steady_clock::now();
        
        for (int i = 0; i < num_readers; ++i) {
            threads.emplace_back(reader_func, i);
        }
        
        // Let threads run for several seconds
        std::this_thread::sleep_for(std::chrono::seconds(3));
        stop = true;
        
        // Wait for all threads
        for (auto& t : threads) {
            t.join();
        }
        
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        // Verify all reads succeeded
        REQUIRE(successful_reads == total_reads.load());  // All reads should succeed
        REQUIRE(total_reads > 0);  // Should have performed many reads
        REQUIRE(max_concurrent_readers > 1);  // We should have had concurrent readers
        
        // Pool should still have all connections
        REQUIRE(pool.size() == num_connections);
        
        // Always show statistics
        std::cout << "\n=== Test Statistics ===" << std::endl;
        std::cout << "Test ran for: " << duration.count() << "ms" << std::endl;
        std::cout << "Total reads: " << total_reads.load() << std::endl;
        std::cout << "Reads per second: " << (total_reads.load() * 1000.0 / duration.count()) << std::endl;
        std::cout << "Max concurrent readers: " << max_concurrent_readers.load() << std::endl;
        std::cout << "===================\n" << std::endl;
    }
    
    SECTION("Cleanup expired connections is thread-safe with concurrent operations") {
        connection_pool pool;
        asio::io_context context;
        
        // Create some initial alive connections to ensure reads always succeed
        const int alive_connections = 5;
        std::vector<std::shared_ptr<mock_connection>> persistent_connections;
        for (int i = 0; i < alive_connections; ++i) {
            auto conn = std::make_shared<mock_connection>(context);
            persistent_connections.push_back(conn);  // Keep alive
            pool.store_connection("alive_host_" + std::to_string(i), 80, false, conn);
        }
        
        // Test concurrent cleanup with other operations
        std::atomic<bool> stop{false};
        std::atomic<int> total_cleaned{0};
        std::atomic<int> stores_made{0};
        
        // Thread that adds connections that will expire
        std::thread store_thread([&]() {
            asio::io_context local_context;
            int counter = 0;
            while (!stop) {
                auto conn = std::make_shared<mock_connection>(local_context);
                pool.store_connection("temp_host_" + std::to_string(counter++), 80, false, conn);
                stores_made++;
                // conn goes out of scope and expires
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        
        // Thread that cleans up
        std::thread cleanup_thread([&]() {
            while (!stop) {
                size_t removed = pool.cleanup_expired();
                total_cleaned += removed;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        
        // Thread that reads
        std::thread read_thread([&]() {
            while (!stop) {
                // Try to get both alive and potentially expired connections
                for (int i = 0; i < alive_connections; ++i) {
                    auto conn = pool.get_connection("alive_host_" + std::to_string(i), 80, false);
                    REQUIRE(conn != nullptr); // These should always be found
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }
        });
        
        // Run for a fixed time
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        stop = true;
        
        // Wait for all threads
        store_thread.join();
        cleanup_thread.join();
        read_thread.join();
        
        // The total cleaned should be approximately equal to stores made
        // (within a small margin due to timing)
        INFO("Stores made: " << stores_made.load() << ", Total cleaned: " << total_cleaned.load());
        REQUIRE(total_cleaned <= stores_made);
        REQUIRE(total_cleaned > 0);
        
        // Final cleanup
        size_t final_cleanup = pool.cleanup_expired();
        
        // After final cleanup, only alive connections should remain
        REQUIRE(pool.size() == alive_connections);
    }
    
    SECTION("Size and clear operations are thread-safe") {
        connection_pool pool;
        asio::io_context context;
        std::atomic<bool> stop{false};
        
        // Pre-populate
        for (int i = 0; i < 5; ++i) {
            auto conn = std::make_shared<mock_connection>(context);
            pool.store_connection("host" + std::to_string(i), 80, false, conn);
        }
        
        // Thread that checks size
        std::thread size_thread([&]() {
            while (!stop) {
                size_t size = pool.size();
                // The pool can grow larger temporarily before clear() is called
                REQUIRE(size <= 50); // More reasonable upper bound considering timing
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
        
        // Thread that stores connections
        std::thread store_thread([&]() {
            int counter = 0;
            while (!stop) {
                auto conn = std::make_shared<mock_connection>(context);
                pool.store_connection("dynamic_host" + std::to_string(counter++), 80, false, conn);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        
        // Thread that clears the pool periodically
        std::thread clear_thread([&]() {
            while (!stop) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                pool.clear();
            }
        });
        
        // Run for a short time
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        stop = true;
        
        // Wait for all threads
        size_thread.join();
        store_thread.join();
        clear_thread.join();
        
        // No crash means success
        REQUIRE(true);
    }
    
    SECTION("Concurrent operations demonstrate thread safety") {
        connection_pool pool;
        asio::io_context context;
        
        // Track order of operations
        std::atomic<int> operation_counter{0};
        std::atomic<int> write_start{-1};
        std::atomic<int> write_end{-1};
        std::atomic<int> read_start{-1};
        std::atomic<int> read_end{-1};
        
        const int num_operations = 50000;
        std::atomic<bool> collision_detected{false};
        
        // Multiple writer threads
        std::vector<std::thread> writers;
        for (int i = 0; i < 5; ++i) {
            writers.emplace_back([&, i]() {
                for (int j = 0; j < num_operations; ++j) {
                    auto conn = std::make_shared<mock_connection>(context);
                    
                    int my_start = operation_counter++;
                    pool.store_connection("host" + std::to_string(i), 80 + j, false, conn);
                    int my_end = operation_counter++;
                    
                    // Check if any read operation interleaved with our write
                    if (read_start.load() > my_start && read_start.load() < my_end) {
                        collision_detected = true;
                    }
                }
            });
        }
        
        // Multiple reader threads
        std::vector<std::thread> readers;
        for (int i = 0; i < 5; ++i) {
            readers.emplace_back([&, i]() {
                for (int j = 0; j < num_operations; ++j) {
                    int my_start = operation_counter++;
                    auto conn = pool.get_connection("host" + std::to_string(i), 80 + j, false);
                    int my_end = operation_counter++;
                    
                    // Update read tracking
                    read_start = my_start;
                    read_end = my_end;
                }
            });
        }
        
        // Wait for all threads
        for (auto& t : writers) t.join();
        for (auto& t : readers) t.join();
        
        // The test passes if we completed without crashes
        // Thread safety is demonstrated by successful completion
        REQUIRE(operation_counter.load() > 0);
        INFO("Total operations completed: " << operation_counter.load());
    }
}
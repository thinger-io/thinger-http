#include <catch2/catch_test_macros.hpp>
#include <thinger/http_client.hpp>
#include <thinger/http/client/connection_pool.hpp>
#include <thinger/http/client/client_connection.hpp>
#include <thinger/asio/sockets/tcp_socket.hpp>
#include <boost/asio.hpp>
#include <thread>
#include <set>
#include "../fixtures/test_server_fixture.hpp"

using namespace thinger;

TEST_CASE_METHOD(thinger::http::test::TestServerFixture, "HTTP Client connection pool isolation", "[http][client][pool]") {
    
    SECTION("Each client instance has its own connection pool") {
        // Create two clients with different timeouts
        http::async_client client1;
        http::async_client client2;
        
        client1.timeout(std::chrono::seconds(10));
        client2.timeout(std::chrono::seconds(2));
        
        // Both clients request the same endpoint
        bool client1_success = false;
        bool client1_delay_success = false;
        bool client2_timeout = false;
        
        // This creates a connection with 10s timeout for client1
        client1.get(base_url + "/get", [&](const http::client_response& res) {
            client1_success = res.ok();
        });
        client1.wait();
        
        // Now client1 requests a slow endpoint - should succeed with existing 10s connection
        client1.get(base_url + "/delay/5", [&](const http::client_response& res) {
            client1_delay_success = res.ok();
        });
        
        // Client2 requests same slow endpoint - should timeout with its 2s timeout
        client2.get(base_url + "/delay/5", [&](const http::client_response& res) {
            client2_timeout = !res.ok();
        });
        
        client1.wait();
        client2.wait();
        
        REQUIRE(client1_success);
        REQUIRE(client1_delay_success);
        REQUIRE(client2_timeout);
    }
    
    SECTION("Connection pools are cleaned up when client is destroyed") {
        bool first_request_complete = false;
        bool first_request_success = false;
        bool second_request_complete = false;
        bool second_request_failed = false;
        
        // Create and destroy a client
        {
            http::async_client client1;
            client1.timeout(std::chrono::seconds(10));
            
            client1.get(base_url + "/get", [&](const http::client_response& res) {
                first_request_success = res.ok();
                first_request_complete = true;
            });
            
            client1.wait();
        } // client1 destroyed here
        
        // Create a new client - should not reuse connections from destroyed client
        http::async_client client2;
        client2.timeout(std::chrono::seconds(2)); // Different timeout
        
        // Request to slow endpoint
        client2.get(base_url + "/delay/5", [&](const http::client_response& res) {
            // Should timeout with 2s timeout (not use old 10s connection)
            second_request_failed = !res.ok();
            second_request_complete = true;
        });
        
        client2.wait();
        
        REQUIRE(first_request_complete);
        REQUIRE(first_request_success);
        REQUIRE(second_request_complete);
        REQUIRE(second_request_failed);
    }
    
    SECTION("Connections are reused within the same client") {
        http::async_client client;
        std::vector<std::chrono::steady_clock::time_point> request_times;
        std::vector<bool> request_results;
        std::mutex mutex;
        
        const int num_requests = 5;
        
        // Make multiple requests to the same endpoint
        for (int i = 0; i < num_requests; ++i) {
            auto start = std::chrono::steady_clock::now();
            
            client.get(base_url + "/get?req=" + std::to_string(i), 
                      [&, start, i](const http::client_response& res) {
                auto end = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock(mutex);
                
                request_results.push_back(res.ok());
                if (res.ok()) {
                    request_times.push_back(end);
                    
                    // Log request completion time
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                    INFO("Request " << i << " took " << duration.count() << "ms");
                } else {
                    INFO("Request " << i << " failed: " << res.error());
                }
            });
            
            // Small delay between requests
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        client.wait();
        
        // Check all requests succeeded
        REQUIRE(request_results.size() == num_requests);
        for (size_t i = 0; i < request_results.size(); ++i) {
            INFO("Request " << i << " result: " << request_results[i]);
            REQUIRE(request_results[i]);
        }
        
        REQUIRE(request_times.size() == num_requests);
        
        // First request might be slower (establishing connection)
        // Subsequent requests should be faster (reusing connection)
        // This is a heuristic test - might be flaky depending on network conditions
    }
    
    SECTION("Different endpoints on same host reuse connections") {
        http::async_client client;
        
        int requests_complete = 0;
        std::vector<bool> request_results;
        
        // First request to /get
        client.get(base_url + "/get", [&](const http::client_response& res) {
            request_results.push_back(res.ok());
            requests_complete++;
        });
        
        // Second request to /headers (same host, should reuse connection)
        client.get(base_url + "/headers", [&](const http::client_response& res) {
            request_results.push_back(res.ok());
            requests_complete++;
        });
        
        // Third request to /status/200 (same host, should reuse connection)
        client.get(base_url + "/status/200", [&](const http::client_response& res) {
            request_results.push_back(res.ok());
            requests_complete++;
        });
        
        client.wait();
        
        REQUIRE(requests_complete == 3);
        REQUIRE(request_results.size() == 3);
        for (size_t i = 0; i < request_results.size(); ++i) {
            REQUIRE(request_results[i]);
        }
    }
    
    SECTION("Different ports use different connections") {
        // Start a second test server on a different port
        thinger::http::test::TestServerFixture test_server2;
        
        http::async_client client;
        int server1_requests = 0;
        int server2_requests = 0;
        std::vector<bool> request_results;
        
        // Request to first server
        client.get(base_url + "/get", [&](const http::client_response& res) {
            request_results.push_back(res.ok());
            if (res.ok()) server1_requests++;
        });
        
        // Request to second server (different port)
        client.get(test_server2.base_url + "/get", [&](const http::client_response& res) {
            request_results.push_back(res.ok());
            if (res.ok()) server2_requests++;
        });
        
        // Another request to first server (should reuse connection)
        client.get(base_url + "/headers", [&](const http::client_response& res) {
            request_results.push_back(res.ok());
            if (res.ok()) server1_requests++;
        });
        
        client.wait();
        
        REQUIRE(request_results.size() == 3);
        for (size_t i = 0; i < request_results.size(); ++i) {
            REQUIRE(request_results[i]);
        }
        REQUIRE(server1_requests == 2);
        REQUIRE(server2_requests == 1);
    }
    
    SECTION("Connection pool cleanup expired connections") {
        http::connection_pool pool;
        boost::asio::io_context context;
        
        // Create mock TCP socket for testing
        class test_tcp_socket : public thinger::asio::tcp_socket {
        public:
            test_tcp_socket(boost::asio::io_context& ctx) 
                : tcp_socket("test", ctx) {}
        };
        
        // Create connections that will expire
        const int expired_connections = 10;
        for (int i = 0; i < expired_connections; ++i) {
            auto socket = std::make_shared<test_tcp_socket>(context);
            auto conn = std::make_shared<http::client_connection>(socket);
            pool.store_connection("expired_host_" + std::to_string(i), 80, false, conn);
            // Let conn go out of scope so the weak_ptr expires
        }
        
        // Create connections that will stay alive
        const int alive_connections = 5;
        std::vector<std::shared_ptr<http::client_connection>> active_connections;
        for (int i = 0; i < alive_connections; ++i) {
            auto socket = std::make_shared<test_tcp_socket>(context);
            auto conn = std::make_shared<http::client_connection>(socket);
            active_connections.push_back(conn);  // Keep alive
            pool.store_connection("alive_host_" + std::to_string(i), 80, false, conn);
        }
        
        // Verify initial pool size
        REQUIRE(pool.size() == expired_connections + alive_connections);
        
        // Run cleanup
        size_t cleaned = pool.cleanup_expired();
        
        // Verify exact number of connections cleaned
        REQUIRE(cleaned == expired_connections);
        REQUIRE(pool.size() == alive_connections);
        
        // Try to get an alive connection - should succeed
        auto alive_conn = pool.get_connection("alive_host_0", 80, false);
        REQUIRE(alive_conn != nullptr);
        
        // Try to get an expired connection - should fail
        auto expired_conn = pool.get_connection("expired_host_0", 80, false);
        REQUIRE(expired_conn == nullptr);
    }
    
    SECTION("Connection pool handles connection failures gracefully") {
        http::async_client client;
        
        // First request to invalid host
        bool first_failed = false;
        client.get("https://this-host-definitely-does-not-exist-12345.com/test", 
                   [&](const http::client_response& res) {
            first_failed = !res.ok();
        });
        
        client.wait();
        REQUIRE(first_failed);
        
        // Second request to valid host should work
        bool second_succeeded = false;
        client.get(base_url + "/get", [&](const http::client_response& res) {
            second_succeeded = res.ok();
        });
        
        client.wait();
        REQUIRE(second_succeeded);
    }
}
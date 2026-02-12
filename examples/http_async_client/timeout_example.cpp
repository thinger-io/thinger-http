#include <iostream>
#include <chrono>
#include <atomic>
#include <thinger/http_client.hpp>

using namespace thinger;

int main() {
    std::cout << "Async Client Timeout Example\n" << std::endl;

    // Create pool client with 5 second timeout
    http::async_client client;
    client.timeout(std::chrono::seconds(5));

    std::cout << "Async client configured with 5s timeout\n" << std::endl;

    auto start = std::chrono::steady_clock::now();

    // Fast request - should succeed
    client.get("https://httpbin.org/delay/1", [](http::client_response& res) {
        if (res.ok()) {
            std::cout << "[OK] Fast request (1s delay) succeeded" << std::endl;
        } else {
            std::cout << "[FAIL] Fast request failed: " << res.error() << std::endl;
        }
    });

    // Medium request - should succeed with 5s timeout
    client.get("https://httpbin.org/delay/3", [](http::client_response& res) {
        if (res.ok()) {
            std::cout << "[OK] Medium request (3s delay) succeeded" << std::endl;
        } else {
            std::cout << "[FAIL] Medium request failed: " << res.error() << std::endl;
        }
    });

    // Slow request - should timeout
    client.get("https://httpbin.org/delay/7", [](http::client_response& res) {
        if (res.ok()) {
            std::cout << "[UNEXPECTED] Slow request (7s delay) succeeded" << std::endl;
        } else {
            std::cout << "[EXPECTED] Slow request timed out: " << res.error() << std::endl;
        }
    });

    std::cout << "\nWaiting for all requests (some should timeout)...\n" << std::endl;

    // Wait for all
    client.wait();

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start
    ).count();

    // All 3 requests run concurrently, so total time is max(delays) capped by timeout
    std::cout << "\nTotal time: ~" << elapsed << " seconds" << std::endl;
    std::cout << "(Should be ~5s since slowest request times out at 5s)" << std::endl;

    // ============================================
    // Wait with timeout
    // ============================================
    std::cout << "\n--- Wait with timeout ---\n" << std::endl;

    http::async_client client2;
    client2.timeout(std::chrono::seconds(30));  // Long timeout

    // Launch slow request
    client2.get("https://httpbin.org/delay/3", [](http::client_response& res) {
        std::cout << "Request completed: " << (res.ok() ? "OK" : res.error()) << std::endl;
    });

    // Wait with short timeout
    if (client2.wait_for(std::chrono::seconds(1))) {
        std::cout << "Completed within 1 second" << std::endl;
    } else {
        std::cout << "Still running after 1 second, pending: " << client2.pending_requests() << std::endl;
    }

    // Wait for completion
    client2.wait();
    std::cout << "Now completed!" << std::endl;

    return 0;
}

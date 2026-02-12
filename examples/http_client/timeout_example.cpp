#include <iostream>
#include <chrono>
#include <thinger/http_client.hpp>

using namespace thinger;

int main() {
    std::cout << "HTTP Client Timeout Example\n" << std::endl;

    // Create client with 5 second timeout
    http::client client;
    client.timeout(std::chrono::seconds(5));

    std::cout << "Client configured with 5s timeout\n" << std::endl;

    auto start = std::chrono::steady_clock::now();

    // Fast request - should succeed
    {
        std::cout << "Testing fast request (1s delay)..." << std::endl;
        auto res = client.get("https://httpbin.org/delay/1");
        if (res.ok()) {
            std::cout << "[OK] Fast request succeeded" << std::endl;
        } else {
            std::cout << "[FAIL] Fast request failed: " << res.error() << std::endl;
        }
    }

    // Medium request - should succeed with 5s timeout
    {
        std::cout << "\nTesting medium request (3s delay)..." << std::endl;
        auto res = client.get("https://httpbin.org/delay/3");
        if (res.ok()) {
            std::cout << "[OK] Medium request succeeded" << std::endl;
        } else {
            std::cout << "[FAIL] Medium request failed: " << res.error() << std::endl;
        }
    }

    // Slow request - should timeout
    {
        std::cout << "\nTesting slow request (7s delay, should timeout)..." << std::endl;
        auto res = client.get("https://httpbin.org/delay/7");
        if (res.ok()) {
            std::cout << "[UNEXPECTED] Slow request succeeded" << std::endl;
        } else {
            std::cout << "[EXPECTED] Slow request timed out: " << res.error() << std::endl;
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start
    ).count();

    std::cout << "\nTotal time: ~" << elapsed << " seconds" << std::endl;

    // ============================================
    // Changing timeout between requests
    // ============================================
    std::cout << "\n--- Changing timeout ---\n" << std::endl;

    http::client client2;

    // Short timeout
    client2.timeout(std::chrono::seconds(2));
    std::cout << "Timeout set to 2s" << std::endl;

    auto res1 = client2.get("https://httpbin.org/delay/3");
    std::cout << "3s delay result: " << (res1.ok() ? "OK" : res1.error()) << std::endl;

    // Longer timeout
    client2.timeout(std::chrono::seconds(10));
    std::cout << "\nTimeout changed to 10s" << std::endl;

    auto res2 = client2.get("https://httpbin.org/delay/3");
    std::cout << "3s delay result: " << (res2.ok() ? "OK" : res2.error()) << std::endl;

    // Note: For concurrent requests with timeout, use http::async_client
    // See examples/http_async_client/timeout_example.cpp

    return 0;
}

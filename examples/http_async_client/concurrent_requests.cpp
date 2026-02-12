#include <iostream>
#include <vector>
#include <atomic>
#include <thinger/http_client.hpp>

using namespace thinger;

int main() {
    std::cout << "Concurrent Requests Example (async_client)\n" << std::endl;

    // URLs to fetch
    std::vector<std::string> urls = {
        "https://api.github.com/users/github",
        "https://api.github.com/users/torvalds",
        "https://api.github.com/users/octocat"
    };

    http::async_client client;
    std::atomic<int> completed{0};

    std::cout << "Launching " << urls.size() << " concurrent requests...\n" << std::endl;

    // Launch all requests concurrently
    for (const auto& url : urls) {
        client.get(url, [&completed](http::client_response& res) {
            if (!res) {
                std::cerr << "Failed: " << res.error() << std::endl;
                ++completed;
                return;
            }

            if (res.is_json()) {
                try {
                    auto json = res.json();
                    std::cout << "User: " << json["login"].get<std::string>()
                              << " - Repos: " << json["public_repos"].get<int>()
                              << std::endl;
                } catch (const std::exception& e) {
                    std::cerr << "JSON error: " << e.what() << std::endl;
                }
            }
            ++completed;
        });
    }

    // Wait for all requests to complete
    client.wait();

    std::cout << "\nCompleted " << completed.load() << " requests concurrently!" << std::endl;

    // ============================================
    // Using coroutines for more control
    // ============================================
    std::cout << "\n--- Using Coroutines ---\n" << std::endl;

    http::async_client client2;
    std::atomic<int> completed2{0};

    for (const auto& url : urls) {
        client2.run([&client2, url, &completed2]() -> awaitable<void> {
            auto res = co_await client2.get(url);

            if (res && res.is_json()) {
                auto json = res.json();
                std::cout << "[coroutine] User: " << json["login"].get<std::string>()
                          << " - Repos: " << json["public_repos"].get<int>()
                          << std::endl;
            }
            ++completed2;
        });
    }

    client2.wait();

    std::cout << "\nCompleted " << completed2.load() << " coroutine requests!" << std::endl;

    // ============================================
    // Check pending requests
    // ============================================
    std::cout << "\n--- Pending Requests ---\n" << std::endl;

    http::async_client client3;

    // Launch requests
    client3.get("https://httpbin.org/delay/1", [](http::client_response&) {
        std::cout << "Request 1 completed" << std::endl;
    });
    client3.get("https://httpbin.org/delay/2", [](http::client_response&) {
        std::cout << "Request 2 completed" << std::endl;
    });

    std::cout << "Pending requests: " << client3.pending_requests() << std::endl;

    // Wait with timeout
    if (client3.wait_for(std::chrono::milliseconds(500))) {
        std::cout << "All completed within timeout" << std::endl;
    } else {
        std::cout << "Still waiting... pending: " << client3.pending_requests() << std::endl;
    }

    // Wait for remaining
    client3.wait();
    std::cout << "All completed!" << std::endl;

    return 0;
}

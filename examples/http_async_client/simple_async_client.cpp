#include <iostream>
#include <thinger/http_client.hpp>

using namespace thinger;

int main() {
    std::cout << "=== Async Client Example ===\n" << std::endl;

    // ============================================
    // 1. Callback-based API
    // ============================================
    std::cout << "--- Callback API ---\n" << std::endl;

    http::async_client client;

    // Simple GET request with callback
    client.get("https://api.github.com/users/github", [](http::client_response& res) {
        if (!res) {
            std::cerr << "Request failed: " << res.error() << std::endl;
            return;
        }

        std::cout << "Status: " << res.status() << std::endl;
        std::cout << "Content-Type: " << res.content_type() << std::endl;
        std::cout << "Content-Length: " << res.content_length() << " bytes\n" << std::endl;

        if (res.is_json()) {
            auto json = res.json();
            std::cout << "User info:" << std::endl;
            std::cout << "  Login: " << json["login"].get<std::string>() << std::endl;
            std::cout << "  Name: " << json["name"].get<std::string>() << std::endl;
            std::cout << "  Public repos: " << json["public_repos"].get<int>() << std::endl;
        }
    });

    client.wait();

    // ============================================
    // 2. Coroutine-based API (co_await)
    // ============================================
    std::cout << "\n--- Coroutine API (co_await) ---\n" << std::endl;

    client.run([&client]() -> awaitable<void> {
        // co_await makes async code look sequential
        auto res = co_await client.get("https://api.github.com/users/torvalds");

        if (!res) {
            std::cerr << "Request failed: " << res.error() << std::endl;
            co_return;
        }

        std::cout << "Status: " << res.status() << std::endl;

        if (res.is_json()) {
            auto json = res.json();
            std::cout << "User info:" << std::endl;
            std::cout << "  Login: " << json["login"].get<std::string>() << std::endl;
            std::cout << "  Name: " << json["name"].get<std::string>() << std::endl;
            std::cout << "  Public repos: " << json["public_repos"].get<int>() << std::endl;
        }

        // Sequential requests within the same coroutine
        std::cout << "\nFetching another user..." << std::endl;
        auto res2 = co_await client.get("https://api.github.com/users/octocat");

        if (res2 && res2.is_json()) {
            auto json = res2.json();
            std::cout << "  Login: " << json["login"].get<std::string>() << std::endl;
            std::cout << "  Public repos: " << json["public_repos"].get<int>() << std::endl;
        }
    });

    client.wait();

    std::cout << "\nAll requests completed!" << std::endl;

    return 0;
}

#include <iostream>
#include <thinger/http_client.hpp>

using namespace thinger;

int main() {
    std::cout << "Simple Async Client Example\n" << std::endl;

    // Create pool client - uses worker thread pool
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
            try {
                auto json = res.json();
                std::cout << "User info:" << std::endl;
                std::cout << "  Login: " << json["login"].get<std::string>() << std::endl;
                std::cout << "  Name: " << json["name"].get<std::string>() << std::endl;
                std::cout << "  Public repos: " << json["public_repos"].get<int>() << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "Failed to parse JSON: " << e.what() << std::endl;
            }
        }
    });

    // Wait for request to complete
    client.wait();

    std::cout << "\n--- Multiple sequential callbacks ---\n" << std::endl;

    // Multiple requests with callbacks - each runs async but we wait between them
    client.get("https://api.github.com/users/torvalds", [](http::client_response& res) {
        if (res && res.is_json()) {
            auto json = res.json();
            std::cout << "Torvalds repos: " << json["public_repos"].get<int>() << std::endl;
        }
    });
    client.wait();

    client.get("https://api.github.com/users/octocat", [](http::client_response& res) {
        if (res && res.is_json()) {
            auto json = res.json();
            std::cout << "Octocat repos: " << json["public_repos"].get<int>() << std::endl;
        }
    });
    client.wait();

    std::cout << "\nAll requests completed!" << std::endl;

    return 0;
}

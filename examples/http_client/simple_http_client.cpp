#include <iostream>
#include <thinger/http_client.hpp>

using namespace thinger;

int main() {
    std::cout << "Simple HTTP Client Example\n" << std::endl;

    // Create HTTP client
    http::client client;

    // Simple GET request - no run() needed!
    auto res = client.get("https://api.github.com/users/github");

    if (!res) {
        std::cerr << "Request failed: " << res.error() << std::endl;
        return 1;
    }

    std::cout << "Status: " << res.status() << std::endl;
    std::cout << "Content-Type: " << res.content_type() << std::endl;
    std::cout << "Content-Length: " << res.content_length() << " bytes\n" << std::endl;

    // Check if response is JSON
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
    } else {
        std::cout << "Body:" << std::endl;
        std::cout << res.body() << std::endl;
    }

    std::cout << "\n--- Multiple sequential requests ---\n" << std::endl;

    // Multiple requests - each one is synchronous
    auto r1 = client.get("https://api.github.com/users/torvalds");
    if (r1) {
        auto json = r1.json();
        std::cout << "Torvalds repos: " << json["public_repos"].get<int>() << std::endl;
    }

    auto r2 = client.get("https://api.github.com/users/octocat");
    if (r2) {
        auto json = r2.json();
        std::cout << "Octocat repos: " << json["public_repos"].get<int>() << std::endl;
    }

    std::cout << "\nAll requests completed!" << std::endl;

    return 0;
}

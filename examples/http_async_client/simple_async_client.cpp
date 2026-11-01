#include <iostream>
#include <thinger/http_client.hpp>

using namespace thinger;

int main() {
    std::cout << "Simple Async Client Example (Callbacks)\n" << std::endl;

    // The workers the clients run on: the application starts them, and stops them at the end
    asio::workers workers;
    workers.start();

    http::async_client client(workers);

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

    // Wait for request to complete
    client.wait();

    std::cout << "\n--- Multiple requests ---\n" << std::endl;

    // Multiple requests with callbacks
    client.get("https://api.github.com/users/torvalds", [](http::client_response& res) {
        if (res && res.is_json()) {
            auto json = res.json();
            std::cout << "Torvalds repos: " << json["public_repos"].get<int>() << std::endl;
        }
    });

    client.get("https://api.github.com/users/octocat", [](http::client_response& res) {
        if (res && res.is_json()) {
            auto json = res.json();
            std::cout << "Octocat repos: " << json["public_repos"].get<int>() << std::endl;
        }
    });

    client.wait();

    std::cout << "\nAll requests completed!" << std::endl;

    workers.stop();
    return 0;
}

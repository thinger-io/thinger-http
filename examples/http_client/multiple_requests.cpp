#include <iostream>
#include <vector>
#include <thinger/http_client.hpp>

using namespace thinger;

int main() {
    std::cout << "Multiple Sequential Requests Example (http::client)\n" << std::endl;

    // URLs to fetch
    std::vector<std::string> urls = {
        "https://api.github.com/users/github",
        "https://api.github.com/users/torvalds",
        "https://api.github.com/users/octocat"
    };

    http::client client;

    std::cout << "Fetching " << urls.size() << " URLs sequentially...\n" << std::endl;

    for (const auto& url : urls) {
        auto res = client.get(url);

        if (!res) {
            std::cerr << "Failed to fetch " << url << ": " << res.error() << std::endl;
            continue;
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
    }

    std::cout << "\nAll requests completed!" << std::endl;

    // Note: For concurrent requests, use http::async_client instead
    // See examples/http_async_client/concurrent_requests.cpp

    return 0;
}

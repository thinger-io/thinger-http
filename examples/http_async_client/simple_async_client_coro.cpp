#include <iostream>
#include <thinger/http_client.hpp>

using namespace thinger;

int main() {
    std::cout << "Simple Async Client Example (Coroutines)\n" << std::endl;

    http::async_client client;

    // Sequential requests using co_await
    client.run([&client]() -> awaitable<void> {
        auto res = co_await client.get("https://api.github.com/users/github");

        if (!res) {
            std::cerr << "Request failed: " << res.error() << std::endl;
            co_return;
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

        // Sequential request within the same coroutine
        std::cout << "\nFetching another user..." << std::endl;
        auto res2 = co_await client.get("https://api.github.com/users/torvalds");

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

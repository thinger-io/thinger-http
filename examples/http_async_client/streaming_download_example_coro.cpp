#include <iostream>
#include <iomanip>
#include <fstream>
#include <thinger/http_client.hpp>

using namespace thinger;

std::string format_bytes(size_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    int unit = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024 && unit < 3) {
        size /= 1024;
        unit++;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " " << units[unit];
    return oss.str();
}

int main() {
    std::cout << "Streaming Download Example (Coroutines)\n" << std::endl;

    http::async_client client;

    // =========================================
    // Example 1: Single streaming GET
    // =========================================
    std::cout << "Example 1: Streaming GET with co_await\n" << std::endl;

    client.run([&client]() -> awaitable<void> {
        auto request = std::make_shared<http::http_request>();
        request->set_url("https://api.github.com/users/github");
        request->set_method(http::method::GET);

        size_t chunks = 0;
        auto result = co_await client.send_streaming(request,
            [&chunks](const http::stream_info& info) {
                chunks++;
                std::cout << "  Chunk #" << chunks
                          << ": " << info.data.size() << " bytes" << std::endl;
                return true;
            });

        if (result) {
            std::cout << "Streaming completed: " << chunks << " chunks, "
                      << format_bytes(result.bytes_transferred) << std::endl;
        }
    });

    client.wait();

    // =========================================
    // Example 2: Parallel downloads
    // =========================================
    std::cout << "\nExample 2: Parallel downloads with co_await\n" << std::endl;

    // Launch 3 parallel download coroutines
    std::string urls[] = {
        "https://httpbin.org/bytes/1024",
        "https://httpbin.org/bytes/2048",
        "https://httpbin.org/bytes/512"
    };
    std::string paths[] = {
        "/tmp/file1.bin",
        "/tmp/file2.bin",
        "/tmp/file3.bin"
    };

    for (int i = 0; i < 3; ++i) {
        client.run([&client, url = urls[i], path = paths[i], i]() -> awaitable<void> {
            std::ofstream file(path, std::ios::binary);
            if (!file) {
                std::cerr << "Cannot open " << path << std::endl;
                co_return;
            }

            auto request = std::make_shared<http::http_request>();
            request->set_url(url);
            request->set_method(http::method::GET);

            auto result = co_await client.send_streaming(request,
                [&file](const http::stream_info& info) {
                    file.write(info.data.data(), static_cast<std::streamsize>(info.data.size()));
                    return true;
                });

            std::cout << "Download " << (i + 1) << ": "
                      << (result ? "OK" : result.error)
                      << " (" << format_bytes(result.bytes_transferred) << ")"
                      << std::endl;
        });
    }

    // Wait for all parallel downloads
    client.wait();

    std::cout << "\nAll done!" << std::endl;

    return 0;
}

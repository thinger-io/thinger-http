#include <iostream>
#include <iomanip>
#include <thinger/http_client.hpp>

using namespace thinger;

// Helper to format bytes
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

// Helper to draw progress bar
void draw_progress(size_t downloaded, size_t total) {
    const int bar_width = 40;
    float progress = total > 0 ? static_cast<float>(downloaded) / total : 0;
    int filled = static_cast<int>(bar_width * progress);

    std::cout << "\r[";
    for (int i = 0; i < bar_width; ++i) {
        if (i < filled) std::cout << "=";
        else if (i == filled) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << "] " << std::setw(3) << static_cast<int>(progress * 100) << "% "
              << format_bytes(downloaded);
    if (total > 0) {
        std::cout << " / " << format_bytes(total);
    }
    std::cout << std::flush;
}

int main(int argc, char* argv[]) {
    std::cout << "Streaming Download Example\n" << std::endl;

    http::client client;

    // =========================================
    // Example 1: Download file with progress
    // =========================================
    std::cout << "Example 1: Download file to disk with progress\n" << std::endl;

    // Download a sample file (using a small text file for demo)
    auto result = client.request("https://raw.githubusercontent.com/torvalds/linux/master/COPYING")
        .download("/tmp/linux_license.txt", [](size_t downloaded, size_t total) {
            draw_progress(downloaded, total);
        });

    std::cout << std::endl; // New line after progress bar

    if (result) {
        std::cout << "Download completed!" << std::endl;
        std::cout << "  Status: " << result.status_code << std::endl;
        std::cout << "  Bytes: " << format_bytes(result.bytes_transferred) << std::endl;
        std::cout << "  File: /tmp/linux_license.txt" << std::endl;
    } else {
        std::cerr << "Download failed: " << result.error << std::endl;
    }

    // =========================================
    // Example 2: Streaming with custom callback
    // =========================================
    std::cout << "\n\nExample 2: Streaming with custom callback\n" << std::endl;

    size_t chunk_count = 0;
    size_t total_bytes = 0;

    auto stream_result = client.request("https://api.github.com/users/github")
        .header("Accept", "application/json")
        .get([&](const http::stream_info& info) {
            chunk_count++;
            total_bytes += info.data.size();

            std::cout << "  Chunk #" << chunk_count
                      << ": " << info.data.size() << " bytes"
                      << " (total: " << info.downloaded << "/"
                      << (info.total > 0 ? std::to_string(info.total) : "unknown") << ")"
                      << std::endl;

            // Return true to continue, false to abort
            return true;
        });

    if (stream_result) {
        std::cout << "\nStreaming completed!" << std::endl;
        std::cout << "  Total chunks: " << chunk_count << std::endl;
        std::cout << "  Total bytes: " << total_bytes << std::endl;
    } else {
        std::cerr << "Streaming failed: " << stream_result.error << std::endl;
    }

    // =========================================
    // Example 3: Abort download mid-stream
    // =========================================
    std::cout << "\n\nExample 3: Abort download after 1KB\n" << std::endl;

    auto abort_result = client.request("https://raw.githubusercontent.com/torvalds/linux/master/COPYING")
        .get([](const http::stream_info& info) {
            std::cout << "  Downloaded: " << info.downloaded << " bytes" << std::endl;
            // Abort after downloading more than 1KB
            if (info.downloaded > 1024) {
                std::cout << "  -> Aborting download!" << std::endl;
                return false;
            }
            return true;
        });

    if (!abort_result) {
        std::cout << "Download was aborted as expected" << std::endl;
        std::cout << "  Bytes transferred: " << abort_result.bytes_transferred << std::endl;
    }

    // =========================================
    // Example 4: POST with streaming response
    // =========================================
    std::cout << "\n\nExample 4: POST with streaming response\n" << std::endl;

    auto post_result = client.request("https://httpbin.org/post")
        .header("X-Custom-Header", "test-value")
        .body(R"({"message": "Hello from streaming client!"})", "application/json")
        .post([](const http::stream_info& info) {
            std::cout << "  Received chunk: " << info.data.size() << " bytes" << std::endl;
            return true;
        });

    if (post_result) {
        std::cout << "POST with streaming completed!" << std::endl;
        std::cout << "  Status: " << post_result.status_code << std::endl;
    } else {
        std::cerr << "POST failed: " << post_result.error << std::endl;
    }

    std::cout << "\nAll examples completed!" << std::endl;

    return 0;
}

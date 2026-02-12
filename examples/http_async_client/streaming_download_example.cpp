#include <iostream>
#include <iomanip>
#include <atomic>
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

int main() {
    std::cout << "Streaming Download Example (async_client)\n" << std::endl;

    http::async_client client;

    // =========================================
    // Example 1: Download file with progress
    // =========================================
    std::cout << "Example 1: Async download with progress\n" << std::endl;

    client.download("https://raw.githubusercontent.com/torvalds/linux/master/COPYING",
        "/tmp/linux_license_async.txt",
        [](http::stream_result& result) {
            std::cout << std::endl;  // New line after progress bar
            if (result) {
                std::cout << "Download completed!" << std::endl;
                std::cout << "  Status: " << result.status_code << std::endl;
                std::cout << "  Bytes: " << format_bytes(result.bytes_transferred) << std::endl;
            } else {
                std::cerr << "Download failed: " << result.error << std::endl;
            }
        },
        [](size_t downloaded, size_t total) {
            draw_progress(downloaded, total);
        });

    client.wait();

    // =========================================
    // Example 2: Streaming with callback
    // =========================================
    std::cout << "\n\nExample 2: Streaming GET with callback\n" << std::endl;

    std::atomic<size_t> chunk_count{0};
    std::atomic<size_t> total_bytes{0};

    client.get_streaming("https://api.github.com/users/github",
        [&chunk_count, &total_bytes](const http::stream_info& info) {
            chunk_count++;
            total_bytes += info.data.size();

            std::cout << "  Chunk #" << chunk_count
                      << ": " << info.data.size() << " bytes"
                      << " (total: " << info.downloaded << "/"
                      << (info.total > 0 ? std::to_string(info.total) : "unknown") << ")"
                      << std::endl;

            return true;  // Continue streaming
        },
        [&chunk_count, &total_bytes](http::stream_result& result) {
            if (result) {
                std::cout << "\nStreaming completed!" << std::endl;
                std::cout << "  Total chunks: " << chunk_count << std::endl;
                std::cout << "  Total bytes: " << total_bytes << std::endl;
            } else {
                std::cerr << "Streaming failed: " << result.error << std::endl;
            }
        });

    client.wait();

    // =========================================
    // Example 3: Multiple concurrent downloads
    // =========================================
    std::cout << "\n\nExample 3: Multiple concurrent downloads\n" << std::endl;

    std::atomic<int> completed{0};

    // Download multiple files concurrently
    client.download("https://httpbin.org/bytes/1024",
        "/tmp/file1.bin",
        [&completed](http::stream_result& result) {
            std::cout << "Download 1: " << (result ? "OK" : result.error)
                      << " (" << result.bytes_transferred << " bytes)" << std::endl;
            completed++;
        });

    client.download("https://httpbin.org/bytes/2048",
        "/tmp/file2.bin",
        [&completed](http::stream_result& result) {
            std::cout << "Download 2: " << (result ? "OK" : result.error)
                      << " (" << result.bytes_transferred << " bytes)" << std::endl;
            completed++;
        });

    client.download("https://httpbin.org/bytes/512",
        "/tmp/file3.bin",
        [&completed](http::stream_result& result) {
            std::cout << "Download 3: " << (result ? "OK" : result.error)
                      << " (" << result.bytes_transferred << " bytes)" << std::endl;
            completed++;
        });

    client.wait();
    std::cout << "\nAll " << completed << " downloads completed!" << std::endl;

    return 0;
}

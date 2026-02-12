#include <iostream>
#include <thinger/http_client.hpp>

using namespace thinger;

int main(int argc, char* argv[]) {
    std::cout << "WebSocket Client Example (http::client - synchronous)\n" << std::endl;

    // Default to public echo server
    std::string url = "wss://echo.websocket.org";
    if (argc > 1) {
        url = argv[1];
    }

    http::client client;

    std::cout << "Connecting to " << url << "..." << std::endl;

    // Connect - returns std::optional<websocket_client>
    auto ws = client.websocket(url);

    if (!ws) {
        std::cerr << "Failed to connect to WebSocket server" << std::endl;
        return 1;
    }

    std::cout << "Connected!\n" << std::endl;

    // ============================================
    // Send and receive text message
    // ============================================
    std::cout << "--- Text Messages ---\n" << std::endl;

    std::cout << "Sending: Hello, WebSocket!" << std::endl;
    if (!ws->send_text("Hello, WebSocket!")) {
        std::cerr << "Failed to send message" << std::endl;
        return 1;
    }

    auto [response, is_binary] = ws->receive();

    if (!response.empty()) {
        std::cout << "Received: " << response << std::endl;
        std::cout << "Binary: " << (is_binary ? "yes" : "no") << std::endl;
    }

    // ============================================
    // Multiple messages
    // ============================================
    std::cout << "\n--- Multiple Messages ---\n" << std::endl;

    for (int i = 1; i <= 3; ++i) {
        std::string msg = "Message #" + std::to_string(i);
        std::cout << "Sending: " << msg << std::endl;

        ws->send_text(msg);
        auto [reply, binary] = ws->receive();

        std::cout << "Received: " << reply << std::endl;
    }

    // ============================================
    // Binary message
    // ============================================
    std::cout << "\n--- Binary Message ---\n" << std::endl;

    std::vector<uint8_t> binary_data = {0x48, 0x65, 0x6c, 0x6c, 0x6f}; // "Hello"
    std::cout << "Sending binary: " << binary_data.size() << " bytes" << std::endl;

    ws->send_binary(binary_data.data(), binary_data.size());
    auto [bin_reply, bin_flag] = ws->receive();

    std::cout << "Received: " << bin_reply
              << " (binary: " << (bin_flag ? "yes" : "no") << ")" << std::endl;

    // ============================================
    // Close connection
    // ============================================
    std::cout << "\nClosing connection..." << std::endl;
    ws->close();

    std::cout << "Done!" << std::endl;

    // Note: For async/callback-based WebSocket, use http::async_client
    // See examples/http_async_client/websocket_client_example.cpp

    return 0;
}

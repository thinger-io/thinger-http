#include <iostream>
#include <thinger/http_client.hpp>

using namespace thinger;

int main(int argc, char* argv[]) {
    std::cout << "WebSocket Client Example (async_client)\n" << std::endl;

    // Default to public echo server
    std::string url = "wss://echo.websocket.org";
    if (argc > 1) {
        url = argv[1];
    }

    http::async_client client;

    // ============================================
    // Example 1: Simple callback API
    // ============================================
    std::cout << "=== Callback API ===\n" << std::endl;
    std::cout << "Connecting to " << url << "..." << std::endl;

    client.websocket(url, [](std::shared_ptr<http::websocket_client> ws) {
        if (!ws) {
            std::cerr << "Failed to connect to WebSocket server" << std::endl;
            return;
        }

        std::cout << "Connected!" << std::endl;

        // Send and receive (sync methods work inside callback)
        std::cout << "Sending: Hello from async_client!" << std::endl;
        ws->send_text("Hello from async_client!");

        auto [response, is_binary] = ws->receive();
        std::cout << "Received: " << response << std::endl;

        // Send multiple messages
        for (int i = 1; i <= 3; ++i) {
            std::string msg = "Message #" + std::to_string(i);
            ws->send_text(msg);
            auto [reply, binary] = ws->receive();
            std::cout << "Echo " << i << ": " << reply << std::endl;
        }

        ws->close();
        std::cout << "Connection closed\n" << std::endl;
    });

    client.wait();

    // ============================================
    // Example 2: Coroutine API
    // ============================================
    std::cout << "=== Coroutine API ===\n" << std::endl;
    std::cout << "Connecting to " << url << "..." << std::endl;

    client.run([&client, &url]() -> awaitable<void> {
        auto ws = co_await client.upgrade_websocket(url);

        if (!ws) {
            std::cerr << "Failed to connect" << std::endl;
            co_return;
        }

        std::cout << "Connected!" << std::endl;

        // Use async methods
        co_await ws->send_text_async("Hello from coroutine!");
        auto [response, binary] = co_await ws->receive_async();
        std::cout << "Received: " << response << std::endl;

        co_await ws->close_async();
        std::cout << "Connection closed\n" << std::endl;
    });

    client.wait();

    // ============================================
    // Example 3: Event-driven with message handlers
    // ============================================
    std::cout << "=== Event-Driven API ===\n" << std::endl;
    std::cout << "Connecting to " << url << "..." << std::endl;

    client.websocket(url, [](std::shared_ptr<http::websocket_client> ws) {
        if (!ws) {
            std::cerr << "Failed to connect" << std::endl;
            return;
        }

        std::cout << "Connected!" << std::endl;

        // Set up handlers
        ws->on_message([](const std::string& msg, bool binary) {
            std::cout << "Received: " << msg << std::endl;
        });

        ws->on_close([]() {
            std::cout << "Connection closed by server" << std::endl;
        });

        ws->on_error([](const std::string& error) {
            std::cerr << "Error: " << error << std::endl;
        });

        // Send a message
        ws->send_text("Hello, event-driven!");

        // Start message loop (receives messages via callbacks)
        ws->run();

        // Keep connection alive briefly
        std::this_thread::sleep_for(std::chrono::seconds(1));
    });

    client.wait();

    std::cout << "\nAll examples completed!" << std::endl;

    return 0;
}

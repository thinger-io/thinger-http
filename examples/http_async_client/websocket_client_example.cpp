#include <future>
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

    // The workers the clients run on: the application starts them, and stops them at the end
    asio::workers workers;
    workers.start();

    http::async_client client(workers);

    // ============================================
    // Example 1: Simple callback API
    // ============================================
    std::cout << "=== Callback API ===\n" << std::endl;
    std::cout << "Connecting to " << url << "..." << std::endl;

    // The callback runs on a worker, which runs the io_context of the WebSocket: there the
    // synchronous calls (send_text(), receive(), close()) throw, so it is a coroutine using
    // the coroutine API
    client.websocket(url, [](std::shared_ptr<http::websocket_client> ws) -> awaitable<void> {
        if (!ws) {
            std::cerr << "Failed to connect to WebSocket server" << std::endl;
            co_return;
        }

        std::cout << "Connected!" << std::endl;

        // Send and receive
        std::cout << "Sending: Hello from async_client!" << std::endl;
        co_await ws->send_text_async("Hello from async_client!");

        auto [response, is_binary] = co_await ws->receive_async();
        std::cout << "Received: " << response << std::endl;

        // Send multiple messages
        for (int i = 1; i <= 3; ++i) {
            std::string msg = "Message #" + std::to_string(i);
            co_await ws->send_text_async(msg);
            auto [reply, binary] = co_await ws->receive_async();
            std::cout << "Echo " << i << ": " << reply << std::endl;
        }

        co_await ws->close_async();
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

    // The message loop runs while the websocket_client lives: the application keeps it beyond
    // the callback, and learns from on_close() when the loop ended
    std::shared_ptr<http::websocket_client> socket;
    std::promise<void> closed;
    auto loop_ended = closed.get_future();
    const std::string greeting = "Hello, event-driven!";

    client.websocket(url, [&socket, &closed, &greeting](std::shared_ptr<http::websocket_client> ws) -> awaitable<void> {
        if (!ws) {
            std::cerr << "Failed to connect" << std::endl;
            closed.set_value();
            co_return;
        }

        std::cout << "Connected!" << std::endl;
        socket = ws;

        // Set up handlers: they run on the worker of the WebSocket
        ws->on_message([ws = ws.get(), &greeting](const std::string& msg, bool binary) {
            std::cout << "Received: " << msg << std::endl;
            // Done once the echo arrived: close without waiting
            if (msg == greeting) ws->stop();
        });

        ws->on_close([&closed]() {
            std::cout << "Connection closed" << std::endl;
            closed.set_value();
        });

        ws->on_error([](const std::string& error) {
            std::cerr << "Error: " << error << std::endl;
        });

        // Send a message
        if (!co_await ws->send_text_async(greeting)) {
            std::cerr << "Failed to send" << std::endl;
            closed.set_value();
            co_return;
        }

        // Start message loop (receives messages via callbacks)
        ws->run();
    });

    client.wait();
    loop_ended.wait();

    std::cout << "\nAll examples completed!" << std::endl;

    workers.stop();
    // Once no worker runs it anymore (its message loop may still be ending on one above)
    socket.reset();
    return 0;
}

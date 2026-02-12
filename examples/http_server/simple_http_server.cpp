#include <thinger/http_server.hpp>
#include <thinger/util/logger.hpp>
#include <iostream>

using namespace thinger;

// This example demonstrates the basic usage of the HTTP server


int main(int argc, char* argv[]) {
    // Initialize logging
    LOG_INFO("Starting Simple HTTP Server Example");
    
    // Create server instance
    http::server server;
    
    // Define a simple root endpoint that returns HTML
    server.get("/", [](http::response& res) {
        res.html(
            "<!DOCTYPE html>"
            "<html>"
            "<head>"
            "    <meta charset=\"UTF-8\">"
            "    <title>Thinger HTTP Server</title>"
            "    <style>"
            "        body { font-family: Arial, sans-serif; margin: 40px; }"
            "        .info { background-color: #f0f0f0; padding: 10px; border-radius: 5px; }"
            "    </style>"
            "</head>"
            "<body>"
            "    <h1>Welcome to Thinger HTTP Server!</h1>"
            "    <div class='info'>"
            "        <h2>Server Information</h2>"
            "        <p>This is a simple HTTP server example using thinger-http library.</p>"
            "        <p>The server is running on HTTP (not encrypted).</p>"
            "    </div>"
            "</body>"
            "</html>"
        );
    });
    
    // Get port from command line or use default
    uint16_t port = 8090;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }
    
    // Start server with callback
    std::cout << "Starting HTTP server..." << std::endl;
    
    // Start server and wait for shutdown
    if (!server.start("0.0.0.0", port, [port]() {
        std::cout << "HTTP Server is now listening on http://0.0.0.0:" << port << std::endl;
        std::cout << "Try opening http://localhost:" << port << " in your browser" << std::endl;
        std::cout << "Press Ctrl+C to stop" << std::endl;
    })) {
        std::cerr << "Failed to start server on port " << port << std::endl;
        return 1;
    }
    
    return 0;
}
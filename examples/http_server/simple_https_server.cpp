#include <thinger/http_server.hpp>
#include <thinger/util/logger.hpp>
#include <iostream>

using namespace thinger;

// This example demonstrates the basic usage of the HTTPS server
// with SSL/TLS support

int main(int argc, char* argv[]) {
    // Initialize logging
    LOG_INFO("Starting Simple HTTPS Server Example");
    
    // Create server instance and enable SSL
    http::pool_server server;
    server.enable_ssl(true);
    
    // Define a simple root endpoint that returns HTML
    server.get("/", [](http::response& res) {
        res.html(
            "<!DOCTYPE html>"
            "<html>"
            "<head>"
            "    <meta charset=\"UTF-8\">"
            "    <title>Secure Thinger Server</title>"
            "    <style>"
            "        body { font-family: Arial, sans-serif; margin: 40px; }"
            "        .secure { color: green; }"
            "        .info { background-color: #f0f0f0; padding: 10px; border-radius: 5px; }"
            "    </style>"
            "</head>"
            "<body>"
            "    <h1>Welcome to Thinger HTTPS Server!</h1>"
            "    <p class='secure'>🔒 This connection is secure (HTTPS)</p>"
            "    <div class='info'>"
            "        <h2>Server Information</h2>"
            "        <p>This is a simple HTTPS server example using thinger-http library.</p>"
            "        <p>The server is running with SSL/TLS encryption.</p>"
            "    </div>"
            "</body>"
            "</html>"
        );
    });
    
    // Get port from command line or use default
    uint16_t port = 8443;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }
    
    // Start server with callback
    std::cout << "Starting HTTPS server..." << std::endl;
    
    // Start server
    if (!server.listen("0.0.0.0", port)) {
        std::cerr << "Failed to start HTTPS server on port " << port << std::endl;
        return 1;
    }
    
    std::cout << "HTTPS Server is now listening on https://0.0.0.0:" << port << std::endl;
    std::cout << "Try opening https://localhost:" << port << " in your browser" << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    std::cout << std::endl;
    std::cout << "Note: Your browser will show a security warning because the certificate" << std::endl;
    std::cout << "      is self-signed. This is normal for development." << std::endl;
    
    // Wait for the server
    server.wait();
    
    return 0;
}
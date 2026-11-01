#include <thinger/http_server.hpp>
#include <thinger/asio/ssl/certificate_manager.hpp>
#include <thinger/util/logger.hpp>
#include <iostream>

using namespace thinger;

// This example demonstrates the basic usage of the HTTPS server
// with SSL/TLS support

int main(int argc, char* argv[]) {
    // Initialize logging
    LOG_INFO("Starting Simple HTTPS Server Example");
    
    // The workers the server runs on, and the server
    asio::workers workers;
    workers.start();
    http::pool_server server(workers);
    
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
    
    // Listen with TLS: the certificates of the listener (without one set, a self-signed
    // certificate for development)
    asio::listener_options options;
    options.endpoint = asio::tcp_endpoint{"0.0.0.0", port};
    options.tls = std::make_shared<asio::certificate_manager>();
    if (auto ec = server.listen(std::move(options))) {
        std::cerr << "Failed to start HTTPS server on port " << port << ": " << ec.message() << std::endl;
        return 1;
    }
    
    std::cout << "HTTPS Server is now listening on https://0.0.0.0:" << port << std::endl;
    std::cout << "Try opening https://localhost:" << port << " in your browser" << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    std::cout << std::endl;
    std::cout << "Note: Your browser will show a security warning because the certificate" << std::endl;
    std::cout << "      is self-signed. This is normal for development." << std::endl;
    
    // Wait for Ctrl+C (or SIGTERM), then finish the requests in flight and stop
    int signal = workers.wait();
    std::cout << "Signal " << signal << " received: shutting down" << std::endl;
    server.shutdown(std::chrono::seconds(5));
    workers.stop();
    
    return 0;
}
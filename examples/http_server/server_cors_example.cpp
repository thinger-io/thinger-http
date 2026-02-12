#include <thinger/http_server.hpp>
#include <thinger/util/logger.hpp>
#include <iostream>

using namespace thinger;

int main(int argc, char* argv[]) {
    // Initialize logging
    LOG_INFO("Starting HTTP Server with CORS Example");
    
    // Create server instance
    http::server server;
    
    // Enable CORS - this will automatically add CORS headers to all responses
    server.enable_cors();
    
    // Simple routes to test CORS
    server.get("/", [](http::response& res) {
        res.html(R"(
            <!DOCTYPE html>
            <html>
            <head>
                <title>CORS Test Page</title>
                <script>
                    async function testCORS() {
                        const baseUrl = 'http://localhost:8091';
                        const endpoints = ['/api/data', '/api/users', '/api/protected'];
                        const results = document.getElementById('results');
                        
                        results.innerHTML = '<h3>Testing CORS...</h3>';
                        
                        for (const endpoint of endpoints) {
                            try {
                                const response = await fetch(baseUrl + endpoint);
                                const data = await response.json();
                                
                                results.innerHTML += `
                                    <div style="color: green;">
                                        ✓ ${endpoint}: ${JSON.stringify(data)}
                                    </div>
                                `;
                            } catch (error) {
                                results.innerHTML += `
                                    <div style="color: red;">
                                        ✗ ${endpoint}: ${error.message}
                                    </div>
                                `;
                            }
                        }
                        
                        // Test preflight request
                        try {
                            const response = await fetch(baseUrl + '/api/data', {
                                method: 'POST',
                                headers: {
                                    'Content-Type': 'application/json',
                                    'X-Custom-Header': 'test'
                                },
                                body: JSON.stringify({ test: 'data' })
                            });
                            const data = await response.json();
                            
                            results.innerHTML += `
                                <div style="color: green;">
                                    ✓ POST with preflight: ${JSON.stringify(data)}
                                </div>
                            `;
                        } catch (error) {
                            results.innerHTML += `
                                <div style="color: red;">
                                    ✗ POST with preflight: ${error.message}
                                </div>
                            `;
                        }
                    }
                </script>
            </head>
            <body>
                <h1>CORS Test Page</h1>
                <p>This page will test CORS by making requests to the API endpoints.</p>
                <p>Open the browser console to see detailed CORS headers.</p>
                
                <button onclick='testCORS()'>Test CORS Requests</button>
                
                <div id="results"></div>
                
                <h2>Note:</h2>
                <p>For this test to work properly, serve this page from a different port (e.g., using a simple HTTP server on port 8080):</p>
                <pre>python3 -m http.server 8080</pre>
                <p>Then navigate to http://localhost:8080/ and click the test button.</p>
            </body>
            </html>
        )");
    });
    
    // API endpoints
    server.get("/api/data", [](http::response& res) {
        res.json({
            {"message", "CORS is working!"},
            {"timestamp", std::time(nullptr)},
            {"data", {1, 2, 3, 4, 5}}
        });
    });
    
    server.get("/api/users", [](http::response& res) {
        res.json({
            {"users", {
                {{"id", 1}, {"name", "Alice"}},
                {{"id", 2}, {"name", "Bob"}},
                {{"id", 3}, {"name", "Charlie"}}
            }}
        });
    });
    
    server.post("/api/data", [](nlohmann::json& body, http::response& res) {
        res.json({
            {"received", body},
            {"message", "POST request successful with CORS"},
            {"timestamp", std::time(nullptr)}
        });
    });
    
    // Protected endpoint with Basic Auth
    server.set_basic_auth("/api/protected", "Protected API", "user", "pass");
    
    server.get("/api/protected", [](http::request& req, http::response& res) {
        res.json({
            {"message", "Authenticated with CORS!"},
            {"user", req.get_auth_user()}
        });
    });
    
    // Error endpoint to test CORS on error responses
    server.get("/api/error", [](http::response& res) {
        res.error(http::http_response::status::internal_server_error, 
                 "This is a test error with CORS headers");
    });
    
    // Custom 404 handler to verify CORS works on 404s too
    server.set_not_found_handler([](http::request& req, http::response& res) {
        res.json({
            {"error", "Not Found"},
            {"path", req.get_http_request()->get_uri()},
            {"message", "CORS headers should be present even on 404"}
        }, http::http_response::status::not_found);
    });
    
    // Get port from command line or use default
    uint16_t port = 8091;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }
    
    // Start server
    if (!server.start("0.0.0.0", port)) {
        std::cerr << "Failed to start server on port " << port << std::endl;
        return 1;
    }
    
    std::cout << "CORS-enabled server running on http://0.0.0.0:" << port << std::endl;
    std::cout << "Test CORS by:" << std::endl;
    std::cout << "1. Opening http://localhost:" << port << " in your browser" << std::endl;
    std::cout << "2. Or using curl to check headers:" << std::endl;
    std::cout << "   curl -I http://localhost:" << port << "/api/data" << std::endl;
    std::cout << "   curl -I -X OPTIONS http://localhost:" << port << "/api/data" << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    
    
    return 0;
}
#include <thinger/http_server.hpp>
#include <thinger/util/logger.hpp>
#include <thinger/asio/workers.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>

using namespace thinger;

int main(int argc, char* argv[]) {
    // Initialize logging
    LOG_INFO("Starting HTTP Server with Controlled Shutdown Example");

    // Create server instance (use pointer so we can control its lifetime)
    http::server server;

    // Enable CORS for cross-origin requests
    server.enable_cors();
    
    // Main page with shutdown button
    server.get("/", [](http::response& res) {
        res.html(R"(
            <!DOCTYPE html>
            <html>
            <head>
                <title>Server Control Panel</title>
                <style>
                    body {
                        font-family: Arial, sans-serif;
                        max-width: 800px;
                        margin: 50px auto;
                        padding: 20px;
                    }
                    .status {
                        padding: 10px;
                        margin: 10px 0;
                        border-radius: 5px;
                    }
                    .running {
                        background-color: #d4edda;
                        color: #155724;
                    }
                    .stopped {
                        background-color: #f8d7da;
                        color: #721c24;
                    }
                    button {
                        padding: 10px 20px;
                        font-size: 16px;
                        cursor: pointer;
                        margin: 5px;
                    }
                    .shutdown-btn {
                        background-color: #dc3545;
                        color: white;
                        border: none;
                        border-radius: 5px;
                    }
                    .shutdown-btn:hover {
                        background-color: #c82333;
                    }
                    .shutdown-btn:disabled {
                        background-color: #6c757d;
                        cursor: not-allowed;
                    }
                    #log {
                        background-color: #f8f9fa;
                        border: 1px solid #dee2e6;
                        padding: 10px;
                        margin-top: 20px;
                        height: 200px;
                        overflow-y: auto;
                        font-family: monospace;
                        font-size: 14px;
                    }
                </style>
                <script>
                    let shutdownInitiated = false;
                    
                    function addLog(message) {
                        const log = document.getElementById('log');
                        const time = new Date().toLocaleTimeString();
                        log.innerHTML += time + ' - ' + message + '<br>';
                        log.scrollTop = log.scrollHeight;
                    }
                    
                    async function checkServerStatus() {
                        try {
                            const response = await fetch('/api/status');
                            const data = await response.json();
                            document.getElementById('status').className = 'status running';
                            document.getElementById('status').textContent = 'Server Status: RUNNING';
                            document.getElementById('uptime').textContent = 'Uptime: ' + data.uptime + ' seconds';
                            document.getElementById('requests').textContent = 'Total Requests: ' + data.request_count;
                            
                            if (!shutdownInitiated) {
                                setTimeout(checkServerStatus, 1000);
                            }
                        } catch (error) {
                            document.getElementById('status').className = 'status stopped';
                            document.getElementById('status').textContent = 'Server Status: STOPPED';
                            document.getElementById('uptime').textContent = 'Uptime: N/A';
                            document.getElementById('requests').textContent = 'Total Requests: N/A';
                            
                            if (shutdownInitiated) {
                                addLog('Server has been successfully shut down');
                            } else {
                                addLog('Lost connection to server');
                                setTimeout(checkServerStatus, 3000);
                            }
                        }
                    }
                    
                    async function shutdownServer() {
                        if (!confirm('Are you sure you want to shut down the server?')) {
                            return;
                        }
                        
                        shutdownInitiated = true;
                        document.getElementById('shutdownBtn').disabled = true;
                        addLog('Initiating server shutdown...');
                        
                        try {
                            const response = await fetch('/api/shutdown', { method: 'POST' });
                            const data = await response.json();
                            addLog('Server response: ' + data.message);
                            addLog('Waiting for graceful shutdown...');
                        } catch (error) {
                            addLog('Error during shutdown request: ' + error.message);
                        }
                    }
                    
                    // Start monitoring on page load
                    window.onload = function() {
                        addLog('Control panel loaded');
                        checkServerStatus();
                    }
                </script>
            </head>
            <body>
                <h1>Server Control Panel</h1>
                
                <div id="status" class="status running">Server Status: CHECKING...</div>
                <div id="uptime">Uptime: N/A</div>
                <div id="requests">Total Requests: N/A</div>
                
                <h2>Server Controls</h2>
                <button id='shutdownBtn' class='shutdown-btn' onclick='shutdownServer()'>
                    Shutdown Server
                </button>
                
                <h2>Activity Log</h2>
                <div id="log"></div>
                
                <h2>Test Endpoints</h2>
                <p>You can test these endpoints while the server is running:</p>
                <ul>
                    <li><a href="/api/status" target="_blank">/api/status</a> - Server status</li>
                    <li><a href="/api/health" target="_blank">/api/health</a> - Health check</li>
                    <li><a href="/api/test" target="_blank">/api/test</a> - Test endpoint</li>
                </ul>
            </body>
            </html>
        )");
    });
    
    // Server statistics
    static std::atomic<uint64_t> request_count{0};
    static auto start_time = std::chrono::steady_clock::now();
    
    // Middleware to count requests
    server.use([](http::request& req, http::response& res, std::function<void()> next) {
        request_count++;
        LOG_INFO("Request #%lu: %s %s", 
                request_count.load(), 
                get_method(req.get_http_request()->get_method()).c_str(),
                req.get_http_request()->get_uri().c_str());
        next();
    });
    
    // Status endpoint
    server.get("/api/status", [](http::response& res) {
        auto now = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        
        res.json({
            {"status", "running"},
            {"uptime", uptime},
            {"request_count", request_count.load()},
            {"timestamp", std::time(nullptr)}
        });
    });
    
    // Health check endpoint
    server.get("/api/health", [](http::response& res) {
        res.json({
            {"status", "healthy"},
            {"service", "HTTP Server"},
            {"version", "1.0.0"}
        });
    });
    
    // Test endpoint
    server.get("/api/test", [](http::response& res) {
        res.json({
            {"message", "Test endpoint working"},
            {"random", rand() % 100}
        });
    });
    
    // Shutdown endpoint
    server.post("/api/shutdown", [&](http::response& res) {
        LOG_WARNING("Shutdown requested via API");
        server.stop();
    });
    
    // Custom 404 handler
    server.set_not_found_handler([](http::request& req, http::response& res) {
        res.json({
            {"error", "Not Found"},
            {"path", req.get_http_request()->get_uri()},
            {"timestamp", std::time(nullptr)}
        }, http::http_response::status::not_found);
    });
    
    // Get port from command line or use default
    uint16_t port = 8092;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }
    
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
#include <thinger/http_server.hpp>
#include <thinger/util/logger.hpp>
#include <iostream>
#include <set>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>

using namespace thinger;

// Store active SSE connections
std::mutex connections_mutex;
std::set<std::shared_ptr<http::sse_connection>> active_connections;
std::atomic<int> event_counter{0};

// Background thread for sending periodic events
void event_broadcaster() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        int event_id = ++event_counter;
        std::string message = "Server time: " + std::to_string(std::time(nullptr)) + 
                             ", Event #" + std::to_string(event_id);
        
        // Send to all connected clients
        std::lock_guard<std::mutex> lock(connections_mutex);
        std::vector<std::shared_ptr<http::sse_connection>> dead_connections;
        
        for (const auto& conn : active_connections) {
            try {
                conn->send_event("time-update");
                conn->send_data(message);
            } catch (const std::exception&) {
                // Connection is dead, mark for removal
                dead_connections.push_back(conn);
            }
        }
        
        // Remove dead connections
        for (const auto& dead : dead_connections) {
            active_connections.erase(dead);
        }
        
        if (!active_connections.empty()) {
            LOG_INFO("Sent event #%d to %zu SSE clients", event_id, active_connections.size());
        }
    }
}

int main(int argc, char* argv[]) {
    // Initialize logging
    LOG_INFO("Starting HTTP Server with SSE Example");
    
    // Create server instance
    http::server server;
    
    // Enable CORS for SSE connections from browsers
    server.enable_cors();
    
    // Start event broadcaster thread
    std::thread broadcaster(event_broadcaster);
    broadcaster.detach();
    
    // Main page with SSE client
    server.get("/", [](http::response& res) {
        res.html(R"(
            <!DOCTYPE html>
            <html>
            <head>
                <title>Server-Sent Events Test</title>
                <style>
                    body {
                        font-family: Arial, sans-serif;
                        max-width: 800px;
                        margin: 50px auto;
                        padding: 20px;
                    }
                    #events {
                        border: 1px solid #ccc;
                        height: 400px;
                        overflow-y: auto;
                        padding: 10px;
                        margin-bottom: 10px;
                        background-color: #f9f9f9;
                    }
                    .event {
                        margin: 5px 0;
                        padding: 5px;
                        border-radius: 3px;
                    }
                    .time-update {
                        background-color: #e3f2fd;
                    }
                    .custom {
                        background-color: #f5f5f5;
                    }
                    .connected {
                        background-color: #d4edda;
                        font-weight: bold;
                    }
                    .disconnected {
                        background-color: #f8d7da;
                        font-weight: bold;
                    }
                    #status {
                        margin-bottom: 10px;
                        font-weight: bold;
                    }
                    .status-connected {
                        color: green;
                    }
                    .status-disconnected {
                        color: red;
                    }
                    button {
                        padding: 5px 15px;
                        margin-right: 10px;
                    }
                </style>
                <script>
                    let eventSource = null;
                    let eventCount = 0;
                    
                    function updateStatus(connected) {
                        const status = document.getElementById('status');
                        status.className = connected ? 'status-connected' : 'status-disconnected';
                        status.textContent = connected ? 'Connected' : 'Disconnected';
                        
                        document.getElementById('connectBtn').disabled = connected;
                        document.getElementById('disconnectBtn').disabled = !connected;
                        document.getElementById('triggerBtn').disabled = !connected;
                    }
                    
                    function addEvent(text, type = 'custom') {
                        const events = document.getElementById('events');
                        const event = document.createElement('div');
                        event.className = 'event ' + type;
                        const timestamp = new Date().toLocaleTimeString();
                        event.textContent = '[' + timestamp + '] ' + text;
                        events.appendChild(event);
                        events.scrollTop = events.scrollHeight;
                        eventCount++;
                        document.getElementById('eventCount').textContent = eventCount;
                    }
                    
                    function connect() {
                        if (eventSource) {
                            eventSource.close();
                        }
                        
                        eventSource = new EventSource('/events');
                        
                        eventSource.onopen = function() {
                            addEvent('Connected to SSE server', 'connected');
                            updateStatus(true);
                        };
                        
                        eventSource.onerror = function(error) {
                            addEvent('Connection error or closed', 'disconnected');
                            updateStatus(false);
                        };
                        
                        // Listen for time-update events
                        eventSource.addEventListener('time-update', function(event) {
                            addEvent('Time Update: ' + event.data, 'time-update');
                        });
                        
                        // Listen for custom events
                        eventSource.addEventListener('custom', function(event) {
                            addEvent('Custom Event: ' + event.data, 'custom');
                        });
                        
                        // Default message handler (no event type)
                        eventSource.onmessage = function(event) {
                            addEvent('Message: ' + event.data, 'custom');
                        };
                    }
                    
                    function disconnect() {
                        if (eventSource) {
                            eventSource.close();
                            eventSource = null;
                            addEvent('Disconnected by user', 'disconnected');
                            updateStatus(false);
                        }
                    }
                    
                    function triggerCustomEvent() {
                        fetch('/trigger-event', {
                            method: 'POST',
                            headers: {
                                'Content-Type': 'application/json'
                            },
                            body: JSON.stringify({
                                message: 'User triggered event at ' + new Date().toLocaleTimeString()
                            })
                        })
                        .then(response => response.json())
                        .then(data => {
                            console.log('Trigger response:', data);
                        })
                        .catch(error => {
                            console.error('Error triggering event:', error);
                        });
                    }
                    
                    // Auto-connect on page load
                    window.onload = function() {
                        updateStatus(false);
                    }
                </script>
            </head>
            <body>
                <h1>Server-Sent Events Test Client</h1>
                
                <div>
                    <button id='connectBtn' onclick='connect()'>Connect</button>
                    <button id='disconnectBtn' onclick='disconnect()' disabled>Disconnect</button>
                    <button id='triggerBtn' onclick='triggerCustomEvent()' disabled>Trigger Custom Event</button>
                </div>
                
                <p>Status: <span id="status" class="status-disconnected">Disconnected</span></p>
                <p>Total Events Received: <span id="eventCount">0</span></p>
                
                <div id="events"></div>
                
                <h2>Test Instructions</h2>
                <ol>
                    <li>Click "Connect" to establish SSE connection</li>
                    <li>You will receive automatic time updates every 5 seconds</li>
                    <li>Click "Trigger Custom Event" to send a custom event to all clients</li>
                    <li>Open multiple browser tabs to see events broadcast to all clients</li>
                    <li>The connection will automatically reconnect if dropped</li>
                </ol>
            </body>
            </html>
        )");
    });
    
    // SSE endpoint
    server.get("/events", [](http::response& res) {
        res.start_sse([](std::shared_ptr<http::sse_connection> sse) {
            LOG_INFO("New SSE connection established");
            
            // Add to active connections
            {
                std::lock_guard<std::mutex> lock(connections_mutex);
                active_connections.insert(sse);
            }
            
            // Send initial connection message
            sse->send_event("custom");
            sse->send_data("Welcome! You are now connected to the SSE server.");
            
            // Send retry interval (reconnect after 3 seconds if connection drops)
            sse->send_retry(3000);
        });
    });
    
    // Endpoint to trigger custom events
    server.post("/trigger-event", [](http::request& req, http::response& res) {
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.get_http_request()->get_body());
        } catch (const std::exception&) {
            body = nlohmann::json::object();
        }
        std::string message = "No message provided";
        
        if (body.contains("message")) {
            message = body["message"].get<std::string>();
        }
        
        // Send custom event to all connected clients
        std::lock_guard<std::mutex> lock(connections_mutex);
        std::vector<std::shared_ptr<http::sse_connection>> dead_connections;
        
        for (const auto& conn : active_connections) {
            try {
                conn->send_event("custom");
                conn->send_data(message);
            } catch (const std::exception&) {
                dead_connections.push_back(conn);
            }
        }
        
        // Remove dead connections
        for (const auto& dead : dead_connections) {
            active_connections.erase(dead);
        }
        
        res.json({
            {"status", "success"},
            {"clients_notified", active_connections.size()},
            {"message", message}
        });
    });
    
    // API status endpoint
    server.get("/api/status", [](http::response& res) {
        std::lock_guard<std::mutex> lock(connections_mutex);
        res.json({
            {"sse_connections", active_connections.size()},
            {"events_sent", event_counter.load()},
            {"server", "running"}
        });
    });
    
    // Get port from command line or use default
    uint16_t port = 8094;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }
    
    // Start server
    std::cout << "SSE server running on http://0.0.0.0:" << port << std::endl;
    std::cout << "Open http://localhost:" << port << " in your browser to test" << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    
    // Start server and wait for shutdown
    if (!server.start("0.0.0.0", port)) {
        std::cerr << "Failed to start server on port " << port << std::endl;
        return 1;
    }
    
    return 0;
}
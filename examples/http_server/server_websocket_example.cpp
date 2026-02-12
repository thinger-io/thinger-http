#include <thinger/http_server.hpp>
#include <thinger/util/logger.hpp>
#include <iostream>
#include <set>
#include <mutex>
#include <algorithm>

using namespace thinger;

// Store active WebSocket connections
std::mutex connections_mutex;
std::set<std::shared_ptr<http::websocket_connection>> active_connections;

int main(int argc, char* argv[]) {
    // Initialize logging
    LOG_INFO("Starting HTTP Server with WebSocket Example");
    
    // Create server instance
    http::server server;
    
    // Enable CORS for WebSocket connections from browsers
    server.enable_cors();
    
    // Main page with WebSocket client
    server.get("/", [](http::response& res) {
        res.html(R"(
            <!DOCTYPE html>
            <html>
            <head>
                <title>WebSocket Test</title>
                <style>
                    body {
                        font-family: Arial, sans-serif;
                        max-width: 800px;
                        margin: 50px auto;
                        padding: 20px;
                    }
                    #messages {
                        border: 1px solid #ccc;
                        height: 300px;
                        overflow-y: auto;
                        padding: 10px;
                        margin-bottom: 10px;
                        background-color: #f9f9f9;
                    }
                    .message {
                        margin: 5px 0;
                        padding: 5px;
                        border-radius: 3px;
                    }
                    .sent {
                        background-color: #e3f2fd;
                        text-align: right;
                    }
                    .received {
                        background-color: #f5f5f5;
                    }
                    .system {
                        background-color: #fff3cd;
                        font-style: italic;
                    }
                    #messageInput {
                        width: 70%;
                        padding: 5px;
                    }
                    button {
                        padding: 5px 15px;
                        margin-left: 5px;
                    }
                    #status {
                        margin-bottom: 10px;
                        font-weight: bold;
                    }
                    .connected {
                        color: green;
                    }
                    .disconnected {
                        color: red;
                    }
                </style>
                <script>
                    let ws = null;
                    let messageCount = 0;
                    
                    function updateStatus(connected) {
                        const status = document.getElementById('status');
                        status.className = connected ? 'connected' : 'disconnected';
                        status.textContent = connected ? 'Connected' : 'Disconnected';
                        
                        document.getElementById('connectBtn').disabled = connected;
                        document.getElementById('disconnectBtn').disabled = !connected;
                        document.getElementById('sendBtn').disabled = !connected;
                        document.getElementById('broadcastBtn').disabled = !connected;
                    }
                    
                    function addMessage(text, type = 'received') {
                        const messages = document.getElementById('messages');
                        const message = document.createElement('div');
                        message.className = 'message ' + type;
                        message.textContent = text;
                        messages.appendChild(message);
                        messages.scrollTop = messages.scrollHeight;
                    }
                    
                    function connect() {
                        const protocol = document.getElementById('protocol').value;
                        const wsUrl = 'ws://localhost:' + window.location.port + '/ws/echo';
                        
                        ws = protocol ? new WebSocket(wsUrl, protocol) : new WebSocket(wsUrl);
                        
                        ws.onopen = function() {
                            addMessage('Connected to WebSocket server', 'system');
                            updateStatus(true);
                        };
                        
                        ws.onmessage = function(event) {
                            addMessage('Received: ' + event.data, 'received');
                        };
                        
                        ws.onerror = function(error) {
                            addMessage('Error: ' + error, 'system');
                        };
                        
                        ws.onclose = function() {
                            addMessage('Disconnected from server', 'system');
                            updateStatus(false);
                        };
                    }
                    
                    function disconnect() {
                        if (ws) {
                            ws.close();
                        }
                    }
                    
                    function sendMessage() {
                        const input = document.getElementById('messageInput');
                        const message = input.value.trim();
                        
                        if (message && ws && ws.readyState === WebSocket.OPEN) {
                            ws.send(message);
                            addMessage('Sent: ' + message, 'sent');
                            input.value = '';
                            messageCount++;
                        }
                    }
                    
                    function sendBroadcast() {
                        if (ws && ws.readyState === WebSocket.OPEN) {
                            const message = 'Broadcast message #' + (++messageCount);
                            ws.send('BROADCAST:' + message);
                            addMessage('Sent broadcast: ' + message, 'sent');
                        }
                    }
                    
                    // Send message on Enter key
                    window.onload = function() {
                        document.getElementById('messageInput').addEventListener('keypress', function(e) {
                            if (e.key === 'Enter') {
                                sendMessage();
                            }
                        });
                        updateStatus(false);
                    }
                </script>
            </head>
            <body>
                <h1>WebSocket Test Client</h1>
                
                <div>
                    <label>Protocol (optional): 
                        <select id="protocol">
                            <option value="">None</option>
                            <option value="chat">chat</option>
                            <option value="echo">echo</option>
                        </select>
                    </label>
                    <button id="connectBtn" onclick="connect();">Connect</button>
                    <button id="disconnectBtn" onclick="disconnect();" disabled>Disconnect</button>
                </div>
                
                <p id="status" class="disconnected">Disconnected</p>
                
                <div id="messages"></div>
                
                <div>
                    <input type="text" id="messageInput" placeholder="Enter message..." />
                    <button id="sendBtn" onclick="sendMessage();" disabled>Send</button>
                    <button id="broadcastBtn" onclick="sendBroadcast();" disabled>Send Broadcast</button>
                </div>
                
                <h2>Test Instructions</h2>
                <ol>
                    <li>Click "Connect" to establish WebSocket connection</li>
                    <li>Type a message and click "Send" or press Enter</li>
                    <li>The server will echo back your message</li>
                    <li>Click "Send Broadcast" to send a message to all connected clients</li>
                    <li>Open multiple browser tabs to test broadcasting</li>
                </ol>
            </body>
            </html>
        )");
    });
    
    // WebSocket echo endpoint
    server.get("/ws/echo", [](http::request& req, http::response& res) {
        // Supported protocols (optional)
        std::set<std::string> protocols = {"chat", "echo"};
        
        // Upgrade to WebSocket
        res.upgrade_websocket([&req](std::shared_ptr<http::websocket_connection> ws) {
            LOG_INFO("New WebSocket connection from %s", req.get_request_ip().c_str());
            
            // Add to active connections
            {
                std::lock_guard<std::mutex> lock(connections_mutex);
                active_connections.insert(ws);
            }
            
            // Set up message handler
            ws->on_message([ws](const std::string& message, bool binary) {
                if (!binary) {  // Handle text messages
                    LOG_DEBUG("Received WebSocket message: %s", message.c_str());
                    
                    // Check if it's a broadcast message
                    if (message.starts_with("BROADCAST:")) {
                        std::string broadcast_msg = message.substr(10);
                        
                        // Send to all connected clients
                        std::lock_guard<std::mutex> lock(connections_mutex);
                        std::vector<std::shared_ptr<http::websocket_connection>> dead_connections;
                        
                        for (const auto& conn : active_connections) {
                            if (conn != ws) { // Don't echo back to sender
                                try {
                                    conn->send_text("Broadcast from another client: " + broadcast_msg);
                                } catch (const std::exception&) {
                                    // Connection is dead, mark for removal
                                    dead_connections.push_back(conn);
                                }
                            }
                        }
                        
                        // Remove dead connections
                        for (const auto& dead : dead_connections) {
                            active_connections.erase(dead);
                        }
                        
                        // Confirm to sender
                        ws->send_text("Your broadcast was sent to " + 
                                      std::to_string(active_connections.size() - 1) + " other clients");
                    } else {
                        // Simple echo
                        ws->send_text("Echo: " + message);
                    }
                }
            });
            
            // Note: Since websocket_connection doesn't have an on_close callback,
            // dead connections will be cleaned up when we try to send to them
            // and the send operation fails
            
            // Send welcome message
            ws->send_text("Welcome to the WebSocket echo server!");
            ws->send_text("Send 'BROADCAST:message' to broadcast to all connected clients");
        }, protocols);
    });
    
    // WebSocket endpoint without protocol negotiation
    server.get("/ws/simple", [](http::request& req, http::response& res) {
        res.upgrade_websocket([&req](std::shared_ptr<http::websocket_connection> ws) {
            LOG_INFO("Simple WebSocket connection from %s", req.get_request_ip().c_str());
            
            ws->on_message([ws](const std::string& message, bool binary) {
                if (!binary) {
                    ws->send_text("You said: " + message);
                }
            });
            
            ws->send_text("Connected to simple WebSocket endpoint");
        });
    });
    
    // API status endpoint
    server.get("/api/status", [](http::response& res) {
        std::lock_guard<std::mutex> lock(connections_mutex);
        res.json({
            {"websocket_connections", active_connections.size()},
            {"server", "running"}
        });
    });
    
    // Get port from command line or use default
    uint16_t port = 8093;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }
    
    // Start server
    std::cout << "WebSocket server running on http://0.0.0.0:" << port << std::endl;
    std::cout << "Open http://localhost:" << port << " in your browser to test" << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    
    // Start server and wait for shutdown
    if (!server.start("0.0.0.0", port)) {
        std::cerr << "Failed to start server on port " << port << std::endl;
        return 1;
    }
    
    return 0;
}
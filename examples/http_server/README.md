# HTTP Server Examples

Examples demonstrating HTTP server functionality including WebSocket and SSE.

## Examples

### simple_http_server

Basic HTTP server with routing.

```bash
./simple_http_server [port]
```

```cpp
http::server server;

server.get("/", [](auto& req, auto& res) {
    res.send("Hello World!");
});

server.get("/api/users/:id", [](auto& req, auto& res) {
    auto id = req["id"];
    res.json({{"id", id}, {"name", "User"}});
});

server.start("0.0.0.0", 8080);
```

### simple_https_server

HTTPS server with TLS certificates.

```bash
./simple_https_server [port]
```

### server_websocket_example

WebSocket server with echo and broadcast functionality.

```bash
./server_websocket_example [port]
# Open http://localhost:8093 in browser
```

```cpp
server.get("/ws/echo", [](auto& req, auto& res) {
    res.upgrade_websocket([](auto ws) {
        ws->on_message([ws](const std::string& msg, bool binary) {
            ws->send_text("Echo: " + msg);
        });
        ws->send_text("Welcome!");
    });
});
```

### server_sse_example

Server-Sent Events for real-time updates.

```bash
./server_sse_example [port]
```

```cpp
server.get("/events", [](auto& req, auto& res) {
    res.start_sse([](auto sse) {
        sse->send("event-name", "data payload");
    });
});
```

### routing_example

URL routing with parameters and wildcards.

```bash
./routing_example [port]
```

```cpp
// Path parameters
server.get("/users/:id", handler);

// Query parameters
server.get("/search", [](auto& req, auto& res) {
    auto query = req.query("q");
});

// Multiple methods
server.route("/resource")
    .get(handleGet)
    .post(handlePost)
    .del(handleDelete);
```

### server_auth_example

Basic HTTP authentication.

```bash
./server_auth_example [port]
```

### server_cors_example

Cross-Origin Resource Sharing configuration.

```bash
./server_cors_example [port]
```

```cpp
server.enable_cors();  // Allow all origins

// Or configure specific origins
server.enable_cors({
    .origins = {"https://example.com"},
    .methods = {"GET", "POST"},
    .headers = {"Content-Type", "Authorization"}
});
```

### server_shutdown_example

Graceful server shutdown handling.

```bash
./server_shutdown_example [port]
```

### advanced_http_server

Complete example with multiple features.

```bash
./advanced_http_server [port]
```

## API Overview

### Server Setup

```cpp
http::server server;

// Routes
server.get("/path", handler);
server.post("/path", handler);
server.put("/path", handler);
server.del("/path", handler);

// Start
server.start("0.0.0.0", 8080);  // Blocking
server.listen("0.0.0.0", 8080); // Non-blocking
server.wait();                   // Wait for shutdown
server.stop();                   // Stop server
```

### Request Object

```cpp
void handler(http::request& req, http::response& res) {
    req["param"];           // Path parameter
    req.query("key");       // Query parameter
    req.header("name");     // Request header
    req.body();             // Request body
    req.json();             // Parse body as JSON
    req.get_request_ip();   // Client IP
}
```

### Response Object

```cpp
void handler(http::request& req, http::response& res) {
    res.send("text");                    // Plain text
    res.json({{"key", "value"}});        // JSON
    res.html("<h1>Hello</h1>");          // HTML
    res.file("/path/to/file");           // File
    res.status(404);                     // Set status
    res.header("X-Custom", "value");     // Set header
    res.redirect("/other");              // Redirect
    res.error(500, "Error message");     // Error response
}
```

### WebSocket Upgrade

```cpp
res.upgrade_websocket([](std::shared_ptr<http::websocket_connection> ws) {
    ws->on_message([ws](const std::string& msg, bool binary) {
        ws->send_text("response");
        ws->send_binary("data");
    });
    ws->send_text("welcome");
});
```

### Server-Sent Events

```cpp
res.start_sse([](std::shared_ptr<http::sse_connection> sse) {
    sse->send("event", "data");
    sse->send("data only");
});
```

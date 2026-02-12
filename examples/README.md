# Examples

This directory contains examples demonstrating how to use the thinger-http library.

## Directory Structure

```
examples/
├── http_client/     # HTTP/WebSocket client examples
├── http_server/     # HTTP server examples (including WebSocket/SSE)
└── logging_example.cpp  # Logging configuration example
```

## Building Examples

Examples need to be explicitly enabled:

```bash
mkdir build && cd build
cmake .. -DTHINGER_HTTP_BUILD_EXAMPLES=ON
cmake --build .
```

Executables will be in `build/examples/`.

## Quick Start

### HTTP Client
```cpp
#include <thinger/http_client.hpp>

thinger::http::client client;
auto res = client.get("https://api.github.com/users/github");
if (res) {
    std::cout << res.json()["login"] << std::endl;
}
```

### WebSocket Client
```cpp
#include <thinger/http_client.hpp>

// Via http::client (sync)
thinger::http::client client;
if (auto ws = client.websocket("wss://echo.websocket.org")) {
    ws->send_text("Hello!");
    auto [msg, binary] = ws->receive();
}

// Via http::async_client (async callback)
thinger::http::async_client pool;
pool.websocket("wss://echo.websocket.org", [](auto ws) {
    if (ws) {
        ws->send_text("Hello!");
        auto [msg, _] = ws->receive();
    }
});
pool.wait();
```

### HTTP Server
```cpp
#include <thinger/http_server.hpp>

thinger::http::server server;
server.get("/", [](auto& req, auto& res) {
    res.send("Hello World!");
});
server.start("0.0.0.0", 8080);
```

## Examples List

| Example | Description |
|---------|-------------|
| `simple_http_client` | Basic HTTP GET requests |
| `multiple_requests` | Multiple sequential/parallel requests |
| `timeout_example` | Request timeout configuration |
| `websocket_client_example` | WebSocket client usage |
| `simple_http_server` | Basic HTTP server |
| `simple_https_server` | HTTPS server with TLS |
| `server_websocket_example` | WebSocket server with echo/broadcast |
| `server_sse_example` | Server-Sent Events |
| `routing_example` | URL routing and parameters |
| `server_auth_example` | Basic authentication |
| `server_cors_example` | CORS configuration |
| `logging_example` | Logging setup with spdlog |

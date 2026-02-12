![hola](/assets/thinger-http-banner.svg "thinger-http")

[![CI](https://github.com/thinger-io/thinger-http/actions/workflows/ci.yml/badge.svg)](https://github.com/thinger-io/thinger-http/actions/workflows/ci.yml)

A modern C++20 HTTP/WebSocket library built on Boost.ASIO with coroutine support.

> [!WARNING]
> This library is under active development and is not yet recommended for production use. APIs and interfaces may change without notice between versions. Feedback and contributions are welcome.

## Performance

Benchmarked with [bombardier](https://github.com/codesenberg/bombardier) — 100 concurrent connections, 10s duration, "Hello World!" endpoint. All frameworks compiled with `-O3` (Release mode) on Apple M2 Max.

| Framework | Req/s | Avg Latency | Throughput |
|---|--:|--:|--:|
| **thinger-http** | **~131,000** | **0.76ms** | 20.4 MB/s |
| [Crow](https://github.com/CrowCpp/Crow) | ~122,000 | 0.82ms | 22.7 MB/s |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | ~34,000 | 3.89ms | 3.6 MB/s |

> Both thinger-http and Crow run multi-threaded. See [`benchmark/`](benchmark/) for details and instructions to reproduce.

## Features

- **HTTP Server & Client** - Full HTTP/1.1 support with connection pooling
- **WebSocket** - Server and client support with text/binary messages
- **Server-Sent Events (SSE)** - Real-time server push
- **SSL/TLS** - HTTPS and WSS support via OpenSSL
- **C++20 Coroutines** - Async-first API with sync wrappers
- **Lightweight** - Minimal dependencies, suitable for embedded systems

## Quick Start

### HTTP Server

```cpp
#include <thinger/http_server.hpp>

int main() {
    thinger::http::server server;

    server.get("/", [](auto& req, auto& res) {
        res.send("Hello World!");
    });

    server.get("/api/users/:id", [](auto& req, auto& res) {
        res.json({{"id", req["id"]}, {"name", "John"}});
    });

    server.start("0.0.0.0", 8080);
}
```

### HTTP Client

```cpp
#include <thinger/http_client.hpp>

int main() {
    thinger::http::client client;

    auto res = client.get("https://api.github.com/users/github");
    if (res) {
        std::cout << res.json()["login"] << std::endl;
    }
}
```

### WebSocket Client

```cpp
#include <thinger/http_client.hpp>

int main() {
    thinger::http::client client;

    // Connect via client.websocket() - returns std::optional<websocket_client>
    if (auto ws = client.websocket("wss://echo.websocket.org")) {
        ws->send_text("Hello!");
        auto [msg, binary] = ws->receive();
        std::cout << "Received: " << msg << std::endl;
        ws->close();
    }
}
```

## Installation

### Requirements

- C++20 compiler (GCC 11+, Clang 14+, MSVC 2022+)
- CMake 3.14+
- Boost 1.80+ (ASIO with coroutines, iostreams)
- ZLIB (HTTP compression via Boost.Iostreams)
- OpenSSL (for HTTPS/WSS)

### CMake Integration

```cmake
include(FetchContent)
FetchContent_Declare(
    thinger-http
    GIT_REPOSITORY https://github.com/thinger-io/thinger-http.git
    GIT_TAG main
)
FetchContent_MakeAvailable(thinger-http)

target_link_libraries(your_target PRIVATE thinger::http)
```

### Build from Source

```bash
git clone https://github.com/thinger-io/thinger-http.git
cd thinger-http
mkdir build && cd build
cmake -DTHINGER_HTTP_BUILD_EXAMPLES=ON ..
cmake --build .
```

### Run an Example

```bash
# Start a simple HTTP server on port 8080
./examples/http_server/simple_http_server

# In another terminal, make a request
curl http://localhost:8080/
```

## HTTP Server

### Basic Routing

```cpp
thinger::http::server server;

// HTTP methods
server.get("/path", handler);
server.post("/path", handler);
server.put("/path", handler);
server.del("/path", handler);
server.patch("/path", handler);
server.options("/path", handler);

// Handler signature
void handler(thinger::http::request& req, thinger::http::response& res);
```

### Path Parameters

```cpp
server.get("/users/:id", [](auto& req, auto& res) {
    std::string id = req["id"];
    res.json({{"user_id", id}});
});

server.get("/files/*path", [](auto& req, auto& res) {
    std::string path = req["path"];  // Wildcard capture
    res.send_file("/data/" + path);
});
```

### Query Parameters

```cpp
server.get("/search", [](auto& req, auto& res) {
    // Get query parameter (returns empty string if not found)
    std::string query = req.query("q");

    // Get query parameter with default value
    std::string page = req.query("page", "1");

    res.json({{"query", query}, {"page", std::stoi(page)}});
});
```

### Request Body

```cpp
server.post("/api/data", [](auto& req, auto& res) {
    // Raw body as string
    std::string body = req.body();

    // Parse body as JSON
    auto json = req.json();
    std::string name = json["name"];

    // Get request header
    std::string content_type = req.header("Content-Type");

    res.json({{"received", name}});
});
```

### Response Types

```cpp
// Plain text
res.send("Hello World!");

// JSON
res.json({{"status", "ok"}, {"count", 42}});

// HTML
res.html("<h1>Welcome</h1>");

// File
res.send_file("/path/to/file.pdf");

// Custom status and headers
res.status(http::http_response::status::created);
res.header("X-Custom", "value");
res.json({{"created", true}});

// Redirect
res.redirect("/new-location");

// Error
res.error(http::http_response::status::not_found, "Not found");
```

### Static Files

```cpp
// Serve directory with fallback to index.html
server.serve_static("/static", "/var/www/static");

// Without fallback to index
server.serve_static("/assets", "/var/www/assets", false);
```

### CORS

```cpp
// Enable CORS for all origins
server.enable_cors();

// Disable CORS
server.enable_cors(false);
```

### HTTPS Server

```cpp
thinger::http::server server;

server.get("/", handler);

// Enable SSL/TLS (requires certificates configured in socket layer)
server.enable_ssl(true);
server.start("0.0.0.0", 443);
```

### Server Lifecycle

```cpp
// Blocking start
server.start("0.0.0.0", 8080);

// Non-blocking
server.listen("0.0.0.0", 8080);
// ... do other work ...
server.wait();  // Wait for shutdown

// Stop server
server.stop();
```

## HTTP Client

### Basic Requests

```cpp
thinger::http::client client;

// GET
auto res = client.get("https://api.example.com/data");

// POST with JSON
auto res = client.post("https://api.example.com/users",
    R"({"name": "John"})", "application/json");

// PUT
auto res = client.put(url, body, content_type);

// DELETE
auto res = client.del(url);
```

### Configuration

```cpp
client.timeout(std::chrono::seconds(30));
client.follow_redirects(true);
client.max_redirects(10);
client.verify_ssl(false);  // Disable SSL verification
```

### Request Builder (Fluent API)

For complex requests with custom headers, body, and other options:

```cpp
thinger::http::client client;

// GET with custom headers
auto res = client.request("https://api.example.com/data")
    .header("Authorization", "Bearer token123")
    .header("X-Custom", "value")
    .get();

// POST with JSON body
auto res = client.request("https://api.example.com/users")
    .header("Authorization", "Bearer token")
    .body(R"({"name": "John", "email": "john@example.com"})", "application/json")
    .post();

// Streaming download with progress
auto result = client.request("https://example.com/large-file.zip")
    .get([](const http::stream_info& info) {
        std::cout << "Downloaded: " << info.downloaded << "/" << info.total << std::endl;
        return true;  // continue downloading
    });

// Download to file
auto result = client.request("https://example.com/file.zip")
    .download("/path/to/file.zip", [](size_t downloaded, size_t total) {
        std::cout << (downloaded * 100 / total) << "%" << std::endl;
    });
```

### Response Handling

```cpp
auto res = client.get(url);

if (res) {  // or res.ok()
    int status = res.status();
    std::string body = res.body();
    std::string type = res.content_type();
    size_t length = res.content_length();

    // JSON parsing
    if (res.is_json()) {
        auto json = res.json();
    }

    // Headers
    std::string header = res.header("X-Custom");
} else {
    std::string error = res.error();
}
```

### Async Client (async_client)

The async client supports three usage patterns: callbacks, coroutines, and the request builder.

#### Callback-based

```cpp
thinger::http::async_client client;

// Simple callback
client.get(url, [](auto& res) {
    if (res.ok()) {
        std::cout << res.body() << std::endl;
    }
});

// Multiple concurrent requests
client.get(url1, callback1);
client.get(url2, callback2);
client.get(url3, callback3);

// Wait for all requests
client.wait();
```

#### Coroutines (co_await)

```cpp
thinger::http::async_client client;

client.run([&]() -> awaitable<void> {
    // Simple request
    auto res = co_await client.get("https://api.example.com/data");

    // With request builder
    auto res2 = co_await client.request("https://api.example.com/users")
        .header("Authorization", "Bearer token")
        .body(R"({"name": "John"})")
        .post();
});

client.wait();
```

#### Request Builder with Callbacks

```cpp
thinger::http::async_client client;

// Fluent API with callback (no coroutines needed)
client.request("https://api.example.com/data")
    .header("Authorization", "Bearer token")
    .header("X-Custom", "value")
    .get([](auto& res) {
        std::cout << res.body() << std::endl;
    });

// POST with builder and callback
client.request("https://api.example.com/users")
    .body(R"({"name": "John"})", "application/json")
    .post([](auto& res) {
        if (res.ok()) {
            std::cout << "User created!" << std::endl;
        }
    });

client.wait();
```

## WebSocket

### WebSocket Server

```cpp
server.get("/ws", [](auto& req, auto& res) {
    res.upgrade_websocket([](std::shared_ptr<http::websocket_connection> ws) {
        // Message handler
        ws->on_message([ws](const std::string& msg, bool binary) {
            if (binary) {
                ws->send_binary("response");
            } else {
                ws->send_text("Echo: " + msg);
            }
        });

        // Send welcome message
        ws->send_text("Connected!");
    });
});
```

### WebSocket Server with Protocols

```cpp
std::set<std::string> protocols = {"chat", "json"};

res.upgrade_websocket([](auto ws) {
    ws->on_message([ws](const std::string& msg, bool binary) {
        ws->send_text(msg);
    });
}, protocols);
```

### WebSocket Client (Sync)

```cpp
thinger::http::client client;

// Simple connect - returns std::optional<websocket_client>
if (auto ws = client.websocket("wss://server.com/ws")) {
    // Send messages
    ws->send_text("Hello");
    ws->send_binary(data, size);

    // Receive
    auto [msg, is_binary] = ws->receive();

    // Close
    ws->close();
}

// With subprotocol
if (auto ws = client.websocket("wss://server.com/ws", "graphql-ws")) {
    // ...
}

// With custom headers (using request builder)
if (auto ws = client.request("wss://server.com/ws")
        .header("Authorization", "Bearer token")
        .header("X-Custom", "value")
        .protocol("graphql-ws")
        .websocket()) {
    ws->send_text("Hello!");
}
```

### WebSocket Client (Async Callback)

```cpp
thinger::http::async_client client;

// Simple connect
client.websocket("wss://server.com/ws", [](std::shared_ptr<http::websocket_client> ws) {
    if (ws) {
        ws->send_text("Hello!");
        auto [msg, _] = ws->receive();
        std::cout << "Received: " << msg << std::endl;
        ws->close();
    }
});

client.wait();

// With custom headers (using request builder)
client.request("wss://server.com/ws")
    .header("Authorization", "Bearer token")
    .protocol("graphql-ws")
    .websocket([](std::shared_ptr<http::websocket_client> ws) {
        if (ws) {
            ws->send_text("Hello!");
        }
    });

client.wait();
```

### WebSocket Client (Coroutine)

```cpp
thinger::http::async_client client;

client.run([&client]() -> awaitable<void> {
    // Simple connect
    auto ws = co_await client.websocket("wss://server.com/ws");

    if (ws) {
        co_await ws->send_text_async("Hello");
        auto [msg, binary] = co_await ws->receive_async();
        co_await ws->close_async();
    }
});

client.wait();

// With custom headers (using request builder)
client.run([&client]() -> awaitable<void> {
    auto ws = co_await client.request("wss://server.com/ws")
        .header("Authorization", "Bearer token")
        .protocol("graphql-ws")
        .websocket();

    if (ws) {
        co_await ws->send_text_async("Hello!");
    }
});

client.wait();
```

### WebSocket Client (Event-Driven)

```cpp
thinger::http::async_client client;

client.websocket("wss://server.com/ws", [](std::shared_ptr<http::websocket_client> ws) {
    if (!ws) return;

    ws->on_message([](const std::string& msg, bool binary) {
        std::cout << "Received: " << msg << std::endl;
    });

    ws->on_close([]() {
        std::cout << "Connection closed" << std::endl;
    });

    ws->on_error([](const std::string& error) {
        std::cerr << "Error: " << error << std::endl;
    });

    ws->send_text("Hello!");
    ws->run();  // Start message loop
});

client.wait();
```

## Server-Sent Events (SSE)

### SSE Server

```cpp
server.get("/events", [](auto& req, auto& res) {
    res.start_sse([](std::shared_ptr<http::sse_connection> sse) {
        // Send named event with data
        sse->send_event("message");
        sse->send_data("Hello!");

        // Send JSON data with event name
        sse->send_event("update");
        sse->send_data(R"({"count": 42})");

        // Set retry interval (milliseconds)
        sse->send_retry(5000);
    });
});
```

### SSE Client (JavaScript)

```javascript
const events = new EventSource('/events');

events.addEventListener('message', (e) => {
    console.log('Message:', e.data);
});

events.addEventListener('update', (e) => {
    const data = JSON.parse(e.data);
    console.log('Update:', data);
});
```

## Logging

### Enable Logging

```cpp
#include <thinger/util/logger.hpp>

// Enable default console logging
thinger::logging::enable();

// Set log level
thinger::logging::set_log_level(spdlog::level::debug);

// Use in code
LOG_INFO("Server started on port {}", port);
LOG_ERROR("Connection failed: {}", error);
```

### Custom Logger

```cpp
auto logger = spdlog::basic_logger_mt("http", "logs/http.log");
thinger::logging::set_logger(logger);
```

### Log Levels

| Level | Usage |
|-------|-------|
| `trace` | Detailed debugging |
| `debug` | Debug information |
| `info` | General information |
| `warn` | Warnings |
| `err` | Errors |
| `critical` | Critical errors |
| `off` | Disable logging |

## Build Options

| CMake Option | Default | Description |
|--------------|---------|-------------|
| `THINGER_HTTP_ENABLE_LOGGING` | `ON` | Enable spdlog integration |
| `THINGER_HTTP_BUILD_TESTS` | `ON` | Build test suite |
| `THINGER_HTTP_BUILD_EXAMPLES` | `OFF` | Build examples |

```bash
cmake -DTHINGER_HTTP_ENABLE_LOGGING=OFF ..
```

## Examples

See the [examples](examples/) directory for complete working examples:

- **HTTP Client**: [examples/http_client/](examples/http_client/)
- **HTTP Async Client**: [examples/http_async_client/](examples/http_async_client/)
- **HTTP Server**: [examples/http_server/](examples/http_server/)

## Thread Safety

- `http::server` - Thread-safe, can handle concurrent requests
- `http::client` - Single-threaded, create one per thread or use `async_client`
- `http::async_client` - Thread-safe, uses internal worker threads
- `http::websocket_client` - Single connection, thread-safe send operations
- `http::websocket_connection` - Server-side WebSocket, thread-safe send, callbacks on IO thread

## License

MIT License - see [LICENSE](LICENSE) for details.

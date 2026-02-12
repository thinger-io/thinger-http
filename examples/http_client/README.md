# HTTP Client Examples

Examples demonstrating HTTP and WebSocket client functionality.

## Examples

### simple_http_client

Basic HTTP GET request with JSON parsing.

```bash
./simple_http_client
```

```cpp
http::client client;
auto res = client.get("https://api.github.com/users/github");
if (res) {
    auto json = res.json();
    std::cout << json["login"] << std::endl;
}
```

### multiple_requests

Multiple sequential requests with connection reuse.

```bash
./multiple_requests
```

### timeout_example

Configure request timeouts.

```bash
./timeout_example
```

```cpp
http::client client;
client.timeout(std::chrono::seconds(5));
auto res = client.get("https://slow-server.com/data");
```

### websocket_client_example

WebSocket client with multiple API styles: sync, callback, and coroutine.

```bash
# Connect to public echo server
./websocket_client_example

# Or specify custom URL
./websocket_client_example ws://localhost:8080/ws
```

## API Overview

### HTTP Client

```cpp
http::client client;

// Configuration (chainable)
client.timeout(5s)
      .follow_redirects(true)
      .max_redirects(10);

// Methods
auto res = client.get(url);
auto res = client.post(url, body, content_type);
auto res = client.put(url, body, content_type);
auto res = client.del(url);

// Response
if (res) {
    res.status();        // HTTP status code
    res.body();          // Response body
    res.json();          // Parse as JSON
    res.content_type();  // Content-Type header
}
```

### WebSocket Client

#### Sync API (via http::client)

```cpp
http::client client;

// Connect - returns std::optional<websocket_client>
auto ws = client.websocket("wss://echo.websocket.org");

if (ws) {
    // Send message
    ws->send_text("Hello!");

    // Receive response
    auto [msg, is_binary] = ws->receive();
    std::cout << "Received: " << msg << std::endl;

    // Close gracefully
    ws->close();
}
```

#### Callback API (via http::async_client)

```cpp
http::async_client client;

client.websocket("wss://echo.websocket.org", [](std::shared_ptr<http::websocket_client> ws) {
    if (ws) {
        ws->send_text("Hello from callback!");
        auto [msg, _] = ws->receive();
        std::cout << "Received: " << msg << std::endl;
        ws->close();
    }
});

client.wait();  // Wait for completion
```

#### Coroutine API (via http::async_client)

```cpp
http::async_client client;

client.run([&client, &url]() -> awaitable<void> {
    // co_await the websocket upgrade
    auto ws = co_await client.upgrade_websocket(url);

    if (ws) {
        // Use async methods with co_await
        co_await ws->send_text_async("Hello from coroutine!");
        auto [msg, _] = co_await ws->receive_async();
        std::cout << "Received: " << msg << std::endl;
        co_await ws->close_async();
    }
});

client.wait();
```

#### Raw Socket Access

```cpp
auto ws = client.websocket("wss://example.com/ws");
if (ws) {
    // Release ownership of the underlying socket for advanced use
    auto raw_socket = ws->release_socket();
    // ws is now invalid, raw_socket owns the connection
}
```

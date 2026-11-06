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

HTTPS server with TLS certificates: a `pool_server` listening with `listener_options`, its `tls` holding a `certificate_manager` (a self-signed certificate for development).

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
    res.upgrade_websocket({}, [](auto ws) {
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

Cross-origin requests from a browser, with a list of origins and credentials. The server listens on two ports: open the test page on the second (`port + 1`), which is the origin allowed, and it calls the API on the first. It sends a cookie, which needs the credentials, and `Authorization` and JSON, which need the preflights the server answers.

```bash
./server_cors_example [port]
curl -i -H "Origin: http://localhost:8092" localhost:8091/api/data
curl -i -X OPTIONS -H "Origin: http://localhost:8092" -H "Access-Control-Request-Method: PUT" localhost:8091/api/data
```

```cpp
// The defaults, those of cors() in Express: any origin, without credentials
server.enable_cors();

// A list of origins with credentials: the origin of the request, if listed, is reflected.
// Returns invalid_argument for options that cannot be applied, e.g. credentials with "*"
if (auto ec = server.enable_cors({
        .origins = {"http://localhost:8092", "https://console.example.com"},
        .allowed_headers = std::vector<std::string>{"Content-Type", "Authorization"},
        .exposed_headers = {"X-Total-Count"},
        .max_age = std::chrono::minutes(10),
        .credentials = true,
    })) {
    return 1;
}
```

| `http::cors_options` | Default |
|---|---|
| `origins` | `{"*"}`: any origin, or a list of exact origins |
| `origin_filter` | none: a function that decides instead of the list |
| `methods` | `GET, HEAD, PUT, PATCH, POST, DELETE` |
| `allowed_headers` | no value: reflect what a preflight asks for |
| `exposed_headers` | empty |
| `max_age` | no value: `Access-Control-Max-Age` is not sent |
| `credentials` | `false` |
| `preflight_continue` | `false`: the library answers preflights itself |

A preflight (`OPTIONS` with `Access-Control-Request-Method`) is answered with `204` on any path, without running the middlewares; with `.preflight_continue = true` it goes through routing and the middlewares as any request. See [CORS](../../README.md#cors) in the main README.

### server_shutdown_example

Graceful server shutdown from a handler: `POST /api/shutdown` answers, then runs `co_await server.shutdown(by)` on the handler's thread, letting the requests in flight finish within 5 seconds.

```bash
./server_shutdown_example [port]
```

### virtual_hosts_example

Several sites on one listener, chosen by the `Host` header: an API host with its own OpenAPI document, a static site, dynamic device subdomains (`*.devices.localhost`) and the default host. Also shows a JSON error format and the client IP behind a trusted local proxy.

```bash
./virtual_hosts_example [static_dir]
curl -H "Host: api.localhost" localhost:8095/v1/status
curl -H "Host: sensor1.devices.localhost" localhost:8095/some/path
```

```cpp
server.host("api.example.com").get("/v1/status", handler);
server.host("*.devices.example.com").get("/:path(.*)", [](http::request& req, http::response& res) {
    auto device = req.get_host_matches()[1];
});
```

### reverse_proxy_example

A front server forwarding to a backend (started by the example, or the URL given): `/api/...` without its prefix and with the credentials of the proxy, and everything on `app.localhost` as it is, event streams, uploads and WebSockets included. Errors of the proxy (`502`, `504`) use a JSON format.

```bash
./reverse_proxy_example [backend_url]
curl -i localhost:8096/api/hello?name=proxy
curl -N -H "Host: app.localhost" localhost:8096/events
```

```cpp
http::reverse_proxy proxy;
auto target = *http::proxy_target::from_url("http://127.0.0.1:3000");
server.host("app.localhost").set_not_found_handler(
    [&](http::request& req, http::response& res) -> thinger::awaitable<void> {
        co_await proxy.forward(req, res, target);
    });
```

### device_proxy_example

Device web interfaces served by a console under `/devices/<id>/services/web/`, with an explicit response policy (`device_proxy_policy.hpp`): frame protections replaced by `frame-ancestors` allowing only the console, cookies without the device `Domain`, and redirects kept under the proxy path. The header documents the risks of each rule and of its less safe variants (removing frame protections, cross-site cookies).

```bash
./device_proxy_example
curl -i localhost:8098/devices/sensor1/services/web/
curl -i localhost:8098/devices/sensor1/services/web/login
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

// Connection settings (idle timeout, lingering close, logging), before listen()
http::connection_options options;
options.idle_timeout = std::chrono::seconds(60);
server.set_connection_options(options);

// Start
server.start("0.0.0.0", 8080);  // Blocking
server.listen("0.0.0.0", 8080); // Non-blocking: an error_code, empty on success
asio::listener_options tls;     // Another listener, HTTPS, serving the same routes
tls.endpoint = asio::tcp_endpoint{"0.0.0.0", 8443};
tls.tls = std::make_shared<asio::certificate_manager>();
server.listen(tls);
server.listening_on();           // {"http://0.0.0.0:8080", "https://0.0.0.0:8443"}
server.wait();                   // Wait for shutdown
server.stop();                   // Stop server (every listener)
```

### Request Object

```cpp
void handler(http::request& req, http::response& res) {
    req["param"];           // Path parameter
    req.query("key");       // Query parameter
    req.header("name");     // Request header
    req.body();             // Request body
    req.json();             // Parse body as JSON
    req.get_request_ip();   // Client IP (forwarded by a trusted proxy, if any)
    req.get_peer_ip();      // IP of the direct peer
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
res.upgrade_websocket({}, [](std::shared_ptr<http::websocket_connection> ws) {
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

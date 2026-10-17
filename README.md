![hola](/assets/thinger-http-banner.svg "thinger-http")

[![CI](https://img.shields.io/github/actions/workflow/status/thinger-io/thinger-http/ci.yml?branch=main&label=CI&logo=cplusplus&logoColor=white)](https://github.com/thinger-io/thinger-http/actions/workflows/ci.yml)
[![Examples](https://img.shields.io/github/actions/workflow/status/thinger-io/thinger-http/examples.yml?branch=main&label=Examples&logo=bookstack&logoColor=white)](https://github.com/thinger-io/thinger-http/actions/workflows/examples.yml)
[![Coverage](https://img.shields.io/endpoint?url=https://gist.githubusercontent.com/alvarolb/8811c934f647f86b48040792983d5abe/raw/coverage.json&logo=codecov&logoColor=white)](https://thinger-io.github.io/thinger-http/)
[![Sanitizers](https://img.shields.io/github/actions/workflow/status/thinger-io/thinger-http/sanitizers.yml?branch=main&label=Sanitizers&logo=adguard&logoColor=white)](https://github.com/thinger-io/thinger-http/actions/workflows/sanitizers.yml)
[![Fuzzing](https://img.shields.io/github/actions/workflow/status/thinger-io/thinger-http/fuzzing.yml?branch=main&label=Fuzzing&logo=hackthebox&logoColor=white)](https://github.com/thinger-io/thinger-http/actions/workflows/fuzzing.yml)

A modern C++20 HTTP/WebSocket library built on Boost.ASIO with coroutine support.

> [!WARNING]
> This library is under active development and is not yet recommended for production use. While its version is 0.x, a minor version may change the API: see [Versioning](#versioning) and the [changelog](CHANGELOG.md). Feedback and contributions are welcome.

## Performance

Requests per second with [wrk](https://github.com/wg/wrk) on Linux (2× Intel Xeon E5-2699 v4, Ubuntu 24.04, GCC 13, `-O3`). Every server has the same routes, 8 threads or workers pinned to the same 8 cores, and wrk runs on 16 other cores: 128 keep-alive connections (32 for a new connection per request), 10 s, median of 5 rounds in rotating order.

| | **thinger-http** | nginx 1.24 + njs 0.8 | [Crow](https://github.com/CrowCpp/Crow) 1.2.1 | [cpp-httplib](https://github.com/yhirose/cpp-httplib) 0.14.3 | Node.js 24, 8 processes | Node.js 24, 1 process |
|---|--:|--:|--:|--:|--:|--:|
| GET "Hello World!" | 533,603 | 436,056 | 398,276 | 155,638 | 188,349 | 22,874 |
| GET with path parameters, JSON answer | 396,260 | 284,629 | 3,105 ¹ | 136,233 | 174,683 | 22,406 |
| POST a JSON body, echoed as JSON | 316,636 | 275,039 | 3,102 ¹ | 123,080 | 128,428 | 16,474 |
| GET on a new connection per request | 125,826 | 125,841 | 105,992 | 70,103 | 15,968 | 8,180 |

thinger-http answers with a median latency of 235 µs, 318 µs, 398 µs and 108 µs in these four cases (p99 under 430 µs with keep-alive). JSON bodies are decoded within the default [limits](#body-types) (nesting depth, values, UTF-8).

Every route is a handler: nginx runs all of them in njs, rather than answering from its configuration.
¹ With its default settings Crow stalls about 40 ms on each of these responses (Nagle's algorithm and delayed ACKs); its `/hello` is not affected.
cpp-httplib serves one connection per thread, so it gets 128 threads on the same 8 cores, and runs with `TCP_NODELAY`.

To reproduce, see [`benchmark/compare`](benchmark/compare/): `benchmark/compare/run.sh` builds the servers and runs every case.

## Features

- **HTTP Server & Client** - Full HTTP/1.1 support with connection pooling
- **WebSocket** - Server and client support with text/binary messages, or as a byte stream socket for other protocols and tunnels
- **Server-Sent Events (SSE)** - Real-time server push
- **SSL/TLS** - HTTPS and WSS support via OpenSSL, with per-domain certificates (SNI)
- **Virtual Hosts** - Routes per host name, exact or by pattern, on the same listener
- **Reverse Proxy** - Forward requests to HTTP, HTTPS or Unix socket targets, streaming bodies and tunneling WebSockets
- **File Uploads** - Streaming multipart/form-data parser, with temporary files and limits
- **C++20 Coroutines** - Async-first API with sync wrappers
- **Graceful Shutdown** - Stop listening, finish the requests in flight and tell long-lived connections, within a grace period; connection statistics at no cost per request
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
- Boost 1.86+ (ASIO with coroutines, iostreams)
- ZLIB (HTTP compression via Boost.Iostreams)
- OpenSSL (for HTTPS/WSS)

### CMake Integration

```cmake
include(FetchContent)
FetchContent_Declare(
    thinger-http
    GIT_REPOSITORY https://github.com/thinger-io/thinger-http.git
    GIT_TAG v0.4.0
)
FetchContent_MakeAvailable(thinger-http)

target_link_libraries(your_target PRIVATE thinger::http)
```

### Versioning

Releases are tagged `vMAJOR.MINOR.PATCH` and follow [Semantic Versioning](https://semver.org/). Pin a tag rather than `main`, which changes often. While the version is 0.x:

- a patch version (0.1.x) only fixes bugs, without changing the API;
- a minor version (0.x.0) may add features and change the API or the behaviour. Every change that can break or alter existing code is listed in [CHANGELOG.md](CHANGELOG.md), under **Changed**, with how to adapt.

From 1.0.0 on, only a major version changes the API in an incompatible way.

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
server.head("/path", handler);
server.options("/path", handler);

// Handler signature
void handler(thinger::http::request& req, thinger::http::response& res);

// Requests matching no route (404/405 by default; nullptr restores them)
server.set_not_found_handler(handler);
```

Every registration method, `set_not_found_handler()` included, also accepts coroutine handlers (see [Coroutine Handlers](#coroutine-handlers)). A handler that throws is answered with `500`; if it had already started its response, the connection is closed instead.

`HEAD` requests are answered by the `GET` route of the path, as in Express, unless a `head()` route matches it (an explicit `HEAD` route wins). The response is the one to `GET` without its body: the same status and headers, `Content-Length` included, so the client reads the next response on the connection where it starts. Every answer drops the body of a response to `HEAD` (`send()`, `json()`, `error()`, `redirect()`, `send_file()`, chunked and streamed responses), also in `head()` routes. Responses to `HEAD` are not compressed, so their `Content-Length` is the length of the uncompressed body; `start_sse()` sends its headers alone (without calling its handler), and a WebSocket upgrade is rejected, as it requires `GET`.

Unmatched requests get `404` when their method has routes (`HEAD` has those of `GET`), or else `405 Method Not Allowed`, with an `Allow` header listing the methods of the routes matching the path (`HEAD` wherever `GET`), empty if none does.

### Path Parameters

```cpp
server.get("/users/:id", [](auto& req, auto& res) {
    std::string id = req["id"];
    res.json({{"user_id", id}});
});

server.get("/users/:id([0-9]+)", [](auto& req, auto& res) {
    // Only numeric ids: the regex constrains the segment
});

server.get("/files/:path(.+)", [](auto& req, auto& res) {
    std::string path = req["path"];  // Wildcard capture: "a/b/c.txt"
    res.send_file("/data/" + path);
});
```

Routes are matched segment by segment (the text between slashes) on a tree, so the cost of finding a route hardly depends on how many there are. Whatever the registration order, a segment is matched first by static text (`/users/me`), then by a constrained parameter (`:id([0-9]+)`, or text mixed with parameters as `:name.:ext`), then by a parameter (`:id`), and last by a parameter that may match slashes (`:path(.+)`), which takes the rest of the path; if the rest of the path does not match below the first option, the next one is tried. Different constraints at the same position are tried in the order they were first registered there, and wildcards in registration order. Registering a route with the same structure as another one (it would never match) logs a warning, and so does adding a constraint (or wildcard) next to another one that may overlap with it (unless they cannot start with the same character), as only that order decides between them. A parameter regex that depends on its surroundings (anchors, word boundaries, lookaheads, back-references) makes its route match the whole path, after every other option.

Paths are matched as received, before percent-decoding (`%2F` is not a slash, and parameters get the encoded text), and trailing slashes and empty segments are significant: `/users` does not match `/users/`, nor `/users/:id/devices` match `/users//devices`. Simple regexes (characters, classes, quantifiers, alternative words) are checked without `std::regex`, which is kept for the rest.

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

The server reads the body before calling the handler, decompressing it if it has `Content-Encoding: gzip` or `deflate` (as the client decodes responses, see [Response Parsing](#response-parsing): case-insensitive names, `x-gzip`, every gzip member, raw deflate, and lists of codings; invalid data, bytes after its end included, is answered with `400`). A body with a coding the server does not decode (such as `br`) is left to the handler as it is, with its `Content-Encoding`. Bodies larger than `set_max_body_size()` (8 MB by default, also once decompressed), or the limit of the route (see [Limits and Timeouts per Route](#limits-and-timeouts-per-route)), are answered with `413 Payload Too Large`; a `Content-Length` over the limit is rejected before reading anything.

Request framing is strict, as a lenient parser could read a request differently than a proxy in front of it (request smuggling). These requests are answered with `400` and the connection is closed:

- Header lines starting with a space or tab (obsolete line folding), which are never joined to the previous line.
- `Content-Length` that is not a plain number (`12abc`, `-1`, ` 5`, too large), or repeated with different values.
- `Transfer-Encoding` other than exactly `chunked` (e.g. `gzip, chunked`, `chunked, chunked`, or repeated), together with `Content-Length`, or in an HTTP/1.0 request.
- Malformed chunked bodies: chunk sizes that are not hexadecimal, empty or overflowing; missing CRLF after a chunk size or its data; bare LF line endings; chunk extensions or trailer sections that are too long. Chunk extensions and trailer fields are accepted and ignored.

The request line and headers are limited to 16 KB and 100 header lines: larger requests are answered with `431 Request Header Fields Too Large` and the connection is closed.

`Expect: 100-continue` (sent by curl on large uploads, which then waits up to a second before sending the body) is answered with `100 Continue` when the body is first read, by the server before the handler or by the handler (`read_body()`, `read()`, `read_some()`), as Go does; a client that sends the body without waiting gets none. A request answered without reading its body (`401` from a middleware, `404`, `415`, a `Content-Length` over the limit answered `413`...) gets no `100 Continue`, so the client never sends the body, and the connection is closed after the response (`Connection: close`), as Go and Node do, since the body may or may not follow. Any other expectation is answered with `417 Expectation Failed` (closing the connection if the request has a body). `Expect` is ignored in HTTP/1.0 requests.

A connection is kept alive by the server only while it reads requests, runs a handler or writes a response. When it closes after the response (`Connection: close`, HTTP/1.0, or a request body that could not be read), nothing keeps it once the handler returned and what was sent so far is written: a response sent from detached code after the handler returned (e.g. from a timer or another thread) is lost, and a chunked response that goes on writing that way loses the connection after its first frames (`write_chunk()` returns `false`). Finish such responses within the handler (a coroutine handler can `co_await` between chunks).

### Coroutine Handlers

Handlers can be coroutines that `co_await` other work (database queries, HTTP calls, timers). Declaring the JSON body in the signature makes the server read it, decode it (JSON by default, or the [body types](#body-types) the route accepts) and validate it against the route schema before the handler runs; a body that cannot be decoded or does not match the schema is answered with `400` and the handler is not called:

```cpp
server.post("/api/users", [&db](http::request& req, nlohmann::json& body, http::response& res) -> thinger::awaitable<void> {
    auto id = co_await db.insert_user(body["name"]);
    res.json({{"id", id}});
}).schema({
    {"type", "object"},
    {"required", {"name"}},
    {"properties", {{"name", {{"type", "string"}}}}}
});

// Without the request
server.put("/api/config", [](nlohmann::json& body, http::response& res) -> thinger::awaitable<void> {
    res.json(body);
    co_return;
});
```

A coroutine taking only `(request&, response&)` reads the body itself (see below). Add `.deferred_body(false)` to have the server read it into `req.body()` first.

### Streaming Request Body

Coroutine handlers read the body themselves, as it arrives (both `Content-Length` and chunked bodies):

```cpp
server.post("/upload", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
    uint8_t buf[8192];
    size_t total = 0;
    while (size_t bytes = co_await req.read_some(buf, sizeof(buf))) {
        total += bytes; // process the data
    }
    res.json({{"received", total}});
});
```

Reads never go past the end of the body, nor past the maximum body size of the route (`set_max_body_size()` unless the route sets its own): a larger body fails reading (`too_large`), a `Content-Length` one at the first read, without reading it. Whatever the handler leaves unread is discarded after it returns, so it is never parsed as the next request on a keep-alive connection; if it exceeds `set_max_body_size()` (or the route limit, if smaller), the connection is closed instead.

- Do not read the body straight from `req.get_socket()`: use `read()`/`read_some()`, or take over the connection (below).
- `co_await req.read_body()` loads the remaining body into `req.body()`, up to the maximum body size (it fails without reading a larger `Content-Length` body). Do not mix it with `read()`/`read_some()` on the same request: the stored body would miss the part already read (and fail to decompress if the request is compressed).
- `read()`/`read_some()` return 0 at the end of the body, and also if reading it fails: `req.get_body_error()` tells why (`too_large`, `malformed` framing, `incomplete` if the connection ended first, or `bad_encoding`). A request whose body fails is answered with `400` (`413` if too large) unless the handler responded, and its connection is closed.

### File Uploads (multipart/form-data)

Coroutine handlers read `multipart/form-data` bodies (HTML forms with files, `curl -F`, the client's [`http::form`](#request-builder-fluent-api)) as they arrive, never holding the whole body in memory. `req.read_multipart()` keeps the fields in memory and writes the files to temporary files, deleted when the form is destroyed (at the end of the handler) unless the handler moves or keeps them:

```cpp
server.post("/v1/storages/:storage/files", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
    http::multipart_options options;
    options.limits.max_file_size = 100 * 1024 * 1024;   // per file
    options.limits.max_files = 10;
    auto form = co_await req.read_multipart(options);
    if (!form) {
        res.error(form.status(), form.message());       // 400 malformed, 413 over a limit, 415 not a form
        co_return;
    }

    std::string description = form.field("description", "");   // a text field
    for (auto& file : form.files()) {
        // file.filename() is what the client sent ("../../etc/passwd" too): never a path
        auto name = file.safe_filename();               // "passwd"
        if (name.empty()) continue;
        if (!file.move_to(storage_path(req["storage"]) / name)) {   // renamed (copied across file systems)
            res.error(http::http_response::status::internal_server_error);
            co_return;
        }
    }
    res.json({{"uploaded", form.files().size()}, {"description", description}});
}).max_body_size(1024 * 1024 * 1024);                   // the route limit bounds the whole upload
```

To stream the parts somewhere else (a database, another server, a hash), `http::multipart_reader` reads them one by one, like Go's `multipart.Reader` or busboy, with the content of each part in pieces of up to 64 KB:

```cpp
server.post("/ingest", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
    http::multipart_reader reader(req);              // limits as the second argument
    while (const http::multipart_part* part = co_await reader.next()) {
        // part->name, part->filename, part->safe_filename(), part->content_type, part->header("X-Any")
        for (auto data = co_await reader.read(); !data.empty(); data = co_await reader.read()) {
            co_await sink.write(part->name, data);   // valid until the next read
        }
    }
    if (reader.failed()) {
        res.error(reader.status(), reader.message());
        co_return;
    }
    res.send("ok");
}).max_body_size(1024 * 1024 * 1024);
```

`multipart_options::on_file` does the same within `read_multipart()`: called for each file with the part and the reader, instead of writing temporary files. The parser itself (`http::multipart_parser`, fed bytes and returning events) does not depend on sockets.

- **Limits** (`http::multipart_limits`), checked as the body arrives: `max_parts` (1000), `max_files` (none), `max_header_size` of each part (16 KB), `max_filename_size` (255 bytes), `max_field_size` (1 MB, each part without filename), `max_fields_size` (8 MB, all of them, as they are kept in memory) and `max_file_size` (none). The whole body is bounded by the maximum body size of the route (`413` `Payload Too Large`), 8 MB by default: give upload routes the limit they need. Exceeding any limit fails with `413`. `max_parts` is Go's default, and the header and field sizes busboy's (Multer).
- **Syntax** (RFC 7578, RFC 2046 5.1), as strict as Go's `mime/multipart`: the boundary from the `Content-Type` (quoted or not, 1 to 70 characters of the RFC 2046 set, exactly one), CRLF line endings, spaces and tabs after delimiters, preamble and epilogue ignored, a closing delimiter required (a truncated body is `400`, as one with no parts but its closing delimiter is an empty form). The boundary within the content is a delimiter only at the start of a line and followed by `--`, whitespace or CRLF. Part headers are case-insensitive, without folding, CR, LF or NUL in values; a repeated `Content-Disposition` or `Content-Type`, or a repeated parameter (`name`, `filename`) is ambiguous and malformed. `filename*` (RFC 5987, UTF-8, ISO-8859-1 or US-ASCII) wins over `filename`, as in busboy; quoted strings keep backslashes that escape nothing, so Windows paths sent by old browsers stay paths (as Go reads them). A part without filename is a field; with one, even empty (a file input without a file), a file. Parts without `Content-Disposition: form-data` or without a name are skipped by `read_multipart()` and the decoder, as Go and busboy do; the reader gives them too, and any `multipart/*` type.
- **Filenames**: `filename()` is what the client sent, which can be a path or have control characters. `safe_filename()` is what follows its last `/` or `\`, without control characters, and empty for `.` and `..`: a name to store the file with, though it can still collide with an existing file or be a dotfile.
- **Temporary files** are created with `mkstemp()` (mode 0600) in `multipart_options::temp_directory` (the system one by default), written as the parts arrive (blocking writes, as Go does), and deleted on any error, and with the form. A file that cannot be written fails with `500`.
- **Bodies the server already read** (`.deferred_body(false)`, in-memory dispatch) are parsed from `req.body()`, decompressed by the server if they had `Content-Encoding`; a body still to be read with a `Content-Encoding` other than `identity` is `415`, as it is not decoded while streaming.

Handlers taking the body decoded (`nlohmann::json&`) can also accept `multipart/form-data` with `.accepts({"form", "multipart"})`: the `multipart` [body decoder](#body-types) reads it whole (within the maximum body size and the default limits) into the same object a urlencoded form gives, files included as objects `{"filename": <safe filename>, "content_type": ..., "content": <binary>}`, so one handler and one schema serve both form encodings. It is not accepted unless listed, as Express's `body-parser` does not parse multipart either: routes receiving files should stream them instead.

### Taking Over the Connection

`res.take_over()` hands the connection to your code, for custom protocol upgrades or tunnels (WebSockets and SSE are built on it). The response set so far is sent first (200 by default; unless it is a `101`, a `2xx` to `CONNECT`, or carries a `Content-Length` or `Transfer-Encoding` set with `header()`, it says `Connection: close`, as what the handler writes is its body until the connection closes); then the server stops reading from the connection and calls the handler with the socket and any bytes the client had already sent past the request. From then on the handler owns the socket. It can also be called later from a copy of the response: until the response starts, the server handles nothing else the client sends on that connection (it stays readable from the socket, for the handler), but it notices a client that closes or half-closes the connection meanwhile, over TLS too, and closes it. A `take_over()` arriving after that does not call its handler: it is released, and a warning logged.

```cpp
server.get("/tunnel", [](http::request& req, http::response& res) {
    res.status(http::http_response::status::switching_protocols);
    res.header("Upgrade", "my-protocol");
    res.header("Connection", "Upgrade");
    res.take_over([](std::shared_ptr<thinger::asio::socket> socket, std::string buffered) {
        // `buffered` holds bytes already received after the request; process them first
    });
});
```

The handlers of `take_over()`, `upgrade_websocket()`, `upgrade_websocket_stream()` and `start_sse()` can be coroutines: they run on the io_context of the connection, detached (what they throw is logged); `upgrade_websocket()` starts reading messages once its coroutine completes. So can the middlewares with `next()` and the callbacks of the async client. Callbacks that are called and not awaited (`on_message()`, `on_shutdown()`, `begin_shutdown()`'s `on_done`, the error formatter, the reverse proxy `on_request`/`on_response` hooks, the OpenAPI hooks...) take only synchronous functions: a coroutine lambda there does not compile, instead of being dropped without running; `co_spawn()` the work from them instead.

```cpp
server.get("/feed", [](http::request& req, http::response& res) {
    res.start_sse([](std::shared_ptr<http::sse_connection> sse) -> thinger::awaitable<void> {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
        for (int i = 0; i < 10; ++i) {
            sse->send_data(std::to_string(i));
            timer.expires_after(std::chrono::seconds(1));
            co_await timer.async_wait(thinger::use_awaitable);
        }
        sse->stop();
    });
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

// File (with ETag and Last-Modified: see Static Files)
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

Every response tells where its body ends with a `Content-Length` (also `send_response()` with a hand-built `http_response`: its content is its body, whatever `Content-Length` or `Transfer-Encoding` it carried), so the connection can carry the next request; `1xx`, `204` and `304` have no body, and `1xx` and `204` no `Content-Length`.

Bodies of 200 bytes or more of a text-based type (`text/*`, JSON, XML, JavaScript, SVG) are compressed with gzip or deflate as the request's `Accept-Encoding` asks (RFC 9110: case-insensitive codings, `q` values, `q=0` as not acceptable, `*` and `identity`; gzip at equal quality), unless the response has `Content-Encoding` or `Cache-Control: no-transform`. They carry `Vary: Accept-Encoding` whether they were compressed or not, as with Express's `compression` middleware (which also adds it to smaller ones). Responses to `HEAD` are never compressed.

### Static Files

```cpp
// Serve directory with fallback to index.html (default)
server.serve_static("/static", "/var/www/static");

// Custom fallback file
server.serve_static("/app", "/var/www/app", "app.html");

// Without fallback (returns 404 for non-existent files)
server.serve_static("/assets", "/var/www/assets", "");

// Options: fallback file and Cache-Control (here, assets with a hash in their names)
server.serve_static("/assets", "/var/www/app/assets", {
    .fallback = "",
    .cache_control = "public, max-age=31536000, immutable"
});
```

`serve_static()` answers `GET` requests, and `HEAD` requests through its `GET` route (so the `GET` routes of the paths it covers, such as an API next to `serve_static("/", ...)`, answer their `HEAD` requests too). The fallback file is served for a directory (its own one), and for the paths without a file (the one of the root directory, as single page applications expect). Paths resolving outside the directory (through symlinks; request targets with `..` are already rejected with `400`) are answered with `403`.

Files are served with validators, so that browsers download them once and only check them afterwards:

- A weak `ETag` from the size and modification time of the file (`W/"<size>-<mtime in ns>"`, in hex; the content is not hashed), and `Last-Modified`.
- `Cache-Control: no-cache` by default: caches keep the file, but revalidate it on every use. Set another policy with `cache_control` (e.g. `max-age` for hashed assets).
- Conditional requests are answered with `304 Not Modified`, without body, while the file does not change: `If-None-Match` with `*` or a list of entity tags (weak comparison, `W/` ignored), or else `If-Modified-Since` (ignored when there is an `If-None-Match`, or when it is not a valid date) with a date not earlier than the modification time. The `304` carries the validators and the `Cache-Control`.
- `Accept-Ranges: bytes`: a `GET` with a single byte range (`Range: bytes=0-99`, `bytes=100-`, `bytes=-100`) gets `206 Partial Content`, or `416` if it starts past the end. Several ranges, or an invalid one, get the whole file; so does a range with an `If-Range` other than the `Last-Modified` date (the weak `ETag` never matches it).
- Text files of 200 bytes or more are compressed for clients accepting gzip or deflate (see [Response Types](#response-types), with `Vary: Accept-Encoding`); ranges are sent as they are. A `HEAD` gets the headers of the `GET` with the length of the file, without reading it.

#### Precompressed Files

A file with a `<file>.br` or `<file>.gz` next to it (as builds emit them: Angular, Vite and webpack plugins, or `brotli`/`gzip -k` after the build) is sent as that sibling to the clients accepting `br` or `gzip`, as nginx `gzip_static`/`brotli_static` and `express-static-gzip` do. This gives Brotli without a Brotli library, and no compression work per request:

- The coding is chosen by the `q` values of `Accept-Encoding` (as for compression), `br` first at equal quality; none if the client gives `identity` a higher quality. The next accepted sibling is tried when one is missing, and the file itself when none is there.
- A sibling is only sent if it is a regular file not older than the file (an older one was compressed from a previous version), and, if it is a symlink, while it resolves inside the served directory.
- It is sent with `Content-Encoding: br` (or `gzip`), the `Content-Type` of the file, `Vary: Accept-Encoding`, and validators of its own: its `Last-Modified`, and its `ETag` with the coding in it (`W/"<size>-<mtime>-br"`, as Apache does), so conditional requests get a `304` per representation, and the tag of one never matches another.
- Byte ranges are ranges of the sibling as sent (as nginx does), with `If-Range` on its date; a `HEAD` gets its length. A request for `app.js.br` itself gets that file as is, without `Content-Encoding`.
- Looking for siblings costs one `stat()` per accepted coding (none if the client accepts neither `br` nor `gzip`); the file itself is still checked with a single one.

It is on by default: unlike nginx (where `gzip_static` is off), a sibling only exists if the deployment put it there, stale ones are skipped, and serving the files compressed at build time is what having them is for. Turn it off with `.precompressed = false`:

```cpp
server.serve_static("/", "/var/www/app", {.precompressed = false});
```

`res.send_file()` does the same in a handler. Its `Cache-Control` is `no-cache` unless the handler set one before (`res.header("Cache-Control", ...)`):

```cpp
server.get("/report", [](http::response& res) {
    res.header("Cache-Control", "private, max-age=60");
    res.send_file("/data/report.pdf");   // 304 while it does not change
});
```

### JSON Schema Validation

Validate request bodies against [JSON Schema](https://json-schema.org/) using [Valijson](https://github.com/tristanpenman/valijson). Chain `.schema()` on any route to enforce validation before the handler is called. Invalid requests get a `400 Bad Request` with error details.

```cpp
server.post("/api/users", [](nlohmann::json& json, auto& res) {
    // json is already validated against the schema
    res.json({{"created", true}, {"name", json["name"]}});
}).schema({
    {"type", "object"},
    {"required", {"name", "email"}},
    {"properties", {
        {"name", {{"type", "string"}}},
        {"email", {{"type", "string"}}},
        {"age", {{"type", "integer"}, {"minimum", 0}}}
    }}
});
```

Works with all JSON handler signatures and HTTP methods:

```cpp
// With request access
server.put("/api/users/:id", [](auto& req, nlohmann::json& json, auto& res) {
    res.json({{"updated", true}, {"id", req["id"]}});
}).schema({
    {"type", "object"},
    {"properties", {
        {"name", {{"type", "string"}}}
    }}
});

// Enum validation
server.post("/api/status", [](nlohmann::json& json, auto& res) {
    res.json({{"status", json["status"]}});
}).schema({
    {"type", "object"},
    {"required", {"status"}},
    {"properties", {
        {"status", {{"type", "string"}, {"enum", {"active", "inactive", "pending"}}}}
    }}
});
```

Invalid bodies get `400` with the body `{"error": {"message": "...", "context": [...]}}`, where `context` is the location of the invalid value; the format can be changed with an [error formatter](#error-format).

A schema always validates the body, whatever the signature of the handler: one taking only `(request&, response&)` or `(response&)`, coroutine or not, gets the body read, decoded and validated before it runs (in `req.body()`, as with `.deferred_body(false)`), with the same answers (`400`, `415` for a type that cannot be decoded).

A schema that cannot be parsed (e.g. a `$ref` to a schema component not registered before the route) is logged as an error when it is set, and the route then answers every request with `500` instead of accepting bodies without validation.

Valijson is enabled by default. Disable with `-DTHINGER_HTTP_ENABLE_VALIJSON=OFF`.

### Body Types

Handlers taking the body as `nlohmann::json&` get it decoded by its `Content-Type`, before the schema validation, so one handler and one schema serve every format. Only JSON is accepted unless the route or the application accept more types, so the formats a server parses never grow by accident:

```cpp
// Device data as JSON, MessagePack, CBOR, UBJSON or a urlencoded form
server.post("/devices/:device/data", [](http::request& req, nlohmann::json& data, http::response& res) {
    res.encode({{"device", req["device"]}, {"received", data}});   // replies in the type the client Accepts
}).schema(data_schema).accepts({"json", "msgpack", "cbor", "ubjson", "form"});

// Raw text, as a JSON string; binary, as a json::binary value
server.post("/devices/:device/log", [](nlohmann::json& line, http::response& res) { /* ... */ }).accepts({"text"});

// Application default for the routes without accepts() ({"json"} unless set)
server.accept_body_types({"json", "msgpack", "cbor"});

// Built-in decoders can be removed, and others added
server.body_decoders().remove("ubjson");
server.body_decoders().add("csv", {"text/csv"}, [](std::string_view body, const http::media_type& type,
                                                   const http::body_limits& limits) -> nlohmann::json {
    return parse_csv(body);   // throw http::body_decode_error("...") to answer 400
});

// Limits of decoded bodies
server.set_body_limits({.max_depth = 32});
```

`accepts()` takes decoder names or media types. The built-in decoders:

| Name | Media types | Decoded as |
|---|---|---|
| `json` | `application/json` and any `+json` (`application/problem+json`) | The JSON value |
| `msgpack` | `application/msgpack`, `application/x-msgpack` | The value; `bin` and `ext` as binary values |
| `cbor` | `application/cbor` and any `+cbor` | The value; byte strings as binary values. Tagged items are rejected |
| `ubjson` | `application/ubjson` | The value |
| `form` | `application/x-www-form-urlencoded` | An object of strings (see below) |
| `multipart` | `multipart/form-data` | The same object, with files as objects (see [File Uploads](#file-uploads-multipartform-data)) |
| `text` | `text/plain` | A string |
| `binary` | `application/octet-stream` | A binary value (`json::binary`) |

- **Content-Type**: parsed as RFC 9110 defines it: the media type is case-insensitive, and parameters are ignored except `charset`, which must be UTF-8 (`us-ascii` too for text) for JSON, forms and text. A type the route does not accept, a malformed `Content-Type`, an unsupported charset or several different `Content-Type` headers are answered with `415 Unsupported Media Type`, before the body is read.
- **Without Content-Type**, the body is taken as JSON, as before body types existed. A route that does not accept JSON answers `415` then.
- **Behavior change**: JSON routes used to parse any body as JSON, whatever its `Content-Type`; now a body sent with another type is answered with `415` (e.g. `curl -d '{...}'` without `-H "Content-Type: application/json"`, which sends a form). This also stops cross-site forms and `text/plain` posts from browsers, which need no CORS preflight, from reaching JSON routes.
- **Empty bodies** are `null`, whatever their type, and the schema decides whether they are valid.
- **Errors**: a body that cannot be decoded is answered with `400` (`Invalid JSON`, `Invalid MessagePack`, `Invalid CBOR: nested deeper than 128 levels`, `Invalid form data: malformed percent-encoding`...), through the [error formatter](#error-format), like `415`. A custom decoder throws `http::body_decode_error` with its message (and `415` if the type is unsupported); any other exception is answered with `400` `Invalid <name> body`.
- **Routes reading the body themselves** (`(request&, response&)` handlers) accept any type, unless they list theirs with `accepts()`: then other types get `415`, and listed types need no decoder (`.accepts({"image/png", "image/jpeg"})`).

Forms are decoded as browsers encode them: `name=value` pairs separated by `&`, percent-decoded, with `+` as a space, in names too. Each name is a member of the object with its value as a string; a repeated name, an array of its values in order (`tags=a&tags=b` is `{"tags": ["a", "b"]}`, but `tags=a` alone is `{"tags": "a"}`, so a schema for repeated fields should accept both, or the clients always send at least two). A pair without `=` has an empty value, and empty pairs and pairs without name are skipped. Numbers are not converted: `age=30` is `"30"`. Malformed percent-encoding or names and values that are not UTF-8 are answered with `400`.

Thinger.io's server decodes the same formats (`api_request::get_request_data`), with these differences: it matches the `Content-Type` by prefix and takes any other type (or none) as binary, where this library answers `415`; of a repeated form name it keeps one value; malformed percent-encoding gives empty strings; `text/html` is text too (`body_decoders().add("text", {"text/plain", "text/html"}, http::body_decoder_registry::decode_text)` does the same here); and it has none of the limits below.

Limits, against bodies that are small but expensive to decode or use, besides `set_max_body_size()` (checked first, also for decompressed bodies):

- **Nesting**: arrays and objects nested deeper than `max_depth` (128 by default) are rejected in every format. Decoding never recurses, and the decoded value is often used recursively (schema validation, `dump()`, copies, comparisons), which could overflow the stack otherwise. This applies to JSON bodies too, which had no limit before.
- **Values**: a body never decodes to more values than it has bytes, so memory is bounded by the body size. Declared lengths are never allocated up front (a MessagePack array of 4 billion elements in 5 bytes fails at the end of the input), and UBJSON arrays of values without payload (`[$Z#l` and a count: billions of nulls in 9 bytes) are rejected.
- **UTF-8**: strings (and object keys) must be valid UTF-8 in every format, so a decoded value can always be dumped as JSON.

Repeated object keys keep the last value. `res.encode(value)` replies with JSON, MessagePack, CBOR or UBJSON, whichever the `Accept` header prefers (JSON if none, or none of them, with `Vary: Accept`). The [OpenAPI](#openapi) document lists the media types each route accepts in its `requestBody`, with the same schema, and [in-memory dispatch](#in-memory-dispatch) decodes bodies the same way (`request->set_content(body, "application/msgpack")`). See `examples/http_server/server_body_decoders_example.cpp`.

### Middleware

Middlewares run for every request, in registration order, before the route handler. Register them before `listen()`/`start()`. A middleware is a coroutine that returns `true` to continue the chain, or responds and returns `false` to stop it (the route handler is not called):

```cpp
server.use([&tokens](http::request& req, http::response& res) -> thinger::awaitable<bool> {
    // The matched route is already known (nullptr if no route matched)
    auto* route = req.get_matched_route();

    // Asynchronous work, e.g. validating a token against a database
    auto user = co_await tokens.validate(req.header("Authorization"));
    if (!user) {
        res.error(http::http_response::status::unauthorized, "Invalid token");
        co_return false;
    }

    req.set_auth_user(*user);
    co_return true;
});
```

Middlewares with a `next()` callback are supported too, synchronous (`next()` must be called before returning) or coroutines (`next()` must be called before the coroutine completes):

```cpp
server.use([](http::request& req, http::response& res, std::function<void()> next) {
    res.header("X-Powered-By", "thinger-http");
    next();
});

server.use([&tokens](http::request& req, http::response& res, std::function<void()> next) -> thinger::awaitable<void> {
    if (co_await tokens.validate(req.header("Authorization"))) next();
    else res.error(http::http_response::status::unauthorized);
});
```

Notes:

- Middlewares run after route matching and **before the request body is read**, so `req.body()` is empty inside them. The body is read afterwards (or left to the handler on deferred-body routes).
- Middlewares and the route handler share the same `response`, so headers set by a middleware are kept in the final response.
- If a middleware stops the chain, any unread request body is discarded so the connection can be reused. If the body is larger than `set_max_body_size()`, the connection is closed after the response.
- A middleware that returns `false` without responding, or throws, gets a `500 Internal Server Error`.
- `request` and `response` are only valid until the middleware returns: do not keep references to them in detached work.

`set_basic_auth()` is a ready-made middleware for HTTP Basic authentication on a path prefix (see `examples/http_server/server_auth_example.cpp`).

### Request Context

A middleware passes values of its own types to the route handler through the request context, `req.context()`: e.g. the principal (user, token, permissions) an authentication middleware resolves. Each value is stored under a key, a `http::context_key<T>` that fixes its type, so the handler reads it with that type, without casts:

```cpp
struct principal {
    std::string user;
    std::vector<std::string> scopes;
};

// One key per value, with static storage duration (a namespace-scope constant)
inline constexpr http::context_key<principal> principal_key{"principal"};

server.use([&tokens](http::request& req, http::response& res) -> thinger::awaitable<bool> {
    auto user = co_await tokens.validate(req.header("Authorization"));
    if (!user) {
        res.error(http::http_response::status::unauthorized, "Invalid token");
        co_return false;
    }
    req.context().set(principal_key, principal{user->name, user->scopes});
    co_return true;
});

server.get("/me", [](http::request& req, http::response& res) {
    const principal* who = req.context().get(principal_key);   // null if none was stored
    if (!who) {
        res.error(http::http_response::status::unauthorized);
        return;
    }
    res.json({{"user", who->user}, {"scopes", who->scopes}});
});
```

- `set(key, value)` stores a value (copied or moved in; move-only types such as `std::unique_ptr` are fine), `emplace(key, args...)` constructs it in place, and both replace (and destroy) the value the key had. `get(key)` returns a pointer to the value, or `nullptr` if the key has none. `contains(key)`, `erase(key)` (destroys it), `take(key)` (moves it out, as a `std::optional`) and `clear()` complete it.
- Keys are told apart by their address, not by their type or name: two `context_key<std::string>` (a trace id and a tenant) are two values, and the keys of two libraries never collide. Define each key once, with static storage duration, and keep it private to the code that stores and reads its value (as Go recommends for context keys), or expose a typed accessor. A value is only read with the type of its key: there are no casts to get wrong.
- Every kind of middleware (coroutine returning `bool`, or with `next()`) and every route callback taking the `request` sees the same context, also for requests matching no route (`get_matched_route()` is null) and in [in-memory dispatch](#in-memory-dispatch), where `dispatch_options::context` seeds it.
- Lifetime: the values belong to the request and are destroyed with it, also when a middleware stops the chain, something throws, or an in-memory dispatch times out. Like the `request`, they are only valid until the middleware or handler returns: a handler answering later from a copy of the response must copy what it needs from them first.
- The context is not synchronized: a request is processed by one coroutine at a time, so only that coroutine (the middlewares, then the handler) uses it.
- A request that stores nothing pays nothing: the context allocates on its first value.

It is the equivalent of Express `res.locals` (or `req.user`, set by Passport), Go `context.WithValue(ctx, key, value)` with a key of an unexported type, and Rust `http::Extensions` (axum, hyper), here with a value per key instead of per type. `set_auth_user()` and `set_auth_groups()` remain for a user name and groups as strings.

### Route Metadata and Documentation

Routes can carry documentation fields (for API documentation tools) and free metadata for your application. The library does not interpret the metadata: middlewares read it from the matched route, e.g. to check the permission a route requires.

```cpp
server.get("/devices/:device([a-z0-9_-]{1,32})", handler)
    .summary("Get a device")
    .description("Returns the device configuration")
    .tag("Devices")
    .operation_id("getDevice")
    .path_param("device", "Device identifier")
    .query_param("fields", "Fields to return")
    .returns(200, "The device", device_schema)
    .returns(404, "Device not found")
    .meta("permission", "Device:Read");

server.use([](http::request& req, http::response& res) -> thinger::awaitable<bool> {
    auto* route = req.get_matched_route();
    if (route && route->has_meta("permission") &&
        !allowed(req, route->get_meta("permission").get<std::string>())) {
        res.error(http::http_response::status::forbidden);
        co_return false;
    }
    co_return true;
});
```

Groups share a path prefix and pass their tags and metadata on to their routes (a route can override a metadata key):

```cpp
auto devices = server.group("/v1/users/:user/devices").tag("Devices").meta("resource", "device");
devices.get("/:device", get_device).meta("permission", "Device:Read");
devices.group("/:device/resources").tag("Resources").get("/:resource", get_resource);
```

All routes, with their documentation and metadata, are available from `server.router().get_routes()`. Registered routes are never moved, so the `route&` returned by a registration method stays valid while more routes are added. See `examples/http_server/route_metadata_example.cpp`.

### Limits and Timeouts per Route

Routes, groups and [virtual hosts](#virtual-hosts) can set their own maximum body size, request timeout and [decode limits](#body-types), instead of the application ones:

```cpp
server.set_max_body_size(1024 * 1024);   // 1 MB for every route that does not set its own

// Imports: large bodies, and time to process them
auto imports = server.group("/v1/imports").max_body_size(512 * 1024 * 1024).timeout(10min);
imports.post("/devices", import_devices);
imports.post("/preview", preview_import).max_body_size(1024 * 1024).timeout(10s);  // overrides the group

// A call answered by a device: fail fast if it does not answer
server.post("/v1/devices/:device/call", call_device)
    .max_body_size(16 * 1024)
    .timeout(5s)                                                        // 504 Gateway Timeout
    .decode_limits({.max_depth = 16});

// Another status for the timeout
server.get("/v1/reports/:id", get_report).timeout(30s, http::http_response::status::service_unavailable);

// A whole host forwarded by a reverse proxy: limits of its not-found handler, and the
// default of its routes
server.host("devices.example.com").max_body_size(64 * 1024 * 1024).timeout(60s).set_not_found_handler(forward);
```

The limit of a request is the one of its route, else of the group it was registered in, else of its virtual host, else of the application (route > group > host > application). Group limits are copied to each route as it is registered, so set them on the group first; a route overrides them with its own (`.timeout(0ms)` removes the timeout of its group or host). Host limits apply when a request is served, to the routes of the host that do not set theirs and to its not-found handler (a proxy for the whole host, say), which has no route. On the server itself they are those of the default host only, while `set_max_body_size()` and `set_body_limits()` apply to every host.

**Maximum body size** (`.max_body_size(bytes)`, `set_max_body_size()` if unset everywhere, `std::numeric_limits<size_t>::max()` for none) bounds every way the body is read, with `413 Payload Too Large` answered through the [error formatter](#error-format) and the connection closed:

- Bodies the server reads (handlers taking the body decoded, synchronous handlers, `.deferred_body(false)`): a `Content-Length` over the limit is rejected before reading anything; a chunked body, as soon as it goes past it; a compressed body, also once decompressed.
- Coroutines reading the body themselves (`read_body()`, `read()`, `read_some()`): reading a larger body fails (`req.get_body_error()` is `too_large`, a `Content-Length` one at the first read, without reading it), and the server answers `413` unless the handler did. **Behavior change**: these reads had no limit before; now they are bounded by `set_max_body_size()` (8 MB by default) unless the route (or its group or host) sets its own: give routes receiving files or other large uploads, and hosts forwarded whole, the limit they need.
- The [reverse proxy](#reverse-proxy): the request body it streams is bounded by the route limit and by `proxy_options::max_body_size`, whichever is smaller. The not-found handler of a host (a proxy for a whole host) has no route: the limit of the host applies, or else the application one.
- [In-memory dispatch](#in-memory-dispatch): the same, from the body of the request.
- A body left unread by the handler (e.g. a middleware answered `401` before it was read) is discarded only up to the smaller of `set_max_body_size()` and the route limit; a larger one closes the connection instead of being read, so a route accepting large bodies does not make the server read them for nothing.

**Timeout** (`.timeout(duration, status = 504)`, none by default) is the maximum time from the dispatch of a request (its headers received and its route matched) until its response **starts**, middlewares and body reading included. If no answer has started by then, the request is answered with `504 Gateway Timeout` (or the status given, e.g. `503 Service Unavailable`) through the [error formatter](#error-format), with the message `No response within the timeout` and the headers set on the response so far (e.g. CORS headers of a middleware). As for any answer, set the status and headers (`res.status()`, `res.header()`) from the thread running the handler, before answering: a handler with a timeout that sets headers later, from another thread, would race with the timeout copying them.

- **Handlers are not cancelled**: a coroutine goes on until it ends, and anything it sends afterwards (or a copy of the response, from another thread) is dropped: `has_responded()` is `true` for every copy, and late answers are logged at debug level only. Keep handlers bounded on their own (their own timeouts on what they wait for).
- **Started responses are never cut**: once the response starts (`send()`, `json()`, `start_stream()`, `start_chunked()`, `start_sse()`, WebSocket upgrades, `take_over()`), the timer is cancelled and the response takes as long as it needs.
- **The connection**: if the body was read whole, the connection stays alive and serves the next request right away, while the handler may still run. If part of the body is still unread (the handler may be reading it), the response closes the connection, as nothing else can tell where the body ends; the handler then sees the body incomplete.
- **Connection timeout**: while a request waits for its response within its own timeout, the connection timeout (`set_connection_timeout()`, 120 s) does not close the connection, so a route can wait longer than it; it applies again to the idle connection afterwards, and uploads extend it as before.
- **Not-found handlers** get the timeout of their virtual host, if it sets one.
- **In-memory dispatch** applies the route timeout too; `dispatch_options::timeout` still bounds the whole call (and cancels the handler), whichever expires first.

The [OpenAPI](#openapi) document shows the limits set on a route (or its group) as `x-max-body-size` (bytes) and `x-timeout-ms` extensions of its operation.

### OpenAPI

The server builds an OpenAPI 3.1 document from its routes: paths and path parameters from the route patterns (including the regex of `:param(regex)`), request bodies from the validation schemas (one entry per [body type](#body-types) accepted), and summaries, tags, parameters, responses and examples from the route documentation. Hidden routes (`.hidden()`), static files and the document itself are left out.

```cpp
// Shared schemas: referenced from routes, published as components and validated at runtime
server.schema_component("Device", {
    {"type", "object"},
    {"required", {"name"}},
    {"properties", {{"name", {{"type", "string"}}}}}
});

server.post("/devices", create_device)
    .schema({{"$ref", "#/components/schemas/Device"}})
    .summary("Create a device")
    .returns(201, "Device created", {{"$ref", "#/components/schemas/Device"}})
    .meta("permission", "Device:Create");

server.openapi()
    .title("Devices API")
    .version("1.0.0")
    // The library does not interpret route metadata: hooks turn it into what you need
    .on_operation([](const http::route& route, const std::string& method, nlohmann::json& operation) {
        if (route.has_meta("permission")) operation["x-permission"] = route.get_meta("permission");
    })
    .on_document([](nlohmann::json& document) {
        document["components"]["securitySchemes"]["bearerAuth"] = {{"type", "http"}, {"scheme", "bearer"}};
    });

server.serve_openapi("/openapi.json");       // optional: not served unless called
auto document = server.openapi().generate(); // or build it directly
```

Register shared schemas before the routes that reference them. See `examples/http_server/openapi_example.cpp`, which also serves Swagger UI.

### In-Memory Dispatch

`server.dispatch()` runs a request without any connection, through the same steps as a network request: route matching, middlewares, body reading and validation, the handler, and 404/405. It is useful for tests, internal calls between modules and gateways (e.g. an MCP server calling the API with the caller's credentials):

```cpp
auto request = http::http_request::create_http_request(http::method::POST, "http://localhost/devices");
request->set_content(R"({"name":"sensor"})", "application/json");
request->add_header("Authorization", "Bearer " + token);   // checked by your middlewares

auto response = co_await server.dispatch(request, {.remote_ip = "10.0.0.7", .timeout = 5s});
// response->get_status_code(), response->get_content()

// Outside a coroutine
server.dispatch(executor, request, [](std::shared_ptr<http::http_response> response) { /* ... */ });

// With initial values of the request context, read by the middlewares and the handler
http::dispatch_options options{.timeout = 5s};
options.context.set(trace_key, trace_id);
response = co_await server.dispatch(request, options);
```

The server does not need to be listening. Chunked responses are collected whole. A `HEAD` request gets the response it gets on a connection: no content, with the `Content-Length` of the body of `GET`. Handlers can keep a copy of the response and answer later. The timeout covers the whole request, handler included: if it expires the result is `504` and the handler is cancelled (its pending awaits are aborted). WebSockets, SSE and `take_over()` answer `501`, as there is no connection. The request is served by the [virtual host](#virtual-hosts) of its URL (or `Host` header). `remote_ip` is the peer address: `get_request_ip()` returns it (or the forwarded client address, if it is a [trusted proxy](#client-ip-behind-proxies)), and it is empty unless set: internal calls never pass as a trusted local address by default. `context` holds the initial values of the [request context](#request-context) (empty by default): middlewares may replace them, and nothing is carried over from a request the caller is serving, so a gateway decides what to pass (or authenticates the call again). Copying `dispatch_options` copies its values; a value of a type that cannot be copied (e.g. `std::unique_ptr`) makes that copy throw `std::logic_error`, so move such options into `dispatch()`.

Everything about routes, hosts, middlewares and request settings (error format, trusted proxies, body limit and decoders, CORS), `dispatch()` included, comes from `http::http_application`, which the server extends with its listeners, TLS and connection settings. An `http::http_application` can also be used on its own, without a server, only to dispatch requests in memory.

### Virtual Hosts

One server can serve several sites on the same listener, chosen by the `Host` header (case-insensitive, port ignored). Each virtual host has its own routes, with the same registration API as the server: every `get`/`post`/... overload (coroutine and JSON body handlers included), route documentation and metadata, `group()`, `schema_component()`, `serve_static()` and `set_not_found_handler()`. The routes registered on the server itself are the default host, which answers any host without a virtual host of its own, so existing code keeps working.

```cpp
// Exact host names
auto& api = server.host("api.example.com");
api.get("/v1/status", [](http::response& res) { res.json({{"status", "ok"}}); });
api.group("/v1/devices").tag("Devices").post("/", create_device).schema(device_schema);

server.host("www.example.com").serve_static("/", "/var/www/site");

// Patterns: "*" matches one label, ":name" a label available as a request parameter
server.host("*.devices.example.com").get("/:path(.*)", [](http::request& req, http::response& res) {
    auto device = req.get_host_matches()[1];   // [0] is the whole host name
    // ... forward the request to the device
});
server.host(":tenant.example.com").get("/", [](http::request& req, http::response& res) {
    res.send("Welcome " + req["tenant"]);
});

// Regular expression on the whole host name (any depth of subdomains here)
server.host_regex("(.+)\\.proxy\\.example\\.com").get("/:path(.*)", proxy_handler);

// Default host: any other name
server.get("/", [](http::response& res) { res.send("default site"); });
```

Hosts are matched in order: exact names, then patterns in registration order, then the default host (`server.host("*")` returns the server itself). Register them before `listen()`. Once the host is chosen, everything works as for a single host: routes and `404`/`405` come from that host only, and the global middlewares (`server.use()`) run for every host after route matching, with `req.get_matched_route()` set to the route of the matched host and `req.get_virtual_host()` to the host. `server.openapi()` documents the default host; for another one build a generator over its router: `http::openapi_generator(server.host("api.example.com").router(), &server.get_body_settings())` (the body settings document the [body types](#body-types) of the application).

For HTTPS, the certificate is chosen per domain before any request is read, through SNI: register one per host name (wildcards such as `*.devices.example.com` included) with `thinger::asio::certificate_manager::instance().set_certificate(hostname, certificate, private_key)`. Virtual hosts then pick the routes for the same name from the `Host` header. See `examples/http_server/virtual_hosts_example.cpp`.

### Error Format

Errors sent with `res.error(status, message)`, and the ones the server generates itself (`400` for bodies that cannot be decoded, schema validation and invalid request framing or bodies, `401` of basic auth, `404`/`405`, `413`, `415` for body types not accepted, `500` from middlewares, handlers and invalid route schemas, `504` (or the status set) of [route timeouts](#limits-and-timeouts-per-route), and `501`/`504` of in-memory dispatch) go through an error formatter. By default the message is sent as `text/plain`, and errors with details (schema validation) as JSON `{"error": {"message": ..., "context": [...]}}`. A formatter changes it for all of them:

```cpp
server.set_error_formatter([](const http::http_error& error, http::http_response& response) {
    // error.status, error.message (may be empty), error.details (null, or {"context": [...]})
    nlohmann::json body = {{"error", {{"message", error.message.empty()
        ? http::http_response::get_reason_phrase(error.status) : error.message}}}};
    if (error.details.is_object()) body["error"].update(error.details);
    response.set_content(body.dump(), "application/json");
});

res.error(http::http_response::status::forbidden, "Not allowed");   // {"error":{"message":"Not allowed"}}
```

The formatter sets the body (and any header) of the response, whose status is already set. If it throws, the default format is used. Set it before `listen()`: it is called from every worker thread, and replacing it while serving is a data race.

### Client IP Behind Proxies

`req.get_request_ip()` is the address of the peer (`req.get_peer_ip()`) unless the peer is a trusted proxy. Then the client address is taken from the forwarding header: it is walked from right to left, skipping trusted proxies, and the first untrusted address is returned (the leftmost one if all of them are trusted).

```cpp
// IPs and CIDR ranges, IPv4 and IPv6; X-Forwarded-For by default
server.set_trusted_proxies({"10.0.0.0/8", "127.0.0.1", "fd00::/8"});

// Or the standard Forwarded header (RFC 7239): for=192.0.2.60;proto=https
server.set_trusted_proxies({"10.0.0.1"}, http::forwarded_header::forwarded);
```

Set them before `listen()` (they are read by every request, without locks). No proxy is trusted by default, and the header of any other peer is ignored, so a client cannot change its address by sending it: code that trusts a local address (e.g. `127.0.0.1` to skip checks) stays safe. List only proxies that append to (or overwrite) the header, and only the header they set: the other one reaches the server as the client sent it.

### Reverse Proxy

`http::reverse_proxy` forwards the request of a route handler to another server (a target) and sends its response back: over TCP, TLS or a Unix socket, streaming both bodies, and turning the connection into a tunnel for protocol upgrades such as WebSockets. Use it from a coroutine handler, before reading the body; the not-found handler of a virtual host forwards everything it receives, whatever the method:

```cpp
http::reverse_proxy proxy;   // thread-safe: one for every route and thread

// Everything on app.example.com, WebSockets included
auto app = *http::proxy_target::from_url("http://127.0.0.1:3000");
server.host("app.example.com").set_not_found_handler(
    [&](http::request& req, http::response& res) -> thinger::awaitable<void> {
        co_await proxy.forward(req, res, app);
    });

// /plugins/:name/...: to a Unix socket per plugin, without the prefix, with other credentials
server.get("/plugins/:name/:path(.*)", [&](http::request& req, http::response& res) -> thinger::awaitable<void> {
    http::proxy_options options;
    options.path = "/" + req["path"];                 // the query of the request is kept
    options.on_request = [](http::request&, http::http_request& outgoing) {
        outgoing.remove_headers("Authorization");
        outgoing.add_header("Authorization", "Bearer " + plugin_token);
    };
    options.on_response = [](http::request&, http::http_response& response) {
        response.add_header("X-Plugin", "1");
    };
    auto target = http::proxy_target::unix_socket("/run/plugins/" + req["name"] + ".sock");
    auto result = co_await proxy.forward(req, res, target, options);
    // result.status, result.complete, result.request_bytes, result.response_bytes, result.error...
});

// HTTPS target with a private CA and its own SNI name
auto device = *http::proxy_target::from_url("https://10.0.0.5:8443/ui");
device.tls.server_name = "device.internal";   // sent with SNI and verified in its certificate
device.tls.ca_file = "/etc/ssl/private-ca.pem";  // trusted instead of the system CAs
```

- **Target**: `proxy_target::from_url()` (`http://`, `https://`, `ws://`, `wss://`, with a base path prepended to every request) or `proxy_target::unix_socket()`. TLS targets are verified by default (chain and name), against the system CAs or only `ca_file`/`ca_pem` if set; `verify = false` is for testing only.
- **Request**: method, path and query, and the end-to-end headers. The path can be replaced (`options.path`), and the `Host` header is the target's unless `preserve_host` or `host` are set. Hop-by-hop headers are removed: `Connection` and the headers it lists, `Keep-Alive`, `TE`, `Trailer`, `Transfer-Encoding`, `Upgrade` (except on upgrades), `Proxy-*`, and `Expect`. A client sending `Expect: 100-continue` gets the `100 Continue` from the proxy once it is connected to the target and starts streaming the body to it, as nginx does (the interim responses of the target are never relayed): if the target cannot be reached, it gets the `502` without sending the body. `TRACE`, `CONNECT` and unknown methods are not forwarded.
- **Forwarding headers**: `X-Forwarded-For`, `-Proto` and `-Host` by default, `Forwarded` (RFC 7239) or none (`options.forwarding`). They agree with `req.get_request_ip()`: the ones received from a [trusted proxy](#client-ip-behind-proxies) (the header it sets) are kept and extended with the peer; from any other peer they are dropped, as the client could forge them.
- **Bodies** are streamed with flow control both ways: the request body goes to the target as the client sends it, and the response to the client as the target sends it (chunked responses and event streams included), each part read only once the previous one was written, so neither is ever held whole. `max_body_size` limits the request body (413), as does the [maximum body size of the route](#limits-and-timeouts-per-route): the smaller one wins.
- **Upgrades**: a request with `Connection: upgrade` (WebSocket or any other protocol) gets the `101` of the target, then the connection becomes a byte tunnel to it, starting with whatever either side sent right after the handshake. `allow_upgrade = false` forwards them as plain requests.
- **Errors**, through the [error formatter](#error-format): `502` if the target cannot be reached (connection refused, TLS verification) or its response is invalid, `504` if it times out. `connect_timeout` (10 s) covers the connection and TLS handshake, `idle_timeout` (60 s, `0` disables it, e.g. for event streams) the time without data in either direction, and `total_timeout` (none by default) the whole exchange. Once the response has started, a failure closes the connection with the client without ending the body, so it sees it incomplete.
- **Hooks**: `on_request` changes the request for the target (method, path, headers), `on_response` the status and headers of its response (also of a `101`), before the body is relayed: they get the incoming request too, and can edit, remove or add any header, repeated ones included (each `Set-Cookie` is a header of its own in `get_headers()`). Headers they add with invalid characters fail the request (`500`) instead of being sent.

The proxy never changes the end-to-end headers of the target by itself: frame protections, cookies and redirects reach the client as the target sent them. Rewriting them is an application policy, expressed with `on_response`, and each rule has a cost. `examples/http_server/device_proxy_example.cpp` (with `device_proxy_policy.hpp`) serves device web interfaces inside a console:

- **Frames**: removing `X-Frame-Options` and `Content-Security-Policy` lets any site embed the pages (clickjacking). The recommended variant replaces them with `frame-ancestors <console origin>`, so only the console can embed them.
- **Cookies**: dropping `Domain` keeps them on the proxy host. Forcing `SameSite=None; Secure` sends them on requests from other sites too, widening the exposure to CSRF: only for a console on another site.
- **Redirects**: a `Location` pointing to the device (its host, or an absolute path) is rewritten under the proxy path, parsing the URL (scheme, userinfo, host) instead of matching substrings; other hosts are left alone.

Framing is never forwarded as received, to avoid request smuggling between the proxy and the target. The request body is framed again from what the strict server parser accepted (requests with ambiguous framing are rejected before reaching the handler): its `Content-Length`, or chunked again without extensions or trailers, and never past its end, so a pipelined request is never sent as part of it. The response of the target is parsed strictly too, by the parser of the [HTTP client](#response-parsing): a status line other than `HTTP/1.x` with a three-digit code, obsolete line folding, `Transfer-Encoding` other than a single `chunked`, `Content-Length` together with it or invalid (or repeated with different values) are answered with `502`; its body is decoded and framed again for the client (chunked, or ended by closing the connection for HTTP/1.0 clients), and anything the target sends after its end is dropped. Each request uses a new connection to the target, closed after the response; reusing them is left to a connection pool.

The flow control comes from `res.start_stream(head, content_length)`, `co_await res.write_stream(data)` and `co_await res.end_stream()`, available to any handler: each write completes once the data is written to the connection, the framing is chosen by the response (`Content-Length` if given, chunked, or closing the connection for HTTP/1.0 clients), and a body that does not match its `Content-Length` closes the connection instead of desynchronizing it. See `examples/http_server/reverse_proxy_example.cpp`.

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

`stop()` stops listening right away and leaves the connections open as they are (on `http::server`, its io_context stops too). For a graceful stop, see below.

Configure the server before `listen()`/`start()`: routes, [virtual hosts](#virtual-hosts), middlewares, the [error formatter](#error-format), [trusted proxies](#client-ip-behind-proxies), body size, decoders, types and limits, and CORS. Every request reads them, from any worker thread, without locks (they cost nothing per request), so changing them while the server is serving is a data race.

### Shutdown

`shutdown(grace)` stops the server gracefully, e.g. before a deploy or on `SIGTERM`:

- it stops listening right away: new connections are refused (TCP and Unix sockets; the socket file is removed);
- idle keep-alive connections are closed at once;
- a connection handling a request finishes it: its response tells the client with `Connection: close` (unless the response already started: then the connection is closed after it), and the connection closes after it;
- connections taken over are told (once, on their thread): a `websocket_connection` closes with 1001 (Going Away), an `sse_connection` closes, and others call what the application set with `on_shutdown()` (`websocket_connection`, `sse_connection`), or with `http::on_server_shutdown(socket, callback)` for the socket of `take_over()` or `upgrade_websocket_stream()`. Proxied WebSockets (reverse proxy tunnels) are not told: they end when either side closes. The server calls `on_shutdown()` once when the shutdown starts, for anything else the application runs;
- once every connection is gone, or the grace period passes, whatever is left is closed, and the server stops as with `stop()` (`wait()` returns).

`shutdown()` waits for that end, and returns whether every connection finished in time. `begin_shutdown(grace, on_done)` returns at once, and calls `on_done(clean)` at the end. Both can be called from any thread, several times (later calls wait for the same end), and from an `asio::signal_set` handler (not from a raw signal handler: they lock and allocate). On a thread of the server (a handler, or a worker of a `pool_server`) only `begin_shutdown()` can be used: `shutdown()` cannot wait there, so it starts it and returns `false`. `stop()` during a shutdown ends it at once (`false`).

```cpp
thinger::http::pool_server server;
// ... routes ...
server.on_shutdown([&] { devices.disconnect_all(); });
server.listen("0.0.0.0", 8080);

boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
signals.async_wait([&](auto, int) {
    server.begin_shutdown(std::chrono::seconds(30), [](bool clean) {
        LOG_INFO("shut down{}", clean ? "" : " (some connections were cut)");
    });
});
server.wait();  // returns once the shutdown ends

// a WebSocket that says goodbye its own way
res.upgrade_websocket([](std::shared_ptr<thinger::http::websocket_connection> ws) {
    ws->on_shutdown([ws] { ws->send_text("{\"event\":\"restarting\"}"); ws->stop(); });
});
```

A `pool_server` can leave the signals to the workers: `wait_graceful(grace)` waits for `SIGINT`, `SIGTERM` or `SIGQUIT` (or the signals given), and on the first one shuts every `pool_server` down gracefully, as `begin_shutdown(grace)` (the other worker clients, as `async_client`, keep working meanwhile, for the requests that are finishing), then stops the workers and returns. A second signal during the grace period stops them at once, as the first one does with `wait()`.

```cpp
thinger::http::pool_server server;
// ... routes ...
server.listen("0.0.0.0", 8080);
thinger::asio::get_workers().wait_graceful(std::chrono::seconds(30));  // on SIGINT, SIGTERM or SIGQUIT
```

Compared with Node.js: `server.close()` also stops listening, closes idle connections (since Node 19) and waits for the others, but has no grace period (that is `closeAllConnections()`, called by the application) and does not tell upgraded connections, which keep it waiting until they close. Here `shutdown(grace)` does `close()`, `closeIdleConnections()` and, once the grace period ends, `closeAllConnections()`, and new connections accepted meanwhile (e.g. in the middle of a TLS handshake) are closed at once.

### Statistics

`stats()` returns the connections open by what they serve (`http_connections`, `websocket_connections`, `sse_connections`, `tunnel_connections` for reverse proxy upgrades, `taken_over_connections` for other `take_over()`s, and `active_connections()`), and the connections served since the server was created (`total_connections`). Counting costs nothing per request: a connection joins or leaves a list (picked by its io_context, so threads do not contend for it) only when it opens, changes what it serves and closes; `stats()` reads those lists, so it is meant for a monitoring endpoint, not for every request. There is no request counter, nor bytes in and out: they would cost on every request. A connection taken over counts until its socket is destroyed, or until closed for `websocket_connection` and `sse_connection`.

```cpp
server.get("/v1/server/statistics", [&server](thinger::http::response& res) {
    auto stats = server.stats();
    res.json({{"http", stats.http_connections}, {"websocket", stats.websocket_connections},
              {"sse", stats.sse_connections}, {"total", stats.active_connections()}});
});
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

### Unix Socket

Connect to services via Unix domain sockets (e.g., Docker, systemd). Configure once on the client — all methods work transparently:

```cpp
thinger::http::client client;
client.unix_socket("/var/run/docker.sock");

// Same API as TCP — the socket is used automatically
auto containers = client.get("http://localhost/containers/json");
auto result = client.post("http://localhost/containers/create",
    R"({"Image": "nginx"})", "application/json");
```

Also available via the request builder for per-request control:

```cpp
auto res = client.request("http://localhost/containers/json")
    .unix_socket("/var/run/docker.sock")
    .get();
```

### Configuration

```cpp
client.timeout(std::chrono::seconds(30));
client.follow_redirects(true);
client.max_redirects(10);
client.verify_ssl(false);  // Disable SSL verification
client.unix_socket("/path/to/socket");  // Unix domain socket
client.max_content_size(8 * 1048576);  // Largest buffered body, also once decompressed (8 MB)
client.auto_decompress(false);  // Bodies as received: no Accept-Encoding, gzip/deflate not decoded
```

### Connection Pool (Keep-Alive)

Each client keeps its connections open between requests (RFC 9112 §9.3): sequential requests to a server reuse one connection instead of connecting (and negotiating TLS) again. A connection carries one request at a time; concurrent requests to the same server open more.

```cpp
client.max_idle_connections_per_host(8);   // idle connections kept per origin (0 disables the pool)
client.max_idle_connections(100);          // in total
client.idle_timeout(std::chrono::seconds(4));

auto stats = client.pool_stats();          // idle and in use, per origin; created and reused
client.clear_connections();                // close the idle ones
```

| Setting | Default | Why |
|---|---|---|
| Idle per origin | 8 | Go keeps 2 (`MaxIdleConnsPerHost`), too few for bursts of concurrent calls: the extra connections are closed and opened again on the next burst. undici and Node keep any number |
| Idle in total | 100 | As Go (`MaxIdleConns`) |
| Idle timeout | 4 s | As undici: shorter than the idle timeout of most servers (5 s in Node and Apache, 60-75 s in nginx and load balancers), so the server rarely closes a connection just as a request is sent on it. Go keeps them 90 s, relying on retrying |

- **Reuse**: a connection is given back to the pool only after a response read whole (a streamed one too, unless its callback aborted it), delimited by its length or chunked, with nothing received past it, and neither the server (`Connection: close`, HTTP/1.0 without keep-alive) nor the request asked to close it. Never after an error, a timeout or a `101` (WebSocket): `send_with_connection()` hands over a connection that switched protocols, and lets the pool reuse any other. The most recently used connection is reused first; when a limit is reached, the oldest idle one is closed.
- **Server timeout**: `Keep-Alive: timeout=N` in a response shortens the idle timeout to N - 1 s (as undici, which subtracts 1-2 s); with N ≤ 1 the connection is not kept.
- **Closed while idle**: a server may close an idle connection at any time. Before reusing one, the client checks without waiting whether the server (or proxy) closed it, reset it or sent anything: then it is closed and the request goes on another, whatever its method, as nothing was sent.
- **Retries**: if a request sent on a reused connection fails before any byte of its response arrives (the server closed the connection as the request was sent), it is sent once more on a new connection when that is safe, as Go does: nothing of it was written, or its method is idempotent (`GET`, `HEAD`, `OPTIONS`, `TRACE`, `PUT`, `DELETE`, RFC 9110 §9.2.2), or it has an `Idempotency-Key` (or `X-Idempotency-Key`) header. A `POST` or `PATCH` is not sent again, as the server may have processed it: the error is returned. Neither are requests on new connections, nor after a timeout. curl retries any method; Node (`http.Agent`) does not retry, and undici only with its opt-in `RetryAgent`.
- **Keys**: connections are shared by requests to the same origin (scheme, host, port, or Unix socket) reaching it the same way (directly or through the same proxy and credentials) and trusting the same CAs. Through a forwarding proxy (plain `http://`), the connection reaches the proxy, so it is shared by requests to any origin, as curl does; tunnels (`CONNECT`) belong to their origin.
- **Nagle**: client connections set `TCP_NODELAY`, as Go, Node and curl do; otherwise a request on a reused connection may wait for the delayed ACK of the server (40 ms).
- **Lifetime**: the `async_client` closes idle connections with a timer on its workers. The synchronous `client` runs its io_context only during a call, and nothing of it runs between calls: it closes the expired ones whenever a request starts or ends, and never reuses them. So after a call, the connections it used in the last idle timeout stay open until the next call, half-closed (`CLOSE_WAIT`) once their servers close them: usually one per server, never more than the limits. curl's easy interface and Python's requests (urllib3) keep them the same way; Go and Node close them on time, as their event loop always runs, and Java's `HttpURLConnection` with a thread of its own. A long-lived `client` making sporadic calls can call `clear_connections()` after them, or disable the pool. Destroying the client, or stopping the `async_client` (or the workers), closes its idle connections.

### Proxies

Requests can go through an HTTP proxy. Plain `http://` requests are sent to the proxy with the full URL as target (absolute-form, RFC 9112 §3.2.2); `https://`, `ws://` and `wss://` ones go through a tunnel opened with `CONNECT host:port` (RFC 9110 §9.3.6), with TLS end to end: SNI and the certificate are those of the origin, never of the proxy. WebSockets use a tunnel even without TLS, as browsers do, since forwarding proxies may not pass upgrades on.

By default the client reads the proxy from the environment, with the conventions of curl:

| Variable | Used for |
|---|---|
| `http_proxy` | `http://` and `ws://` requests. **Only lowercase**: `HTTP_PROXY` is ignored, as in a CGI program it holds the `Proxy` header of the request being served ([httpoxy](https://httpoxy.org), CVE-2016-5385) |
| `https_proxy`, `HTTPS_PROXY` | `https://` and `wss://` requests |
| `all_proxy`, `ALL_PROXY` | both, when the variable of the scheme is not set |
| `no_proxy`, `NO_PROXY` | hosts reached directly |

Lowercase comes first; empty variables count as not set. The environment is read when the client is created.

```cpp
thinger::http::client client;

// One proxy for every scheme (replaces the environment), with the hosts reached directly
client.proxy("http://user:password@proxy.corp:3128", "internal.corp, 10.0.0.0/8");

// Or one per scheme
client.proxy(http::proxy_config{
    .http = "http://proxy.corp:3128",
    .https = "http://proxy.corp:3129",
    .no_proxy = "internal.corp"});

client.proxy("");                       // no proxy at all, whatever the environment says
client.proxy_from_environment(false);   // ignore the environment (when proxy() is not set)

// Per request (and its redirects), over the client settings
auto res = client.request("https://api.example.com/data")
    .proxy("http://other-proxy:8080")   // or .proxy("") to connect directly
    .get();

// Which proxy a URL would use ("http://host:port", empty if direct)
std::string via = client.proxy_for("https://api.example.com/");
```

**Precedence**: the settings of the request, then those set with `proxy()`, then the environment (unless disabled). Each replaces the next one whole, `NO_PROXY` included.

**Proxy URLs**: `http://[user:password@]host[:port]`. The scheme is optional (`proxy.corp:3128` is an HTTP proxy) and the port 80 by default, as in Go and Node (curl uses 1080). User and password are percent-decoded and sent as `Proxy-Authorization: Basic` to the proxy only: in the `CONNECT` of a tunnel, or with each forwarded request, never to the origin. Only HTTP proxies are supported: an `https://` proxy (TLS to the proxy) or a SOCKS one fails the requests that would use it with `client_error::invalid_proxy`, as does an invalid URL, rather than connecting directly.

**NO_PROXY**: comma separated, case-insensitive, spaces trimmed. Host names are compared with the host of the URL, never resolved.
- `example.com`, `.example.com` and `*.example.com` all match `example.com` and its subdomains (as curl; Go matches only the subdomains with a leading dot);
- IP addresses (`10.1.2.3`, `::1`, `[::1]`) and CIDR ranges (`10.0.0.0/8`, `fd00::/8`);
- any of them with a port (`example.com:8080`, `[::1]:8443`) only for that port;
- `*` alone matches every host.

`localhost`, `*.localhost`, loopback addresses (`127.0.0.0/8`, `::1`) and Unix sockets never use the proxy, as in Go: a proxy reaching "localhost" would reach itself.

**Redirects**: the proxy is chosen again for each hop, so a redirect to a `NO_PROXY` host goes directly (and the other way round).

**Errors**: when the proxy refuses the tunnel, the request fails with `client_error::proxy_authentication_required` (407) or `client_error::proxy_tunnel_failed` (any other status), and the response of the proxy is kept: `res.status()`, its headers (`Proxy-Authenticate`) and body (`stream_result::status_code` when streaming). A plain `http://` request through a forwarding proxy gets the 407 as its response, without error. The answer to `CONNECT` is parsed as strictly as any response (a 2xx has no body, and nothing may follow it before the client speaks). The timeout of the client covers the connection to the proxy and the `CONNECT` too.

**Connections** are only shared by requests reaching the origin the same way (directly or through the same proxy, with the same credentials) and trusting the same CAs; through a forwarding proxy, by requests to any origin (see [Connection Pool](#connection-pool-keep-alive)).

Streaming works the same way through a proxy, forwarded or tunneled: the stream callback (`request(...).get(callback)`, `send_streaming()`) and the chunked callback of a request (`http_request::set_chunked_callback()`, e.g. to follow Docker logs) get the body as it arrives.

### Trusted CAs

Servers are verified against the system CAs by default (`verify_ssl(true)`). Private CAs (an internal PKI, or a TLS-inspecting proxy re-signing traffic) can be trusted on top of them, per client or per request, from a PEM file (one or more certificates) or PEM text:

```cpp
client.add_ca_file("/etc/ssl/private-ca.pem");   // besides the system CAs
client.add_ca_pem(pem_text);
client.use_system_ca(false);                     // only the CAs added

auto res = client.request("https://kubernetes.default.svc/api")
    .ca_file("/var/run/secrets/kubernetes.io/serviceaccount/ca.crt")   // this request only
    .get();
```

The certificate is still verified for the host of the URL. A CA that cannot be loaded fails the requests needing it (`client_error::invalid_ca_certificate`). TLS contexts are built once per set of trusted CAs and shared by the connections of the client; a CA file is loaded again when it changes (its modification time or size).

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

When there is no response, `res.error_code()` tells why: the error of the connection (`connect`, `timed_out`...) or a `thinger::http::client_error` for a response that could not be read (`res.error()` and, when streaming, `stream_result::error` have its message):

```cpp
if (res.error_code() == http::client_error::invalid_framing) { /* ... */ }
```

### Response Parsing

Responses are parsed as strictly as the server parses requests, so a response is never read past its end, nor the next one on a connection taken from its body (RFC 9112). Anything else fails the request, and the connection is closed:

- **Status line**: `HTTP/1.x` (a higher minor version is handled as 1.1) and a three-digit status from 100 to 599 (`invalid_status_line`).
- **Header lines**: CRLF line endings, a token as name, no obsolete line folding nor control characters (`invalid_header`); the whitespace around a value is not part of it. At most 100 lines and 16 KB (`header_section_too_large`).
- **Body**: none for responses to `HEAD`, 1xx, 204 and 304, whatever their headers. Otherwise `Transfer-Encoding: chunked` (decoded strictly, trailers dropped: `malformed_body`), `Content-Length`, or else until the server closes the connection. `Transfer-Encoding` other than a single `chunked` (no other coding is supported), in an HTTP/1.0 response or together with `Content-Length`, and a `Content-Length` other than digits, or repeated with different values, are `invalid_framing`. A response cut by the end of the connection is `incomplete_response`; one larger than `max_content_size()` (8 MB) is `body_too_large`, unless streamed.
- **Content-Encoding**: a `gzip` (or `x-gzip`) or `deflate` body is decoded once read whole (and the header removed), within `max_content_size()` too: inflating stops as soon as the decoded body goes past it (`body_too_large`), so a body of a few KB expanding to gigabytes (a decompression bomb) never takes more memory than the limit. A body that is not valid gzip or deflate (corrupt or truncated) is `bad_content_encoding`, rather than handed compressed. Codings follow RFC 9110 and what curl, Go and Node's `fetch` accept:
  - Names are case-insensitive, and `x-gzip` is gzip.
  - gzip decodes every member of the body (concatenated gzip files, RFC 1952), the limit applying to all of them, and fails on anything after the last one but zero bytes (padding, which Node and `gzip -d` accept), as Go does.
  - deflate is the zlib format, or raw deflate without the zlib header (as some servers send it; curl and browsers accept it). Nothing may follow the end of the stream.
  - A list of codings (`Content-Encoding: deflate, gzip`, or several headers) is undone in reverse order, as they were applied, each step within the limit; at most five of them (as curl and Node's `fetch`), else `bad_content_encoding`. A list with a coding that is not supported (`gzip, br`) is kept as it is, with nothing decoded, as Node's `fetch` does.

  Other codings, empty bodies (responses to `HEAD`), and every body with `auto_decompress(false)`, are kept as they are. Streamed bodies (the stream callback, `download()`) and the chunks of `http_request::set_chunked_callback()` are handed as received, never decoded, so they are never inflated: streaming asks for `Accept-Encoding: identity`, unless the request sets its own, and then the body is what the server sent (compressed if its `Content-Encoding` says so). The limit is on the decoded size, as `maxOutputLength` bounds a buffered inflate in Node's `zlib`; there is no limit on the compression ratio (neither Node, Go nor curl has one).
- **Interim responses** (`100 Continue`, `103 Early Hints`) are skipped. `101 Switching Protocols` ends the response, and is accepted only for a request with `Upgrade` (`unexpected_upgrade`).
- **Connection reuse**: only after a response delimited by its length or chunked, kept alive, and with nothing received past its end (see [Connection Pool](#connection-pool-keep-alive)).

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

A client offering several subprotocols (`Sec-WebSocket-Protocol: a, b`) gets the first one it offers that is in the set; one offering none of them, or none at all when the set is not empty, is answered with `400`.

### WebSocket Flow Control

`send_text()` and `send_binary()` queue the message and return. The queue holds up to 100 messages; one sent to a full queue is dropped (the newest), which suits sampled telemetry but not a stream that must arrive whole. For those, the server can follow the pace of the client, and stop reading a client whose data it cannot forward yet:

```cpp
res.upgrade_websocket([](std::shared_ptr<http::websocket_connection> ws) -> thinger::awaitable<void> {
    // Wait for each message to be written to the socket: one message queued at a time
    for (auto& chunk : chunks) {
        if (!co_await ws->send_binary_async(chunk)) co_return;   // closed
    }
});

// Or queue without waiting, and wait while the queue is above its high watermark
ws->set_write_limits({.max_messages = 0, .high_watermark = 1 << 20, .low_watermark = 256 << 10});
ws->send_binary(chunk);
co_await ws->wait_writable();

// Stop reading while the destination of the messages is congested
ws->on_message([ws, sink](std::string message, bool binary) {
    sink->write(std::move(message));
    if (sink->congested()) {
        ws->pause_reading();
        sink->on_drain([ws] { ws->resume_reading(); });
    }
});
```

- **Awaited sends** (`send_text_async()`, `send_binary_async()`) complete once the message is written to the socket, as Go's `WriteMessage`: `true`, or `false` if the connection closed first. They are never dropped.
- **Limits** (`set_write_limits()`, a `http::write_limits`): `max_messages` (100 by default) and `max_bytes` (no limit by default) of the queue, the message being written included; an empty queue always takes a message. `queued_bytes()` and `queued_messages()` tell what is not written yet; they are atomic, so any thread may read them (as Node's `bufferedAmount`). `queue_full()` tells whether the overflow policy applies to the next message, which is not the watermark state.
- **Watermarks**, as Netty's `WriteBufferWaterMark` and `isWritable()`: once the bytes queued reach `high_watermark` the connection is not writable, and `wait_writable()` waits, until they drain to `low_watermark`. `on_high_watermark()` and `on_low_watermark()` are told of each change asynchronously, posted to the thread of the connection as Node emits `'drain'`: never from within a send or a drop, so they may send themselves, and never once the connection is closing. Changing the limits re-evaluates the state: disabling the watermarks, or raising the low one above what is queued, makes it writable.
- **Overflow policy** of a full queue, for the messages sent without waiting: `drop_newest` (the default), `drop_oldest` (the oldest messages queued, never the one being written nor an awaited one; when that is not enough, the new message is dropped, so the limits hold), or `close`, which closes the connection with `1013 Try Again Later` (the code the IANA registry has for a server shedding a client, which may reconnect later; `1008 Policy Violation` is about the messages a peer sends).
- **Write timeout** (`write_timeout`, off by default): a message whose write to the socket does not complete in that time closes the connection with `1013`, as a client that stopped reading; awaited sends and `wait_writable()` then complete with `false`. It is measured per message, so set it well above the time a large message takes at the slowest rate a client may read.
- **Pausing reads**: after `pause_reading()` nothing more is read from the socket: called from `on_message()`, the message being delivered is the last one; from another thread, one more may be delivered, so the TCP window of the client fills and its writes block, as `socket.pause()` in Node; `resume_reading()` goes on where it stopped. While paused, the pings and the close frame of the client are not seen (as in any WebSocket not being read: the client may time out), and the keepalive is suspended, as its pongs could not be read: a dead client is not noticed until reading resumes, so bound the pause, or set a `write_timeout`. `stop()` still closes it: the closing handshake reads up to the close frame of the client, discarding the messages before it.

### WebSocket as a Socket

`res.upgrade_websocket_stream()` hands the accepted WebSocket out as a socket (`thinger::asio::websocket`, a `thinger::asio::socket`), with the same interface as TCP, TLS and Unix sockets: a protocol written for a socket (MQTT, a custom binary protocol...) runs over WebSocket unchanged, without telling the difference, and a tunnel connects it to any other socket.

```cpp
server.get("/mqtt", [](http::request& req, http::response& res) {
    res.upgrade_websocket_stream({.protocols = {"mqtt", "mqttv3.1", "mqttv3.1.1"}},
        [](std::shared_ptr<thinger::asio::websocket> ws, std::string protocol) {
            // protocol: the subprotocol accepted, e.g. "mqttv3.1"
            co_spawn(ws->get_io_context(), run_protocol(ws), thinger::detached);
        });
});

// A protocol written for any socket: a 4-byte length and a payload, answered with an echo
thinger::awaitable<void> run_protocol(std::shared_ptr<thinger::asio::socket> socket) {
    for (;;) {
        uint8_t header[4];
        auto [ec, bytes] = co_await socket->read(header, sizeof(header));  // exactly 4 bytes
        if (ec) co_return;  // boost::asio::error::eof once the client closed
        std::vector<uint8_t> payload((header[0] << 24) | (header[1] << 16) | (header[2] << 8) | header[3]);
        if (!payload.empty() && std::get<0>(co_await socket->read(payload.data(), payload.size()))) co_return;
        co_await socket->write({boost::asio::buffer(header), boost::asio::buffer(payload)});  // one message
    }
}
```

The handler runs on the io_context of the socket once the `101` response is written; like any socket, the WebSocket is used there (see [Thread Safety](#thread-safety)). Its options:

| Option | Default | Description |
|---|---|---|
| `protocols` | none | Subprotocols spoken, alternatives included. The first one the client offers (in its order of preference) that is in the list is accepted, returned in the response and passed to the handler |
| `protocol_required` | `true` | With `protocols`: a client offering none of them is answered with `400`. If `false`, it is accepted without a subprotocol (and the handler gets an empty one). Without `protocols`, any client is accepted without one |
| `binary` | `true` | Messages written are binary, or text (`ws->set_binary()` changes it later) |
| `ping_interval` | `0` (off) | Ping the client once nothing was received from it for this long |
| `idle_timeout` | `0` (off) | Close the connection once nothing was received for this long, pongs included |

The handshake is checked as RFC 6455 requires: a request that is not a WebSocket upgrade, or of another version than 13, gets `426` (with `Sec-WebSocket-Version: 13`); one that is not a `GET`, lacks `Connection: Upgrade` or has an invalid `Sec-WebSocket-Key`, `400`. Frames the client sends right after its request, without waiting for the response, are kept for the first reads.

As a stream:

- Reads return the payload of the data messages received, binary or text, as one continuous stream: neither messages nor the frames of a fragmented message are delimited. `read()` waits for exactly the bytes asked, across frames and messages; `read_some()` returns what one frame holds at most, and never 0 bytes (empty frames are skipped); `read_until()` and the `streambuf` reads work as on TCP. Text is not validated as UTF-8 (the stream may cut a character anywhere), nor are close reasons; `upgrade_websocket()` validates whole text messages.
- Each write sends one whole message (unfragmented). Writes may be called while others are pending, from several coroutines: they are sent one after another, in call order, never interleaved. Writing 0 bytes sends nothing. A write interrupted in the middle of its frame (`cancel()`, or the cancellation of the operation) leaves the rest of the frame to be sent before anything else, so the framing is never broken.
- Pings are answered (and unsolicited pongs ignored) while reading, so some read must be pending, as a protocol reading its socket has. The pong goes out as soon as no frame is being written, without holding the reads. The keepalive counts on reading too: what is not read is not received. It sends one ping at a time, and closes the connection if a ping cannot be sent.
- A close frame from the client is answered, the connection closed, and reads end with `boost::asio::error::eof`, as on a TCP connection closed by its peer. Writes fail afterwards.
- `close()` returns at once: pending operations end, the socket is closed for its user, and the closing handshake finishes on its own: the rest of a frame being written, a close frame (code 1000), and the connection closed when the client answers, or after 5 seconds. Called where nothing runs the io_context of the socket (e.g. by the synchronous client between calls), it closes the connection at once instead. `co_await ws->close_graceful()` does the handshake, waiting. A WebSocket destroyed open on its io_context sends 1001 (going away), and so does the idle timeout; protocol errors (an unmasked frame, an unknown opcode...) are answered with 1002.

For message-oriented code `upgrade_websocket()` (above) remains; `ws->read_message_some()` with `is_message_complete()` and `is_binary()` gives the message boundaries over the same socket.

### Tunnels

`thinger::asio::socket_pipe` copies data both ways between two sockets until either end closes: a WebSocket and a TCP, TLS or Unix socket (a VNC server, a Docker container, a service behind a proxy), or a socket of your own (see [Custom Sockets](#custom-sockets)):

```cpp
server.get("/vnc", [](http::request& req, http::response& res) {
    res.upgrade_websocket_stream({.protocols = {"binary"}}, [](std::shared_ptr<thinger::asio::websocket> ws, std::string) {
        co_spawn(ws->get_io_context(), [ws]() -> thinger::awaitable<void> {
            // the other end, on the io_context of the WebSocket
            auto target = std::make_shared<thinger::asio::tcp_socket>("vnc", ws->get_io_context());
            if (co_await target->connect("10.0.0.5", "5900", std::chrono::seconds(10))) {
                ws->close();
                co_return;
            }
            auto pipe = std::make_shared<thinger::asio::socket_pipe>(ws, target);
            co_await pipe->run();   // until either end closes (or pipe->start() to run it detached)
        }, thinger::detached);
    });
});
```

- Each direction writes what it read before reading more, so a slow reader slows its writer down (flow control): nothing piles up in memory.
- When either end closes (or fails), the pipe closes both: the WebSocket client gets a close frame when the other end closes, and the other end is closed when the client closes. `pipe->cancel()` ends it from any thread.
- Both sockets are used from the coroutine running the pipe: create the other one on the io_context of the WebSocket (`unix_socket` and `ssl_socket` take it in their constructors too).

### Client WebSocket as a Socket

A WebSocket opened by the client is a socket too, e.g. to tunnel to a WebSocket target or to run a protocol over it:

```cpp
auto ws = co_await client.websocket("wss://server.com/tunnel", "binary");
if (ws) {
    // ws->get_protocol(): the subprotocol the server accepted
    std::shared_ptr<thinger::asio::websocket> socket = ws->release_socket();
    socket->set_binary(true);   // the client writes text messages by default
    auto pipe = std::make_shared<thinger::asio::socket_pipe>(socket, other_socket);
    co_await pipe->run();
}
```

It is used on its io_context (`socket->get_io_context()`): with `async_client`, the one of the coroutine that opened it.

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

## Custom Sockets

Protocols and tunnels work on `thinger::asio::socket`, so an application can define its own socket type and use it wherever a socket is used, e.g. a virtual stream multiplexed inside another connection as an end of a `socket_pipe`, or under a protocol. A socket must implement:

| Method | Guarantee |
|---|---|
| `read_some(buffer, max_size)` | Waits until at least one byte is available and returns it: never 0 bytes without an error (unless `max_size` is 0). `boost::asio::error::eof` once the other end closed the stream and everything it sent was read |
| `write(buffer, size)` | Returns once all the bytes are written (accepted, as a TCP socket copies them to the kernel), or with an error |
| `close()` | Closes right away, without waiting: `is_open()` is `false` afterwards, pending operations complete with an error and new ones fail. Idempotent, and safe from the destructor |
| `cancel()` | Pending operations complete with `operation_aborted`; the socket stays open |
| `is_open()` | Whether it is open |

Everything else has a default: `read()` (exact), `read(streambuf)`, `read_until()` and the other `write()` overloads are built on these; `connect()` and `wait()` answer `operation_not_supported`; `is_secure()` is `false`, `available()` 0 and the address getters empty. Override them when the socket can do better. As C++ hides the overloads of a name a class declares, add `using socket::read;` and `using socket::write;` to call the default overloads on the derived type.

Sockets are not thread-safe: every call, `close()` and `cancel()` included, happens on a thread running the io_context passed to the constructor, and operations complete there. Data arriving from another thread (e.g. from the connection the virtual stream runs inside) is handed over by posting it to that io_context. At most one read and one write are pending at a time. Pending operations should support per-operation cancellation (asio operations and timers do), used by the awaitable operators (`||`); `close()` must end them anyway.

```cpp
// A virtual stream: what is written goes to `send`; what is received comes from push(),
// called from any thread (an empty string ends the stream)
class virtual_stream : public thinger::asio::socket, public std::enable_shared_from_this<virtual_stream> {
public:
    virtual_stream(boost::asio::io_context& io_context, std::function<void(std::string)> send)
        : socket("virtual", io_context), signal_(io_context), send_(std::move(send)) {}

    // keep the default overloads visible next to the ones implemented
    using socket::read;
    using socket::write;

    void push(std::string data) {
        boost::asio::post(io_context_, [self = shared_from_this(), data = std::move(data)] {
            if (data.empty()) self->eof_ = true;
            self->inbox_ += data;
            self->signal_.cancel();     // wakes the pending read
        });
    }

    thinger::awaitable<thinger::io_result> read_some(uint8_t buffer[], size_t max_size) override {
        for (;;) {
            if (!open_) co_return thinger::io_result{boost::asio::error::bad_descriptor, 0};
            if (!inbox_.empty()) {
                size_t bytes = std::min(max_size, inbox_.size());
                std::memcpy(buffer, inbox_.data(), bytes);
                inbox_.erase(0, bytes);
                co_return thinger::io_result{boost::system::error_code{}, bytes};
            }
            if (eof_) co_return thinger::io_result{boost::asio::error::eof, 0};
            // wait for push(), close() or cancel(), or the cancellation of this read
            auto generation = cancelled_;
            signal_.expires_at(std::chrono::steady_clock::time_point::max());
            co_await signal_.async_wait(thinger::use_nothrow_awaitable);
            auto state = co_await boost::asio::this_coro::cancellation_state;
            if (cancelled_ != generation || state.cancelled() != boost::asio::cancellation_type::none)
                co_return thinger::io_result{boost::asio::error::operation_aborted, 0};
        }
    }

    thinger::awaitable<thinger::io_result> write(const uint8_t buffer[], size_t size) override {
        if (!open_) co_return thinger::io_result{boost::asio::error::bad_descriptor, 0};
        send_(std::string(reinterpret_cast<const char*>(buffer), size));
        co_return thinger::io_result{boost::system::error_code{}, size};
    }

    void close() override { open_ = false; signal_.cancel(); }
    void cancel() override { ++cancelled_; signal_.cancel(); }
    bool is_open() const override { return open_; }

private:
    boost::asio::steady_timer signal_;
    std::function<void(std::string)> send_;
    std::string inbox_;
    bool eof_ = false;
    bool open_ = true;
    uint64_t cancelled_ = 0;
};
```

This `write()` returns at once; a socket whose other end may be slow should wait (e.g. for room in a window) to keep the flow control of a tunnel. `tests/fixtures/stream_test_helpers.hpp` has a complete one, used in tests in a tunnel with a WebSocket.

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

The queue of an SSE connection has the same flow control as a WebSocket, counting fields (`send_event()`, `send_data()`, `send_retry()`): `set_write_limits()` (up to 101 fields by default, dropping the newest), `queued_bytes()`, the watermarks, the write timeout and `wait_writable()`. `drop_oldest` drops whole events, never the rest of an event being written (fields of an event without its data yet are not dropped: the newest field is, then); `close` and the write timeout close the connection, and `EventSource` reconnects.

```cpp
res.start_sse([](std::shared_ptr<http::sse_connection> sse) -> thinger::awaitable<void> {
    sse->set_write_limits({.max_messages = 0, .high_watermark = 1 << 20, .low_watermark = 256 << 10});
    for (auto& update : updates) {
        sse->send_data(update);
        if (!co_await sse->wait_writable()) co_return;   // closed
    }
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

### What a Server Logs When It Starts and Stops

At `info`, a server logs one line when it starts listening, one when a graceful shutdown starts (with the connections open and the grace period) and one when it stopped, as Node and Go servers do; the workers log the signal that stops them (`wait()`, `wait_graceful()`). Warnings, as a self-signed certificate generated for want of one, stay. The threads and the steps of starting and stopping (workers, acceptors, worker clients) log at `debug`.

```text
listening on http://0.0.0.0:8080
listening on https://0.0.0.0:8443
received signal 15: stopping gracefully, within 30000 ms (a second signal stops at once)
shutting down the server on http://0.0.0.0:8080: 3 connections open, grace period of 30000 ms
shutting down the server on https://0.0.0.0:8443: 0 connections open, grace period of 30000 ms
server on https://0.0.0.0:8443 stopped
server on http://0.0.0.0:8080 stopped
```

### What the Message Logs Show

The client and the server log each request and response: the request line or the status (`info`), the header names (`debug`) and the body size (`trace`). In the request line, the values of query parameters that carry credentials (`access_token`, `authorization`, `token`, `api_key`, `password`...) are shown as `[redacted]`. Header values, bodies and WebSocket messages are never logged by default, as they may hold credentials: tokens, cookies, passwords, private keys.

For debugging, `log_bodies()` also logs the header values (`debug`) and the start of the bodies (`trace`), up to a maximum length, with control characters escaped. The values of `Authorization`, `Proxy-Authorization`, `Cookie` and `Set-Cookie` are redacted even then, and WebSocket messages are still logged by their size alone.

```cpp
http::server server;
server.log_bodies(true, 512);      // before the server starts

http::client client;
client.log_bodies(true, 512);      // fluent, as the other client settings
```

```text
[CLIENT->] POST /login
Headers:
  Authorization: [redacted]
  Content-Type: application/json
Body: 1834 bytes
  {"user":"alice","note":"..."}... (1322 more bytes)
```

## Build Options

| CMake Option | Default | Description |
|--------------|---------|-------------|
| `THINGER_HTTP_ENABLE_LOGGING` | `ON` | Enable spdlog integration |
| `THINGER_HTTP_ENABLE_VALIJSON` | `ON` | Enable JSON Schema validation |
| `THINGER_HTTP_ENABLE_SSL` | `ON` | Enable SSL/TLS support |
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
- `http::request_context` - Not synchronized: used by the coroutine processing its request (middlewares, then handler)
- `http::response` - Copies share the response and can answer from any thread, even at the same time: only the first answer is sent. Chunks (`write_chunk()`, `end_chunked()`) are written in call order once `start_chunked()` returned `true`. Set the status and headers (`status()`, `header()`) from the thread running the handler, before answering, also on routes with a timeout: a timeout answer (504) copies the headers set so far
- `http::client` - Single-threaded, create one per thread or use `async_client`
- `http::async_client` - Thread-safe, uses internal worker threads
- `http::websocket_client` - Single connection, thread-safe send operations
- `http::websocket_connection` - Server-side WebSocket, thread-safe send, callbacks on IO thread
- `asio::socket` (TCP, TLS, Unix, `asio::websocket`, custom sockets) - Not thread-safe, like the asio objects they wrap: use a socket on a thread running its io_context (`get_io_context()`), dispatching there from other threads. `socket_pipe::cancel()` may be called from any thread

## License

MIT License - see [LICENSE](LICENSE) for details.

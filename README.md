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
| GET "Hello World!" | 485,127 | 417,739 | 386,738 | 153,313 | 214,450 | 25,946 |
| GET with path parameters, JSON answer | 383,279 | 286,000 | 3,098 ¹ | 134,998 | 196,137 | 24,334 |
| POST a JSON body, echoed as JSON | 290,347 | 268,435 | 3,118 ¹ | 123,758 | 134,752 | 17,056 |
| GET on a new connection per request | 118,297 | 122,774 | 107,879 | 68,935 | 16,366 | 8,532 |

thinger-http answers with a median latency of 259 µs, 329 µs, 434 µs and 114 µs in these four cases (p99 under 470 µs with keep-alive). JSON bodies are decoded within the default [limits](#body-types) (nesting depth, values, UTF-8).

Every route is a handler: nginx runs all of them in njs, rather than answering from its configuration.
¹ With its default settings Crow stalls about 40 ms on each of these responses (Nagle's algorithm and delayed ACKs); its `/hello` is not affected.
cpp-httplib serves one connection per thread, so it gets 128 threads on the same 8 cores, and runs with `TCP_NODELAY`.

To reproduce, see [`benchmark/compare`](benchmark/compare/): `benchmark/compare/run.sh` builds the servers and runs every case.

## Features

- **HTTP Server & Client** - Full HTTP/1.1 support with connection pooling
- **WebSocket** - Server and client support with text/binary messages, or as a byte stream socket for other protocols and tunnels
- **Server-Sent Events (SSE)** - Real-time server push
- **SSL/TLS** - HTTPS and WSS support via OpenSSL, with per-domain certificates (SNI) and Mozilla's "intermediate" settings by default (TLS 1.2 and 1.3)
- **Several Listeners** - One server, and one set of routes, on HTTP, HTTPS and Unix sockets at once
- **Virtual Hosts** - Routes per host name, exact or by pattern, on the same listener
- **Reverse Proxy** - Forward requests to HTTP, HTTPS or Unix socket targets, streaming bodies and tunneling WebSockets
- **File Uploads** - Streaming multipart/form-data parser, with temporary files and limits
- **C++20 Coroutines** - Async-first API with sync wrappers
- **Graceful Shutdown** - Stop listening, finish the requests in flight and tell long-lived connections, until one deadline; connection statistics at no cost per request
- **Request Cancellation** - Each request knows why it is no longer awaited (client gone, timeout, shutdown); routes opt in to have their handlers interrupted
- **Dependencies** - Boost 1.86+ (Asio, iostreams), OpenSSL and zlib, with a C++20 compiler (see [Requirements](#requirements))

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

- C++20 compiler. Tested with GCC 13, Clang 20 and Apple Clang 17 (see [macOS](#macos)), with no compiler flag or fmt macro of your own. GCC 11+, Clang 14+ and MSVC 2022+ have what the code needs, but are not tested
- CMake 3.14+
- Boost 1.86+ (ASIO with coroutines, iostreams)
- ZLIB (HTTP compression via Boost.Iostreams)
- OpenSSL (for HTTPS/WSS)
- spdlog 1.15.1+, unless `THINGER_HTTP_ENABLE_LOGGING` is off. CMake downloads `v1.15.1`, which brings fmt 11.1, when no spdlog is installed or the one installed is older, and linking `thinger::http` gives you its includes and library. A `spdlog::spdlog` target your project already defined stays, whatever its version

### CMake Integration

```cmake
include(FetchContent)
FetchContent_Declare(
    thinger-http
    GIT_REPOSITORY https://github.com/thinger-io/thinger-http.git
    GIT_TAG v0.11.0
)
FetchContent_MakeAvailable(thinger-http)

target_link_libraries(your_target PRIVATE thinger::http)
```

### Versioning

Releases are tagged `vMAJOR.MINOR.PATCH` and follow [Semantic Versioning](https://semver.org/). Pin a tag rather than `main`, which changes often. While the version is 0.x:

- a patch version (0.1.x) only fixes bugs, without changing the API;
- a minor version (0.x.0) may add features and change the API or the behaviour. Every change that can break or alter existing code is listed in [CHANGELOG.md](CHANGELOG.md), under **Changed**, with how to adapt.

From 1.0.0 on, only a major version changes the API in an incompatible way.

[MIGRATING.md](MIGRATING.md) walks through each minor version's breaking changes in migration order, with code before and after: see [from 0.10 to 0.11](MIGRATING.md#from-010-to-011), [from 0.9 to 0.10](MIGRATING.md#from-09-to-010) and [from 0.8 to 0.9](MIGRATING.md#from-08-to-09).

### Build from Source

```bash
git clone https://github.com/thinger-io/thinger-http.git
cd thinger-http
mkdir build && cd build
cmake -DTHINGER_HTTP_BUILD_EXAMPLES=ON ..
cmake --build .
```

### macOS

macOS is a secondary platform: CI does not run on it. The library and its test suite build and pass on arm64 with Apple Clang 17, and Boost and OpenSSL 3 from Homebrew. The tests take about 40 minutes:

```bash
brew install cmake boost openssl@3
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)
cmake --build build -j6
ctest --test-dir build --output-on-failure --timeout 120
```

`OPENSSL_ROOT_DIR` selects OpenSSL 3 when `openssl@4` is installed too: without it CMake may take OpenSSL 4, with which the whole test suite has not been run.

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

Paths are matched as received, before percent-decoding (`%2F` is not a slash), and path parameters are raw: `req["name"]` of `/files/my%20file.png` is `my%20file.png`, so a handler decodes it if it needs to (`serve_static()` decodes its own path, see [Static Files](#static-files)). Trailing slashes and empty segments are significant: `/users` does not match `/users/`, nor `/users/:id/devices` match `/users//devices`. Simple regexes (characters, classes, quantifiers, alternative words) are checked without `std::regex`, which is kept for the rest and matches at most 2048 bytes of text: a longer segment or path does not match such a regex (libstdc++ recurses about once per byte), so the request goes on to the other routes or the not-found answer.

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

- Header lines starting with a space or tab (obsolete line folding), which are never joined to the previous line, and whitespace between a field name and its colon (`Content-Length : 5`).
- `Content-Length` that is not a plain number (`12abc`, `-1`, `+5`, `5 5`, empty, too large), or repeated with different values.
- `Transfer-Encoding` other than exactly `chunked` (e.g. `gzip, chunked`, `chunked, chunked`, or repeated), together with `Content-Length`, or in an HTTP/1.0 request.
- Malformed chunked bodies: chunk sizes that are not hexadecimal, empty or overflowing; missing CRLF after a chunk size or its data; bare LF line endings; chunk extensions or trailer sections that are too long. Chunk extensions and trailer fields are accepted and ignored.
- An HTTP/1.1 request without a `Host` field, or any request with more than one (whatever their values and letter case), as RFC 9110 section 7.2 requires and Node and Go do. An HTTP/1.0 request may omit it, and an empty `Host:` is served by the default [virtual host](#virtual-hosts). [In-memory dispatch](#in-memory-dispatch) does not check it.

The whitespace (spaces and tabs) around a header value is not part of it, as RFC 9112 section 5.1 allows and Node, Go and Beast read it: `Name:value`, `Name:` (an empty value), `Name:\tvalue` and trailing spaces or tabs are accepted, and tabs inside a value are kept. So `Content-Length: 5 ` frames a 5-byte body and `Transfer-Encoding:\tchunked` a chunked one, still checked as above on the value without that whitespace.

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

- A request gives no access to its socket: read the body with `read()`/`read_some()`, or take over the connection (below). `req.connection()` tells its peer and local addresses and whether it is TLS, and `req.tls()` its TLS session.
- `co_await req.read_body()` loads the remaining body into `req.body()`, up to the maximum body size (it fails without reading a larger `Content-Length` body). Do not mix it with `read()`/`read_some()` on the same request: the stored body would miss the part already read (and fail to decompress if the request is compressed).
- `read()`/`read_some()` return 0 at the end of the body, and also if reading it fails: `req.get_body_error()` tells why (`too_large`, `malformed` framing, `incomplete` if the connection ended first, `bad_encoding`, or `timeout` past the read deadline). A request whose body fails is answered with `400` (`413` if too large, `408` past the deadline) unless the handler responded, and its connection is closed.
- `req.set_read_deadline(time_point)` sets an absolute deadline for the reads of the body, pending and later ones, as `http.ResponseController.SetReadDeadline` in Go: the handler moves it as it reads ("60 s more for each 64 MiB received"), which cuts a client that goes silent or trickles without cutting a long upload. As it passes, the read returns what it had, or 0, and `get_body_error()` is `timeout` for good; nothing is answered for the handler. It replaces the request timeout of the connection or route for the rest of the body, and `time_point::max()` removes it, leaving only the idle timeout. It does not bound the response. Without calling it nothing changes, nor does it cost anything; a body read from memory never waits, so it never passes.

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

To write in blocks of a fixed size, `reader.read(max_size)` returns at most `max_size` bytes, without copying, leaving the rest of the piece for the next call, as `Read(p)` in Go's `multipart.Part`; `read()` returns the piece as it came, `read(0)` returns empty at once without advancing, which is not the end of the part. A read past the deadline of `req.set_read_deadline()` fails the reader with `400` (`invalid_body`): check `req.get_body_error()` to answer `408` instead.

- **Limits** (`http::multipart_limits`), checked as the body arrives: `max_parts` (1000), `max_files` (none), `max_header_size` of each part (16 KB), `max_filename_size` (255 bytes), `max_field_size` (1 MB, each part without filename), `max_fields_size` (8 MB, all of them, as they are kept in memory) and `max_file_size` (none). The whole body is bounded by the maximum body size of the route (`413` `Payload Too Large`), 8 MB by default: give upload routes the limit they need. Exceeding any limit fails with `413`. `max_parts` is Go's default, and the header and field sizes busboy's (Multer).
- **Syntax** (RFC 7578, RFC 2046 5.1), as strict as Go's `mime/multipart`: the boundary from the `Content-Type` (quoted or not, 1 to 70 characters of the RFC 2046 set, exactly one), CRLF line endings, spaces and tabs after delimiters, preamble and epilogue ignored, a closing delimiter required (a truncated body is `400`, as one with no parts but its closing delimiter is an empty form). The boundary within the content is a delimiter only at the start of a line and followed by `--`, whitespace or CRLF. Part headers are case-insensitive, without folding, CR, LF or NUL in values; a repeated `Content-Disposition` or `Content-Type`, or a repeated parameter (`name`, `filename`) is ambiguous and malformed. `filename*` (RFC 5987, UTF-8, ISO-8859-1 or US-ASCII) wins over `filename`, as in busboy; quoted strings keep backslashes that escape nothing, so Windows paths sent by old browsers stay paths (as Go reads them). A part without filename is a field; with one, even empty (a file input without a file), a file. Parts without `Content-Disposition: form-data` or without a name are skipped by `read_multipart()` and the decoder, as Go and busboy do; the reader gives them too, and any `multipart/*` type.
- **Filenames**: `filename()` is what the client sent, which can be a path or have control characters. `safe_filename()` is what follows its last `/` or `\`, without control characters, and empty for `.` and `..`: a name to store the file with, though it can still collide with an existing file or be a dotfile.
- **Temporary files** are created with `mkstemp()` (mode 0600) in `multipart_options::temp_directory` (the system one by default), written as the parts arrive (blocking writes, as Go does), and deleted on any error, and with the form. A file that cannot be written fails with `500`.
- **Bodies the server already read** (`.deferred_body(false)`, in-memory dispatch) are parsed from `req.body()`, decompressed by the server if they had `Content-Encoding`; a body still to be read with a `Content-Encoding` other than `identity` is `415`, as it is not decoded while streaming.

Handlers taking the body decoded (`nlohmann::json&`) can also accept `multipart/form-data` with `.accepts({"form", "multipart"})`: the `multipart` [body decoder](#body-types) reads it whole (within the maximum body size and the default limits) into the same object a urlencoded form gives, files included as objects `{"filename": <safe filename>, "content_type": ..., "content": <binary>}`, so one handler and one schema serve both form encodings. It is not accepted unless listed, as Express's `body-parser` does not parse multipart either: routes receiving files should stream them instead.

### Taking Over the Connection

`res.take_over()` hands the connection to your code, for custom protocol upgrades or tunnels (WebSockets and SSE are built on it). The response set so far is sent first (200 by default; unless it is a `101`, a `2xx` to `CONNECT`, or carries a `Content-Length` or `Transfer-Encoding` set with `header()`, it says `Connection: close`, as what the handler writes is its body until the connection closes); then the server stops reading from the connection and calls the handler with an `http::upgraded_connection`: the `socket`, the bytes the client had already sent past the request (`buffered`), and the `scope` of the connection. From then on the handler owns the socket. It can also be called later from a copy of the response: until the response starts, the server handles nothing else the client sends on that connection (it stays readable from the socket, for the handler), but it notices a client that resets the connection meanwhile, over TLS too, and closes it (a client that only closes its sending side has not left, see [Cancellation](#cancellation)). A `take_over()` arriving after that does not call its handler: it is released, and a warning logged.

```cpp
server.get("/tunnel", [](http::request& req, http::response& res) {
    res.status(http::http_response::status::switching_protocols);
    res.header("Upgrade", "my-protocol");
    res.header("Connection", "Upgrade");
    res.take_over([](http::upgraded_connection conn) {
        // conn.buffered holds bytes already received after the request; process them first
        conn.scope.on_shutdown([socket = conn.socket] { socket->close(); });
        // ... run the protocol on conn.socket; once done: conn.scope.closed()
    });
});
```

The scope (`http::server_scope`, in `thinger/http/server/upgrade.hpp`) is the only link between the connection and the server it came from, and does not keep the server alive:

- `on_shutdown(callback)`: called once, on the io_context of the socket, when a [shutdown](#shutdown) starts its drain, so the handler finishes and closes the connection (with a WebSocket, a close 1001). Set after the drain started, it is still called once, posted. It is released before it is called, so it may own the socket. What it throws is logged.
- `set_type(type)`: what the connection serves in the [statistics](#statistics) (`taken_over` by default).
- `closed()`: the handler closed the connection. It no longer counts and is not told of a shutdown. Calling it again does nothing.

The server counts the connection while the socket handed out lives (its copies, and a WebSocket built on it, included), or until `closed()`. Whatever is still open when the shutdown deadline comes is closed. A scope without a server does nothing: a default-constructed one, one whose server was destroyed, or one of a socket that did not come from a server.

The handlers of `take_over()`, `upgrade_websocket()`, `upgrade_websocket_stream()` and `start_sse()` can be coroutines: they run on the io_context of the connection, detached (what they throw is logged); `upgrade_websocket()` starts reading messages once its coroutine completes. So can the middlewares with `next()` and the callbacks of the async client. Callbacks that are called and not awaited (`on_message()`, `on_shutdown()`, the error formatter, the reverse proxy `on_request`/`on_response` hooks, the OpenAPI hooks...) take only synchronous functions: a coroutine lambda there does not compile, instead of being dropped without running; `co_spawn()` the work from them instead.

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
res.header("X-Custom", "value");       // adds a field, even if it is already there (Set-Cookie)
res.set_header("Cache-Control", "no-store");   // replaces every field of that name
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

// Paths without a file go to the not-found handler (see Fallthrough)
server.serve_static("/", "/var/www/app", {.fallthrough = true});
```

`serve_static()` answers `GET` requests, and `HEAD` requests through its `GET` route (so the `GET` routes of the paths it covers, such as an API next to `serve_static("/", ...)`, answer their `HEAD` requests too). The fallback file is served for a directory (its own one), and for the paths without a file (the one of the root directory, as single page applications expect). Paths resolving outside the directory (through symlinks; request targets with a `..` segment in their percent-decoded path, as `/a/../b` or `/%2E%2E/x`, are already rejected with `400`, while `..` elsewhere, as in `/files/v1..v2.txt` or a query, is served) are answered with `403`.

The path under the prefix is percent-decoded before looking for the file, so `/static/my%20file.png` serves `my file.png` and `/static/informe-%C3%B1.pdf` serves `informe-ñ.pdf` (UTF-8 names). `+` is literal (`a+b.txt` is that file, as Node's `send` reads it), a decoded `%2F` separates directories, and a malformed escape or a decoded NUL (`%00`) gets `400`. The decoded path is still checked to resolve inside the directory (`403` otherwise).

Dotfiles (`.env`, `.git/config`, `.htpasswd`) are not served by default, as with Express's `serve-static`: a path is a dotfile path when a segment of its decoded path under the prefix is longer than one character and starts with `.`, other than `..`, which the traversal checks handle (so `/.git/config` and `/%2Eenv` are, `/files/v1..v2.txt` and `/a.b/c.txt` are not). The `dotfiles` option says what to do with them, judging by the path alone, whether or not the file exists:

- `http::dotfiles_policy::ignore` (default): answered as a path without a file, with the root fallback or `404` (with [`fallthrough`](#fallthrough), it passes), so a dotfile cannot be told apart from a missing file.
- `http::dotfiles_policy::deny`: `403`, without the fallback.
- `http::dotfiles_policy::allow`: served as any other file.

A segment that is exactly `.well-known` is always served (RFC 8615, for ACME challenges and the like), though dotfiles under it are still checked. The configured directory itself may have dot segments (`/srv/.cache/www`): only the part under the prefix is checked.

```cpp
server.serve_static("/", "/var/www/app", {.dotfiles = http::dotfiles_policy::deny});
```

Files are served with validators, so that browsers download them once and only check them afterwards:

- A weak `ETag` from the size and modification time of the file (`W/"<size>-<mtime in ns>"`, in hex; the content is not hashed), and `Last-Modified`.
- `Cache-Control: no-cache` by default: caches keep the file, but revalidate it on every use. Set another policy with `cache_control` (e.g. `max-age` for hashed assets).
- Conditional requests are answered with `304 Not Modified`, without body, while the file does not change: `If-None-Match` with `*` or a list of entity tags (weak comparison, `W/` ignored), or else `If-Modified-Since` (ignored when there is an `If-None-Match`, or when it is not a valid date) with a date not earlier than the modification time. The `304` carries the validators and the `Cache-Control`.
- `Accept-Ranges: bytes`: a `GET` with a single byte range (`Range: bytes=0-99`, `bytes=100-`, `bytes=-100`) gets `206 Partial Content`, or `416` if it starts past the end. Several ranges, or an invalid one, get the whole file; so does a range with an `If-Range` other than the `Last-Modified` date (the weak `ETag` never matches it).
- Text files of 200 bytes or more, up to 4 MiB, are compressed for clients accepting gzip or deflate (see [Response Types](#response-types), with `Vary: Accept-Encoding`); ranges are sent as they are. Larger files are sent uncompressed (and without `Vary`), unless they have a precompressed sibling (below): compressing on the fly needs the body whole in memory. A `HEAD` gets the headers of the `GET` with the length of the file, without reading it.
- Bodies over 1 MiB (the file, the range or the sibling sent) are streamed: read and written 64 KiB at a time, each piece once the previous one is written, so a response holds about one piece whatever the size of the file and the pace of the client (a client that stops reading is closed by the [`write_timeout`](#connection-options) of the connection). Smaller bodies, the text files compressed for the client (up to 4 MiB) and the responses of [In-Memory Dispatch](#in-memory-dispatch), which has no connection to stream to, are read whole, as before. nginx and Node's `send` (behind `express.static`) never read a file whole either; 64 KiB is the piece of Node's file streams, and 1 MiB keeps a single read for the files of a usual web application.

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

`res.send_file()` does the same in a handler. A file whose extension is unknown or missing goes as `application/octet-stream`, and a `Content-Type` set before by the handler or a middleware is kept, as in Express. Its `Cache-Control` is `no-cache` unless the handler set one before (`res.header("Cache-Control", ...)`):

```cpp
server.get("/report", [](http::response& res) {
    res.header("Cache-Control", "private, max-age=60");
    res.send_file("/data/report.pdf");   // 304 while it does not change
});
```

#### Fallthrough

By default `serve_static()` answers every path it gets, with the root fallback or its own `404`, so the application has no say on the paths without a file. With `.fallthrough = true`, those paths go to the not-found handler of the host instead, for rules such as an `index.html` under some prefixes only, or a `404` of its own:

```cpp
server.serve_static("/", "/var/www/app", {.fallthrough = true});

// index.html under /console/ only, and a 404 without body elsewhere
server.set_not_found_handler([](http::request& req, http::response& res) {
    auto method = req.get_http_request()->get_method();
    if ((method == http::method::GET || method == http::method::HEAD) &&
        req.get_http_request()->get_path().starts_with("/console/")) {
        res.send_file("/var/www/app/index.html");
        return;
    }
    res.error(http::http_response::status::not_found);
});
```

The not-found handler gets every method without a route, not only the `GET` and `HEAD` of `serve_static()`: hence the check.

- **What passes:** a `GET` or `HEAD` for a path without a file to serve: it does not exist (a name too long to exist included), it is a directory without its fallback file, it is neither a file nor a directory (a FIFO, a socket), or it is a dotfile with the policy `ignore`. Conditional and range requests for it pass as a plain `GET`, without a `304`, `206` or `416`. The response reaches the handler untouched (no validators, `Cache-Control` or `Accept-Ranges`), the handler runs once, the middlewares do not run again, and `get_matched_route()` is null, as in a request without a route. The parameter `path` of the route is still in the request (`req["path"]`), so take the path from `req.get_http_request()->get_path()`, as above.
- **What does not pass:** a path resolving outside the directory (`403`), a dotfile with the policy `deny` (`403`), a malformed escape or a decoded NUL (`400`) and a `..` segment (`400`, from the request parser). A not-found handler that serves an `index.html` or works as a proxy never sees them. A failure of the file system (no permission, the directory does not exist) is still a `500`.
- **The fallback** is only the index of a directory, the root included: `/` and `/docs` serve their `index.html`, and `/missing` passes. Without a fallback (`.fallback = ""`), those directories pass too.
- **Where it goes:** to the not-found handler of the host where `serve_static()` was registered or, if it has none, to the answer that host gives to a `GET` without a route. It does not go to another `serve_static()`: the router chooses one route per request and has no `next()`, so with `"/"` and `"/assets"` on a host, a path without a file under `/assets` is not looked for in the one of `"/"`.
- **Against `fallthrough` in Express's `serve-static`:** it is off by default, where Express has it on, as a single page application relying on the root fallback would stop working. And the `400` and `403` are answered, where Express lets every error under `500` pass. A directory gets its index without a redirect, with or without the option, where `serve-static` redirects `/docs` to `/docs/`.

#### Serving Files from a Handler

`http::file_server::serve()` does what `serve_static()` does from a handler of your own, for what a route of its own needs: another parameter name, or rules of the not-found handler. It is declared, with `http::static_options`, in `<thinger/http/server/file_server/static_files.hpp>`:

```cpp
bool serve(http::request& req, http::response& res, const std::filesystem::path& directory,
           std::string_view path, const http::static_options& options);
```

```cpp
#include <thinger/http/server/file_server/static_files.hpp>

// A route with a parameter of another name; without fallthrough, it always answers
server.get("/files/:rest(.*)", [](http::request& req, http::response& res) {
    http::file_server::serve(req, res, "/var/www/files", req["rest"], {});
});

// A not-found handler: files under /app/, then the rules of the application
server.set_not_found_handler([](http::request& req, http::response& res) {
    auto method = req.get_http_request()->get_method();
    auto path = req.get_http_request()->get_path();
    if ((method == http::method::GET || method == http::method::HEAD) && path.starts_with("/app/")) {
        if (http::file_server::serve(req, res, "/var/www/app", std::string_view(path).substr(5),
                                     {.fallthrough = true})) {
            return;  // answered: a file, a 304, a 403...
        }
        if (path.starts_with("/app/console/")) {
            res.send_file("/var/www/app/index.html");
            return;
        }
    }
    res.error(http::http_response::status::not_found);
});
```

- **The path** is the file relative to `directory`, raw as it comes in the URI and without the query: what `req.get_http_request()->get_path()` gives after your prefix, or a route parameter, which is captured raw. The function percent-decodes it as above, so do not decode it first. Empty, it is the directory itself. Remove the `/` after your prefix too: a path starting with `/` is absolute, not relative to `directory`, and gets `403` outside it. No route parameter is read, so a route does not need one called `path`.
- **What it guarantees:** the same as `serve_static()`, with the same `static_options`: decoding, containment, dotfiles, validators and `304`, byte ranges, precompressed siblings and `Cache-Control`.
- **Whether it answered:** the result, so there is no need to look at the file system. It is `false` only with `fallthrough`, for a path without a file (as listed in [Fallthrough](#fallthrough)), with the response untouched for the caller to answer it. It is `true` otherwise, for a rejected path too (`400`, `403`). Without `fallthrough` it always answers, as `serve_static()` does: a file, the root fallback, or a `404`.
- **What it leaves to the caller:** the method, which it does not look at, so a not-found handler must check it. And a failure of the file system is thrown, which the server answers with `500`.

Over a connection, a malformed escape (`%zz`) in the target is rejected with `400` by the request parser, before any handler. The function rejects it for what does not come from there, such as a request dispatched in memory.

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

Routes, groups and [virtual hosts](#virtual-hosts) can set their own maximum body size, timeout, request deadline and [decode limits](#body-types), instead of the application ones:

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

**Maximum body size** (`.max_body_size(bytes)`, `set_max_body_size()` if unset everywhere, `std::numeric_limits<std::uint64_t>::max()` for none) bounds every way the body is read, with `413 Payload Too Large` answered through the [error formatter](#error-format) and the connection closed:

- Bodies the server reads (handlers taking the body decoded, synchronous handlers, `.deferred_body(false)`): a `Content-Length` over the limit is rejected before reading anything; a chunked body, as soon as it goes past it; a compressed body, also once decompressed.
- Coroutines reading the body themselves (`read_body()`, `read()`, `read_some()`): reading a larger body fails (`req.get_body_error()` is `too_large`, a `Content-Length` one at the first read, without reading it), and the server answers `413` unless the handler did. **Behavior change**: these reads had no limit before; now they are bounded by `set_max_body_size()` (8 MB by default) unless the route (or its group or host) sets its own: give routes receiving files or other large uploads, and hosts forwarded whole, the limit they need.
- The [reverse proxy](#reverse-proxy): the request body it streams is bounded by the route limit and by `proxy_options::max_body_size`, whichever is smaller. The not-found handler of a host (a proxy for a whole host) has no route: the limit of the host applies, or else the application one.
- [In-memory dispatch](#in-memory-dispatch): the same, from the body of the request.
- A body left unread by the handler (e.g. a middleware answered `401` before it was read) is discarded only up to the smaller of `set_max_body_size()` and the route limit; a larger one closes the connection instead of being read, so a route accepting large bodies does not make the server read them for nothing.

**Closing with the body unread**: when the connection closes after a response while the client may still be sending its request (`413`, `417`, a `401` before a body too large to discard, a request timeout, a header section too large...), it closes gracefully, as RFC 9112 (section 9.6) describes. Once the response is written, the server stops sending, so the client sees the end of the connection. It then reads and drops what keeps arriving until the client closes, 8 MiB were dropped, or 5 s passed (by default, see [Connection Options](#connection-options)), whichever comes first, and closes. Closed at once, the unread data would make the system reset the connection, and a client still sending its body would get `EPIPE` or a reset instead of the response. Meanwhile the connection timeout does not apply, and the connection holds no more memory than its read buffer. A body that arrived whole with the head leaves nothing to wait for, and a request that is not HTTP (`400`) closes at once, as Go does. A [graceful shutdown](#shutdown) closes the connections lingering at once (their responses were written whole), and the responses ending during it linger 0.5 s at most. A body within the limit that the handler left unread is still read and dropped to keep the connection alive, as Node does (Node reads any body to its end); a larger one is read only so the client gets its answer, as Go does.

```cpp
// Up to 1 MiB or 2 s; 0 for either closes at once. For this server only (see Connection Options)
http::connection_options options;
options.linger_max_bytes = 1024 * 1024;
options.linger_timeout = std::chrono::seconds(2);
server.set_connection_options(options);   // before listen()
```

**Timeout** (`.timeout(duration, status = 504)`, none by default) is the maximum time from the dispatch of a request (its headers received and its route matched) until its response **starts**, middlewares and body reading included. If no answer has started by then, the request is answered with `504 Gateway Timeout` (or the status given, e.g. `503 Service Unavailable`) through the [error formatter](#error-format), with the message `No response within the timeout` and the headers set on the response so far (e.g. CORS headers of a middleware). As for any answer, set the status and headers (`res.status()`, `res.header()`) from the thread running the handler, before answering: a handler with a timeout that sets headers later, from another thread, would race with the timeout copying them.

- **Handlers are not interrupted unless their route says so**: after the `504`, the [cancellation scope](#cancellation) of the request is cancelled with `cancel_reason::timeout`. A coroutine goes on until it ends, finding the reason in `req.cancelled()`, and anything it sends afterwards (or a copy of the response, from another thread) is dropped: `has_responded()` is `true` for every copy, and late answers are logged at debug level only. A route declaring `.cancel_on({http::cancel_reason::timeout})` has its handler interrupted instead, at its next cancellable await. Keep the other handlers bounded on their own (their own timeouts on what they wait for, or `req.cancellation_slot()` bound to them).
- **Started responses are never cut**: once the response starts (`send()`, `json()`, `start_stream()`, `start_chunked()`, `start_sse()`, WebSocket upgrades, `take_over()`), the timer is cancelled and the response takes as long as it needs.
- **The connection**: if the body was read whole, the connection stays alive and serves the next request right away, while the handler may still run. If part of the body is still unread (the handler may be reading it), the response closes the connection, as nothing else can tell where the body ends; the handler then sees the body incomplete.
- **Connection timeout**: while a request waits for its response within its own timeout, the connection timeout (`connection_options::idle_timeout`, 120 s) does not close the connection, so a route can wait longer than it; it applies again to the idle connection afterwards, and uploads extend it as before.
- **Not-found handlers** get the timeout of their virtual host, if it sets one.
- **In-memory dispatch** applies the route timeout too; `dispatch_options::timeout` still bounds the whole call, whichever expires first, and cancels the scope of the request with `timeout` too.

**Request deadline** (`.request_timeout(duration)`, else `connection_options::request_timeout`, 300 s by default, see [Connection Options](#connection-options)) is the maximum time to **receive** a request, from its start (its first byte, or the start of serving the connection for its first request) until its body was read to its end, whatever the pace of its bytes: a slow upload (slow POST) cannot hold a connection and its handler for ever. A route receiving large uploads raises it, or turns it off with `0ms`, as it sets its own maximum body size:

```cpp
// Firmware images over slow links: up to 30 minutes to arrive
server.post("/v1/firmware", upload_firmware).max_body_size(256 * 1024 * 1024).request_timeout(30min);

// The routes of a group, and a whole host forwarded by a reverse proxy, without one
auto media = server.group("/v1/media").request_timeout(0ms);
media.put("/:file", store_file);
server.host("files.example.com").request_timeout(0ms).set_not_found_handler(forward);
```

- **What it answers**: the [cancellation scope](#cancellation) of the request is cancelled with `cancel_reason::timeout` (a route declaring `.cancel_on({http::cancel_reason::timeout})` has its handler interrupted), a pending body read fails (`read_body()` returns `false`), and the request is answered `408 Request Timeout` with `Connection: close`; the connection then closes gracefully, as the body may still be arriving (see Closing with the body unread above), as Node does. What the handler reads or writes afterwards fails on the closed connection. If its response already started (a handler, or the [reverse proxy](#reverse-proxy), streaming it while the upload goes on), no `408` can be sent: the connection is closed, ending the response. It is also closed, without a `408`, when an earlier response on the connection is still being written (pipelined requests): a write in progress is never cut short to make room for it.
- **What it bounds**: only the reception of the request. A request without a body is received with its head, so it never applies to it, nor to a body that arrived whole: what bounds the response is the timeout above. The time a client waits for its `100 Continue` (`Expect: 100-continue`, sent when the handler first reads the body) counts towards it, as in Node.
- **From the start of the request**: a route deadline is applied once the route matched, measured from the start of the request, so one lower than the time already spent expires at once. A route `.timeout()` longer than it does not delay it.
- **Precedence** as for the other limits: route > group > host (its not-found handler too) > `connection_options::request_timeout`. Its head is received before the route is known, so the head of a request is bounded by the connection option and `header_timeout` only. The deadlines of a connection do not apply to an [in-memory dispatch](#in-memory-dispatch), nor to a connection once taken over (WebSocket, SSE, `take_over()`).

The [OpenAPI](#openapi) document shows the limits set on a route (or its group) as `x-max-body-size` (bytes) and `x-timeout-ms` extensions of its operation.

### Cancellation

Each request has a cancellation scope, which the server cancels once, for the first reason that comes (`http::cancel_reason`):

- `client_gone`: the client reset the connection, a read or write on it failed (a reset or a broken pipe as the response is written), or the connection ended in the middle of the request body. A client that only closes its sending side after a complete request (a TCP FIN, as simple HTTP stacks do; over TLS a `close_notify`, or a FIN without one) has **not** left: its response is written, with `Connection: close`, and the connection closes after it. So a client that closes cleanly while its handler runs is noticed later, at the first write of its response or at the route timeout: a long poll declares `timeout` too.
- `timeout`: the [timeout of the route](#limits-and-timeouts-per-route) expired, right after its `504`, or that of an [in-memory dispatch](#in-memory-dispatch).
- `shutdown`: the [shutdown](#shutdown) reached its deadline with the request in progress, the server was [stopped or destroyed](#server-lifecycle), or the application was destroyed with an [in-memory dispatch](#in-memory-dispatch) in flight.

Cancellation is cooperative. The server interrupts a handler only on a route that declares the reason with `.cancel_on(reasons)`, and at the shutdown deadline, on `stop()` and on its destruction, every handler of a request it still follows (see below). On any other route the handler goes on, and looks at the scope where it chooses:

```cpp
using http::cancel_reason;

// A long poll: interrupted once its client is gone or its timeout expired
server.get("/v1/events", [&events](http::request& req, http::response& res) -> thinger::awaitable<void> {
    auto event = co_await events.next();   // throws operation_aborted when interrupted
    res.json(event);
}).timeout(60s).cancel_on({cancel_reason::client_gone, cancel_reason::timeout});

// A default route: it runs to its end, abandoning only the wait it binds to the scope
server.get("/v1/report", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 5s);
    co_await timer.async_wait(boost::asio::bind_cancellation_slot(req.cancellation_slot(), thinger::use_nothrow_awaitable));
    if (req.cancelled() != cancel_reason::none) co_return;   // nobody waits for the report
    res.json({{"status", "ready"}});
}).timeout(10s);
```

- `req.cancelled()` returns the reason, `none` until the scope is cancelled, from any thread.
- `co_await req.client_gone()` completes once the scope is cancelled, whatever the reason, and returns it (with `||`, alongside other work); `none` if the wait itself is cancelled. It never completes for a scope never cancelled.
- `req.cancellation_slot()` is cancelled (terminal) with the scope while the handler runs, and once more by a later `shutdown`: bind it with `boost::asio::bind_cancellation_slot` to the awaits the handler can abandon, one pending await at a time (asio's rule).
- On a `cancel_on` route, asio cancels the handler coroutine: its pending await, or its next one, ends with `operation_aborted` (thrown, or returned with `as_tuple`), and so do those of the coroutines it awaits. Code that does not await is not interrupted. An interrupted handler is not a failure: no `500` (the timeout already answered, the client is gone, or the connection closes), its exception logged at debug level; a response it had started closes the connection after it. `cancel_on()` is set on each route (groups and hosts do not carry it), and `cancel_on({})` keeps the default.

Work that must not be interrupted on such a route (a write and what follows it) runs outside the cancellation state of the handler, either for the rest of the handler or for that work only:

```cpp
// The rest of the handler: cancellation disabled (without a filter, reset_cancellation_state()
// lets the terminal cancellation of the scope through)
co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
co_await store.save(record);

// Only the work: a coroutine of its own, spawned with no slot, which the handler awaits to its
// end (its next await is interrupted then)
co_await co_spawn(co_await boost::asio::this_coro::executor, store.save(record),
                  boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, thinger::use_awaitable));
```

The scope is cancelled once: a handler that disabled its cancellation checks `req.cancelled()` afterwards. A `shutdown` still interrupts a handler that an earlier reason let go on, once, and `req.cancelled()` keeps the first reason. The scope is cancelled on the thread of the connection (the strand of an in-memory dispatch), where the handler runs. An in-memory dispatch follows the same rules: its timeout is `timeout`, a caller cancelling it is `client_gone`, and destroying the application is `shutdown`. A connection follows a request until its response is written: a handler still running after that, such as one going on after the `504` of its route timeout was written, is not interrupted by the server stopping, and runs on against what it shares with the server. A dispatch has no such limit: destroying the application reaches its handler until it ends. The [reverse proxy](#reverse-proxy) stops the exchange with its target when the scope is cancelled.

This is Go's model, where the context of the request is cancelled and the handler decides when to look; Node does not cancel handlers.

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

The server does not need to be listening. Chunked responses are collected whole. A `HEAD` request gets the response it gets on a connection: no content, with the `Content-Length` of the body of `GET`. Handlers can keep a copy of the response and answer later. The timeout covers the whole request, handler included: if it expires the result is `504`, and the [cancellation scope](#cancellation) of the request is cancelled with `cancel_reason::timeout`; the handler goes on unless its route declared `.cancel_on({http::cancel_reason::timeout})`, which interrupts it (its pending awaits end with `operation_aborted`). A caller cancelling the coroutine awaiting `dispatch()` (a cancellation slot, or `||` with a timer) cancels the scope with `cancel_reason::client_gone`, which interrupts only a route declaring it, and `dispatch()` throws `operation_aborted`. WebSockets, SSE and `take_over()` answer `501`, as there is no connection. The request is served by the [virtual host](#virtual-hosts) of its URL (or `Host` header). `remote_ip` is the peer address: `get_request_ip()` returns it (or the forwarded client address, if it is a [trusted proxy](#client-ip-behind-proxies)), and it is empty unless set: internal calls never pass as a trusted local address by default. `context` holds the initial values of the [request context](#request-context) (empty by default): middlewares may replace them, and nothing is carried over from a request the caller is serving, so a gateway decides what to pass (or authenticates the call again). Copying `dispatch_options` copies its values; a value of a type that cannot be copied (e.g. `std::unique_ptr`) makes that copy throw `std::logic_error`, so move such options into `dispatch()`.

Destroying the application (or the server, which is one) ends its dispatches in flight without waiting for them, and may be done from any thread or from one of their handlers:

- A dispatch without a complete response, one streaming in parts included, ends at once instead of at its timeout: `co_await dispatch()` throws `boost::system::system_error` with `operation_aborted`, the same as a caller cancelling it and the transport error a client sees from Node's `closeAllConnections()` or Go's `Server.Close()`. The variant with a callback has no error to report, so its callback is called once with `503 Service Unavailable` (RFC 9110 section 15.6.4). A response already complete is delivered, even if the caller has not resumed yet. The caller gets its result when its executor runs, which the destructor does not need.
- A dispatch not started yet (a callback queued on its executor, an awaitable still to be awaited) ends the same, without running any middleware or handler and without reaching [`on_response()`](#response-hook). Nor does a later step start: a request whose middleware was suspended never reaches its handler.
- A handler running is interrupted with `cancel_reason::shutdown` as on [`stop()`](#server-lifecycle), whatever its route declared: its next cancellable await ends with `operation_aborted`. That includes a handler that went on after the `504` of the dispatch timeout: `req.cancelled()` keeps saying `timeout`, the first reason. Its current step, and the awaits it isolated from cancellation (see [Cancellation](#cancellation)), end on their own; what it sends afterwards, from a copy of the response too, is dropped; and `on_response()` is called once, when it ends.

Once the destructor returned, the library touches nothing of the application: a dispatch and its handler share only what it serves with (routes, virtual hosts, middlewares, hooks and settings), which stays alive until they end. What the handler captured itself is the application's to keep alive, as on a server. Calling `dispatch()` on an application already destroyed is an error, like calling any other member.

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

Hosts are matched in order: exact names, then patterns in registration order, then the default host (`server.host("*")` returns the server itself). Register them before `listen()`. A name longer than any DNS name (253 bytes, without the port and one trailing dot) is never matched against the patterns, whose regular expressions would run on whatever a client sends: it is served by the default host, as any name no virtual host matches (exact names are still found). Once the host is chosen, everything works as for a single host: routes and `404`/`405` come from that host only, and the global middlewares (`server.use()`) run for every host after route matching, with `req.get_matched_route()` set to the route of the matched host and `req.get_virtual_host()` to the host. `server.openapi()` documents the default host; for another one build a generator over its router: `http::openapi_generator(server.host("api.example.com").router(), &server.get_body_settings())` (the body settings document the [body types](#body-types) of the application).

For HTTPS, the certificate is chosen per domain before any request is read, through SNI: register one per host name (wildcards such as `*.devices.example.com` included) in the [certificate manager](#https-server) of the server, with `manager->set_certificate(hostname, certificate, private_key)`. Virtual hosts then pick the routes for the same name from the `Host` header. A request whose `Host` would get another client certificate mode or certificate entry than its SNI name is answered `421 Misdirected Request` before its host runs anything, so a [client certificate mode per host](#client-certificates) protects its virtual host; the same name, names sharing one configuration (a wildcard, the default certificate) and clients without SNI on the listener's mode are served. See `examples/http_server/virtual_hosts_example.cpp`.

### Error Format

Errors sent with `res.error(status, message)`, and the ones the server generates itself (`400` for bodies that cannot be decoded, schema validation and invalid request framing or bodies, `401` of basic auth, `404`/`405`, `413`, `415` for body types not accepted, `500` from middlewares, handlers and invalid route schemas, `504` (or the status set) of [route timeouts](#limits-and-timeouts-per-route), and `501`/`503`/`504` of in-memory dispatch) go through an error formatter. By default the message is sent as `text/plain`, and errors with details (schema validation) as JSON `{"error": {"message": ..., "context": [...]}}`. A formatter changes it for all of them:

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

### Response Hook

`on_response()` is called once per request with its response, for access logs, metrics or audit records: the status, the bytes written (head included), the time since the request arrived, and how it ended. Every request the application receives is reported, whoever answered it: the handler, a middleware, or the server itself (`400` for invalid framing or bodies, `404`, `405`, `413`, `415`, `417`, `500`, route timeouts...), on a connection or [dispatched in memory](#in-memory-dispatch) (where the bytes are the size of the body).

```cpp
server.on_response([](const http::request& req, const http::response_info& info) {
    // on the thread of the connection: hand anything slow to another thread
    access_log.write(req.get_request_ip(), req.get_http_request()->get_path(), static_cast<int>(info.status),
                     info.bytes, std::chrono::duration_cast<std::chrono::microseconds>(info.duration).count());
});
```

- It is called once both the response ended and the application is done with the request (its handler returned, or its route timed out), so the hook sees what the middlewares and the handler stored in the [request context](#request-context).
- `info.outcome` is `completed`, `aborted` (the connection closed or failed before the response was written, `abort_stream()`, a handler failing once its response started, or nothing answered; `status` is `0` then), or `taken_over`.
- Streamed responses (`start_chunked()`, `start_stream()`, with `info.streamed` set) are reported once their end is written. WebSocket upgrades, event streams and `take_over()` are reported as `taken_over` once the response handing the connection over is written: what happens on the connection afterwards is not, see `on_close()` of [`websocket_connection`](#websocket-server) and [`sse_connection`](#sse-server).
- Requests a connection cannot parse (`400`, `431`) never reach the application, and are not reported.
- What the hook throws is logged. Without a hook, nothing is allocated or measured per request. Set it before `listen()`.

### Client IP Behind Proxies

`req.get_request_ip()` is the address of the peer (`req.get_peer_ip()`) unless the peer is a trusted proxy. Then the client address is taken from the forwarding header: it is walked from right to left, skipping trusted proxies, and the first untrusted address is returned (the leftmost one if all of them are trusted).

```cpp
// IPs and CIDR ranges, IPv4 and IPv6; X-Forwarded-For by default
server.set_trusted_proxies({"10.0.0.0/8", "127.0.0.1", "fd00::/8"});

// Or the standard Forwarded header (RFC 7239): for=192.0.2.60;proto=https
server.set_trusted_proxies({"10.0.0.1"}, http::forwarded_header::forwarded);
```

Set them before `listen()` (they are read by every request, without locks). No proxy is trusted by default, and the header of any other peer is ignored, so a client cannot change its address by sending it: code that trusts a local address (e.g. `127.0.0.1` to skip checks) stays safe. List only proxies that append to (or overwrite) the header, and only the header they set: the other one reaches the server as the client sent it.

`req.get_request_scheme()` is `"http"` or `"https"`: the scheme of the connection (for [in-memory dispatch](#in-memory-dispatch), the one of the request URL), unless the peer is a trusted proxy. Then it is the one the proxies forwarded for that client, found in the same walk: the `proto` of the `Forwarded` element where the walk above stops; with `X-Forwarded-For`, the `X-Forwarded-Proto` value in the same position when both headers have as many values (proxies appending to both), else its last value, written by the nearest proxy (one overwriting it). Either way what a client sends ahead of a trusted proxy is never read. Any value other than `http` or `https` is ignored, and the scheme of the connection is used, as when the header is missing.

```cpp
// e.g. to build absolute URLs, or to tell whether a cookie may be Secure
bool secure = req.get_request_scheme() == "https";
```

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
- **Tunnel idle timeout**: `tunnel_idle_timeout` (120 s, `0` disables it) closes both connections of a tunnel, to the client and to the target, once nothing was forwarded in either direction for that long. Any byte forwarded either way is activity, so WebSocket data, pings and pongs keep a tunnel open, also with traffic in one direction only; a byte counts once written to the other side, so a peer that stops reading ends the tunnel too, as do the bytes sent right after the handshake when their peer never reads them. A tunnel ended mid-frame is not closed with a WebSocket close frame: both connections close, as nginx does at `proxy_read_timeout`, and the client sees `1006`. It is separate from `idle_timeout`, which covers the exchange only: setting either to 0 leaves the other on. Like the other options, it can be set per `forward()` call. 120 s sits above the 60 s of nginx's `proxy_read_timeout` and AWS ALB and the 100 s of Cloudflare because a thinger-http WebSocket pings a quiet peer only after 60 s: a shorter deadline would cut a quiet WebSocket before its keepalive sends anything (a `static_assert` keeps the default above that ping interval). It equals the WebSocket and connection idle timeouts (120 s), so a tunnel never ends before its endpoints would notice a dead peer themselves.
- **Errors**, through the [error formatter](#error-format): `502` if the target cannot be reached (connection refused, TLS verification) or its response is invalid, `504` if it times out. `connect_timeout` (10 s) covers the connection and TLS handshake, `idle_timeout` (60 s, `0` disables it, e.g. for event streams) the time without data in either direction during the exchange (a tunnel has its own, see above), and `total_timeout` (none by default) the whole exchange. Once the response has started, a failure closes the connection with the client without ending the body, so it sees it incomplete.
- **A client gone**: the exchange stops, and its connection to the target closes, once the [cancellation scope](#cancellation) of the request is cancelled: the client reset the connection (within milliseconds, also while the target is silent, as an event stream may be), the route timed out, or the shutdown reached its deadline. The proxy never reads the connection of the client for it, and a client closing only its sending side after its request gets the response of the target.
- **Hooks**: `on_request` changes the request for the target (method, path, headers), `on_response` the status and headers of its response (also of a `101`), before the body is relayed: they get the incoming request too, and can edit, remove or add any header, repeated ones included (each `Set-Cookie` is a header of its own in `get_headers()`). Headers they add with invalid characters fail the request (`500`) instead of being sent.

The proxy never changes the end-to-end headers of the target by itself: frame protections, cookies and redirects reach the client as the target sent them. Rewriting them is an application policy, expressed with `on_response`, and each rule has a cost. `examples/http_server/device_proxy_example.cpp` (with `device_proxy_policy.hpp`) serves device web interfaces inside a console:

- **Frames**: removing `X-Frame-Options` and `Content-Security-Policy` lets any site embed the pages (clickjacking). The recommended variant replaces them with `frame-ancestors <console origin>`, so only the console can embed them.
- **Cookies**: dropping `Domain` keeps them on the proxy host. Forcing `SameSite=None; Secure` sends them on requests from other sites too, widening the exposure to CSRF: only for a console on another site.
- **Redirects**: a `Location` pointing to the device (its host, or an absolute path) is rewritten under the proxy path, parsing the URL (scheme, userinfo, host) instead of matching substrings; other hosts are left alone.

Framing is never forwarded as received, to avoid request smuggling between the proxy and the target. The request body is framed again from what the strict server parser accepted (requests with ambiguous framing are rejected before reaching the handler): its `Content-Length`, or chunked again without extensions or trailers, and never past its end, so a pipelined request is never sent as part of it. The response of the target is parsed strictly too, by the parser of the [HTTP client](#response-parsing): a status line other than `HTTP/1.x` with a three-digit code, obsolete line folding, `Transfer-Encoding` other than a single `chunked`, `Content-Length` together with it or invalid (or repeated with different values) are answered with `502`; its body is decoded and framed again for the client (chunked, or ended by closing the connection for HTTP/1.0 clients), and anything the target sends after its end is dropped. Each request uses a new connection to the target, closed after the response; reusing them is left to a connection pool.

The flow control comes from `res.start_stream(head, content_length)`, `co_await res.write_stream(data)` and `co_await res.end_stream()`, available to any handler: each write completes once the data is written to the connection, the framing is chosen by the response (`Content-Length` if given, chunked, or closing the connection for HTTP/1.0 clients), and a body that does not match its `Content-Length` closes the connection instead of desynchronizing it. See `examples/http_server/reverse_proxy_example.cpp`.

### CORS

CORS is off by default: no response has `Access-Control-*` headers and an `OPTIONS` is routed as any other method. `enable_cors()` turns it on for the whole application (every virtual host, on every listener, and `dispatch()`), with the defaults of `cors()` in Express:

```cpp
server.enable_cors();    // any origin, without credentials
server.disable_cors();   // back to no CORS headers
```

With credentials (cookies or `Authorization` in a browser), list the origins allowed. Each is compared with the `Origin` of the request, and the one that matches is reflected:

```cpp
if (auto ec = server.enable_cors({
        .origins = {"https://console.example.com", "https://admin.example.com"},
        .allowed_headers = std::vector<std::string>{"Content-Type", "Authorization"},
        .exposed_headers = {"X-Total-Count"},
        .max_age = std::chrono::minutes(10),
        .credentials = true,
    })) {
    // ec is invalid_argument: the options cannot be applied (the reason is logged)
}
```

Or decide with a function, which replaces the list:

```cpp
server.enable_cors({
    .origin_filter = [](std::string_view origin, const http::http_request&) {
        return origin.ends_with(".example.com");
    },
    .credentials = true,
});
```

| `http::cors_options` | Default | Meaning |
|---|---|---|
| `origins` | `{"*"}` | `{"*"}` alone is any origin. Otherwise the exact origins allowed, ASCII case-insensitive (empty for none, `"null"` for that origin) |
| `origin_filter` | none | `bool(std::string_view origin, const http_request&)`: whether to allow an origin, instead of the list. Called once for each request with an `Origin`, before the middlewares and on the thread of the request, so it must not block. If it throws, the request is answered `500`, with no origin allowed |
| `methods` | `GET, HEAD, PUT, PATCH, POST, DELETE` | `Access-Control-Allow-Methods` of a preflight (not sent if empty) |
| `allowed_headers` | no value | `Access-Control-Allow-Headers` of a preflight. Without a value, what it asks for in `Access-Control-Request-Headers`; with a value, that list (not sent if empty). Being optional, it is written `std::vector<std::string>{...}` |
| `exposed_headers` | empty | `Access-Control-Expose-Headers` |
| `max_age` | no value | `Access-Control-Max-Age` of a preflight, in seconds: not sent without a value, sent when `0` |
| `credentials` | `false` | `Access-Control-Allow-Credentials: true` |
| `preflight_continue` | `false` | Let a preflight go on as any request (see below) |

`enable_cors()` returns a `boost::system::error_code`, empty when applied. It is `invalid_argument`, with the reason in the log and the CORS in force unchanged, for `credentials` with any origin (`*`, which browsers reject: use a list or a filter), `*` listed with other origins, an origin with control characters, a method or header name that is not a token, or a negative `max_age`. As the [other settings](#server-lifecycle), set it before serving.

**Responses.** The headers go on every response of the application, whoever answers it: a handler in any form (files, chunked and streams too), a middleware, or the library itself (`400`, `404`, `405`, `413`, `415`, `500`, a route timeout), so that a browser can read the errors too. A response to a request that is not a preflight has only these:

- `Access-Control-Allow-Origin`: with any origin `*`, also to a request without `Origin`; with a list or a filter, the origin of the request as it came, if allowed. With a list or a filter every response also says `Vary: Origin`, allowed or not and with or without `Origin`, merged with the other fields of `Vary`, those of a head given to `start_stream()` or sent by the target of a `reverse_proxy` too.
- `Access-Control-Allow-Credentials: true` with `credentials`, and `Access-Control-Expose-Headers` with `exposed_headers`.

An origin not allowed gets none of them. CORS is not access control: its request still runs and only its browser cannot read the response, so authorize requests in a middleware. An `Access-Control-*` header set by a handler or a middleware stays instead of the one of CORS, never next to it. `Access-Control-Allow-Origin: *` never goes with `Access-Control-Allow-Credentials` from the library.

**Preflight.** An `OPTIONS` with a non-empty `Access-Control-Request-Method` is a preflight (any other `OPTIONS` is routed as usual). The library answers it with `204` and no body, on any path and host, before route matching: no middleware or handler runs, because a browser sends a preflight without credentials and an authorization middleware would reject it. `on_response` is called for it as for any request. Besides the headers above, an origin allowed gets `Access-Control-Allow-Methods`, `Access-Control-Allow-Headers` and `Access-Control-Max-Age` (when reflecting the requested headers, `Vary` lists `Access-Control-Request-Headers` too). An origin not allowed gets `204` with `Vary` only, and its browser blocks the request. The method and headers asked for are not checked against the lists: the browser does that with the lists it receives. CORS adds no route, so nothing of it appears in the [OpenAPI document](#openapi).

To handle preflights in the application (to log them, limit them, or answer them with its own `OPTIONS` route and its metadata), set `preflight_continue`. They then follow the normal path, route matching and middlewares included, and end in that route or in `404`/`405`/the not-found handler, with the preflight headers already on the response:

```cpp
server.enable_cors({.preflight_continue = true});

// A middleware sees this route as the matched one, with its metadata
server.options("/api/devices", [](http::response& res) {
    res.status(http::http_response::status::no_content);
    res.send("");
}).meta("permission", "Device:Preflight");
```

See `examples/http_server/server_cors_example.cpp`.

### HTTPS Server

```cpp
thinger::http::server server;

server.get("/", handler);

// The certificates of TLS: a manager the application creates and owns
auto manager = std::make_shared<thinger::asio::certificate_manager>();
if (auto ec = manager->set_default_certificate(certificate, private_key)) {
    // ec is invalid_argument: the certificate or its key cannot be used (the reason is logged)
}
manager->set_certificate("api.example.com", api_certificate, api_private_key);  // by SNI

// A TLS listener: the manager goes with its options
thinger::asio::listener_options options;
options.endpoint = thinger::asio::tcp_endpoint{"0.0.0.0", 443};
options.tls = manager;
if (auto ec = server.listen(options)) {
    // invalid_argument (e.g. the manager has no default certificate), or the error binding
}
server.wait();
```

The same server may listen on plain HTTP too, with another `listen()`: see [Listeners](#listeners).

The certificates are kept by an `asio::certificate_manager` (`thinger/asio/ssl/certificate_manager.hpp`): a default one (`set_default_certificate()`, a self-signed one for development if none is set; `remove_default_certificate()` drops it, and no self-signed one is generated then; a null context is refused with `invalid_argument` and keeps the current one, it does not drop it) and one per host name, selected by SNI (`set_certificate(host, certificate, key)`, `*.example.com` for a wildcard). The application creates as many as it needs, always with `std::make_shared` (the SNI callback of its contexts reaches it through a `weak_ptr`), and each one keeps its own certificates, [TLS settings](#tls-settings) and [client certificate modes](#client-certificates): two servers with different managers serve different certificates for the same host name. A TLS listener listens with the default certificate of its manager, taken when it opens; replacing it later applies to the next handshakes. A manager without a default certificate (removed) makes `listen()` return `invalid_argument`, with an error logged. Each listener holds its manager until it stops, so a manager created for `listener_options::tls` and kept nowhere else lives as long as the listeners using it. A context the server listens with may outlive its manager (the application keeps it elsewhere, or a connection still uses it once the listener stopped): once the manager is gone, its handshakes stay on that context (its certificate, whatever the host name), without SNI.

Host names are matched exactly, or by a wildcard `*.domain` for one more label (`*.example.com` matches `a.example.com`, not `example.com` nor `a.b.example.com`), case-insensitive: an exact name wins over a wildcard, and a lookup costs the same however many names are registered. `set_certificate()` takes a host name (labels of 1 to 63 letters, digits, hyphens or underscores, 253 characters at most) or `*.` followed by one, and refuses any other name with `invalid_argument`: no name is taken as a pattern. `get_registered_hosts()` lists the names lowercase, as registered, and `remove_certificate(name)` removes the certificate registered with that name (a wildcard by its `*.domain`), returning `false` if there is none.

The setters return a `boost::system::error_code`: empty on success, so `if (ec)` means that it failed, and `invalid_argument` when what they are given cannot be used (an invalid PEM, a key that is not the certificate's, a name that is not a host name, options OpenSSL refuses), with the reason logged; nothing changes then.

#### TLS Settings

The contexts the manager creates follow Mozilla's ["intermediate"](https://wiki.mozilla.org/Security/Server_Side_TLS) configuration, as Node and nginx do by default: TLS 1.2 and 1.3, ECDHE key exchange (forward secrecy) with AES-GCM or ChaCha20-Poly1305, and the cipher the client prefers. `set_tls_options()` changes them, for the contexts created afterwards (set it before the certificates and before listening):

```cpp
thinger::asio::tls_options options;                         // the defaults
options.min_version = thinger::asio::tls_version::tls1_3;  // TLS 1.3 only
options.prefer_server_ciphers = true;                       // in the order of `ciphers`
// options.ciphers (TLS 1.2), options.ciphersuites (TLS 1.3), options.groups (curves),
// options.security_level (OpenSSL)
if (auto ec = manager->set_tls_options(options)) {
    // OpenSSL refuses them: the current options are kept
}
```

The certificate of each host gets the same options, and SNI cannot weaken them: the versions, ciphers and security level of a handshake are those of the context the server listens with (the default one when it started listening), and the context SNI selects gives only its certificate (OpenSSL would take its ciphers and security level too: the manager keeps those of the listener). A context given ready-made (`set_default_certificate(context)`, `set_certificate(host, context)`) keeps its own settings; `asio::apply_tls_options(context, options)` sets them on it. The setters of earlier versions, `set_server_ciphers()` and `enable_legacy_protocols(true)` (which still allows TLS 1.0 and 1.1 for old clients), are deprecated: they change these options, validated as `set_tls_options()` does.

### Listeners

A server (`http::server`, `http::pool_server`, both deriving from `http::server_base`) serves its application on as many listeners as it opens: plain TCP, TLS and Unix domain sockets, each with a `listen()` of its own. Every listener serves the same routes, virtual hosts, middlewares and hooks: one server answers HTTP and HTTPS, and a route added after `listen()` answers on all of them (added before requests arrive, see [Server Lifecycle](#server-lifecycle)).

```cpp
thinger::http::server server;
server.get("/", handler);

if (auto ec = server.listen("0.0.0.0", 80)) { /* plain TCP; ec says why it cannot */ }

thinger::asio::listener_options https;
https.endpoint = thinger::asio::tcp_endpoint{"0.0.0.0", 443};
https.tls = manager;
if (auto ec = server.listen(https)) { /* ... */ }

thinger::asio::listener_options local;
local.endpoint = thinger::asio::unix_endpoint{"/run/app.sock"};
server.listen(local);

server.get("/health", health);   // answers on the three
// server.listening_on(): {"http://0.0.0.0:80", "https://0.0.0.0:443", "unix:/run/app.sock"}
server.wait();
```

`listen()` returns a `boost::system::error_code`, empty on success (`if (ec)` means it failed): the error of the listener (`invalid_argument` for options that cannot be used, such as a manager without a default certificate, or the error binding), `address_in_use` for an endpoint the server listens on already, or that another `listen()` is opening (the same Unix path, or the same host and port; a port 0 always opens a new one), `shut_down` while a [shutdown](#shutdown) is in progress, or `operation_aborted` when `stop_listening()`, `stop()` or a shutdown, called on another thread, stopped the listener while it opened (e.g. while it waited to bind again). A `listen()` that fails opens nothing and leaves the listeners open as they are. A listener accepts connections only once `listening_on()` lists it, so whatever stops the listeners, on any thread, stops every one: none outlives a `stop_listening()` or a shutdown it raced with. Every option of `listener_options` below applies (client certificates, handshake deadline, session cache, attempts, remotes allowed and forbidden), per listener. `listening_on()` lists where the server listens, one URL per listener in the order they opened, with the port the system chose for a port 0 (IPv6 hosts in brackets), and is empty while it does not listen. `stop()`, `shutdown()` and `is_listening()` act on every listener, and `stats()` counts the connections of all.

The servers accept their connections with an `asio::listener` (`thinger/asio/listener.hpp`), which a protocol of the application (e.g. MQTT) uses on its own: one type for plain TCP, TLS (a `certificate_manager` in `tls`) and Unix domain sockets, set up with a `listener_options` and two `executor_provider`s, the executor to accept on and the next one to serve each connection on (where its handshake runs and the handler is called). In this release both must run on an `io_context` (its executor, or a strand of one): `open()` returns `invalid_argument` otherwise.

```cpp
boost::asio::io_context io;
auto on_io = [&]() -> boost::asio::any_io_executor { return io.get_executor(); };

thinger::asio::listener_options options;
options.endpoint = thinger::asio::tcp_endpoint{"0.0.0.0", 8883};  // or unix_endpoint{"/run/x.sock"}
options.tls = manager;                                             // null: plain connections
// options.tcp_no_delay = true;          // TCP_NODELAY on every connection
// options.max_listening_attempts = -1;  // a busy port is retried every 5 s; 1 fails at once
// options.allowed_remotes, options.forbidden_remotes: addresses closed as they are accepted

thinger::asio::listener listener(options, on_io, on_io);
if (auto ec = listener.open()) {
    // invalid_argument (options that cannot be used), already_open, or the error binding
}
listener.start([](std::shared_ptr<thinger::asio::socket> socket) {
    // a TLS connection arrives once its handshake succeeded
});
// listener.url() is "https://0.0.0.0:8883", listener.local_port() the port bound (0 picks one)
```

`open()` resolves the host (synchronously), binds and listens, and `start()` accepts: a listener is used once, and a second `open()` returns `already_open` (after one that failed, it opens again with the same options). A Unix domain socket replaces a file already at its path and removes it when it stops; TLS is served on TCP endpoints only. The listener holds its manager from its construction until `stop()` (or its destructor), whatever `open()` returns, so the application need not keep a reference to it: its connections start with the default certificate `open()` found, and take the certificate and the client certificate mode of their host name (SNI) from the manager. Without `tls`, `client_verify` is ignored, with a warning logged.

`stop()` closes the listening socket and the connections whose handshake is pending, which never reach the handler. It may be called from any thread, the handler included, and more than once; once it returns, no handler starts and none runs on another thread (it waits for them, so a handler must never wait for a thread calling `stop()`). The destructor stops the listener.

#### Client Certificates

A TLS listener may ask its clients for a certificate (mutual TLS), in one of three modes (`asio::client_verify`): `none` (the default: no certificate is asked for), `optional` (asked for; a client without one goes on) or `required` (the handshake fails without one; in TLS 1.3 the client learns it at its first read, from a `certificate_required` alert). A listener (e.g. one of a protocol such as MQTT) sets it in `listener_options::client_verify`:

```cpp
thinger::asio::listener_options options;
options.endpoint = thinger::asio::tcp_endpoint{"0.0.0.0", 8883};
options.tls = manager;
options.client_verify.mode = thinger::asio::client_verify::optional;
// options.client_verify.trusted_cas = pem;      // validate against these CAs during the handshake
// options.client_verify.max_depth = 4;          // intermediate CAs allowed (with trusted CAs)
// options.client_verify.max_cert_list = 16384;  // bytes of the chain a client may send

thinger::asio::listener listener(options, on_io, on_io);
if (auto ec = listener.open()) { /* invalid_argument: e.g. the trusted CAs cannot be read */ }
```

- **Deferred validation** (no `trusted_cas`, the usual case when each account brings its own CAs): the handshake accepts any well-formed chain, as long as the client proves it holds the key of its certificate, and the application validates the chain afterwards, from `socket->tls()->peer_chain_der` (`req.tls()->peer_chain_der` on an HTTP route, or `ssl_socket::peer_certificate()` and `peer_chain()`, without copies). The library judges neither the issuer nor the dates, key types or algorithms: that policy belongs to the application.
- **Trusted CAs**: OpenSSL validates the chain during the handshake, and a certificate presented that does not validate fails it, in `optional` mode too (a client without a certificate goes on in `optional`). `tls()->verify_result` is `X509_V_OK` then. Trusted CAs that cannot be read make `open()` return `invalid_argument`, and the listener binds nothing rather than serve connections without the validation asked for.
- **Limits**: `max_cert_list` bounds the memory a client uses before it is authenticated (16 KiB by default); `max_depth` the intermediate CAs between the certificate and a trusted CA, as OpenSSL counts them (`SSL_set_verify_depth`: 1 allows one intermediate).

The mode is set on each connection, never on the context the server listens with, which may be the default context of a `certificate_manager`, shared with other listeners (e.g. an HTTPS listener of a server keeps asking for none). A host name may ask for a mode of its own, e.g. the host of the devices on a port shared with a web console:

```cpp
auto manager = std::make_shared<thinger::asio::certificate_manager>();
if (auto ec = manager->set_client_verify("devices.example.com", thinger::asio::client_verify::required)) {
    // not a host name
}
manager->set_client_verify("*.devices.example.com", thinger::asio::client_verify::optional);
manager->remove_client_verify("devices.example.com");   // back to the mode of the listener
```

Host names (SNI) are matched as the certificates (exact, or `*.domain` for one more label), case-insensitive; other hosts keep the mode of their listener, whose trusted CAs and limits apply (deferred validation on a listener that asks for none, as a server's by default). They apply to the next handshakes of the servers already listening.

The precedence, for each handshake:

| Handshake | Mode |
|---|---|
| Names a host (SNI) with a mode set on the manager | the host's, stricter or weaker than the listener's: a `required` listener lets the clients of a host set to `none` in without a certificate |
| Names no host, or one without a mode of its own | the listener's `client_verify` |

The listener sets the mode on each connection (`ssl_socket::set_client_verify()` is internal: only the listener calls it, until 0.10), so two listeners sharing a manager keep their own modes.

The mode of a host protects its [virtual host](#virtual-hosts) too: the handshake chose it by SNI, but the server picks the virtual host of each request by its `Host`, so an HTTPS request whose `Host` would get another TLS configuration than its SNI is answered `421 Misdirected Request` (RFC 9110 15.5.20, as nginx answers), before any middleware, route, upgrade or handler runs, and without reading its body (the connection closes after the response if the body was sent; otherwise it serves the next request). A TLS configuration is the client certificate mode of a name (its own, else the listener's) and its certificate entry (the exact name, the wildcard or the default certificate that serves it, by its registered name: replacing a certificate changes none, and a host name registered with the same certificate as the default is still an entry of its own). The request is served when:

- its `Host` is the SNI name (case-insensitive, port and trailing dot ignored), whatever changed since the handshake;
- its `Host` gets the same mode and the same certificate entry as the SNI name (e.g. two names of one wildcard, or two names of the default certificate);
- the client sent no SNI, and its `Host` gets the mode of the listener (proxies often send none): without SNI only the mode is compared, as the client negotiated the default certificate for any host.

Any other mode, in either direction (e.g. SNI `www.example.com` and `Host: devices.example.com` on the example above, or a `Host` that falls back to a `required` listener from a host set to `none`), or another certificate entry with SNI, gets the 421, logged at debug level with both names (the client decides how often). The check uses the manager of the listener, not the virtual hosts, so it applies to an application without them too; plain TCP, Unix sockets and [in-memory dispatch](#in-memory-dispatch) are never checked. A request without a host name, or with an IP address, gets the default certificate and the listener's mode.

What a connection tells of TLS once its handshake is done, for any socket (`std::nullopt` for a plain one; a WebSocket answers for the socket it runs on, so there is nothing to cast):

```cpp
if (auto tls = socket->tls()) {
    // tls->version ("TLSv1.3"), tls->cipher, tls->server_name (SNI, "" if none),
    // tls->resumed, tls->peer_chain_der (DER, the client's certificate first; empty if it
    // sent none), tls->verify_result (with trusted CAs)
}
```

Sessions are resumed with client certificates: every context of the manager has the same session id context, and a listener asking for certificates one of its own (a digest of its options), so a session is only resumed on a listener with the same policy. A resumed session asks for no certificate: it keeps the one of the session it resumes, but OpenSSL keeps its leaf only, not the rest of the chain, so `peer_chain_der` holds the leaf alone then. A session resumed without a certificate on a host name that requires one fails the handshake. Sessions are resumed with tickets (TLS 1.3, and TLS 1.2 unless the client disables them): the session cache of the server, for resumption by session id, drops the session of a connection closed without a close_notify alert, as a closed `ssl_socket` does.

#### Handshake Deadline

A TLS client has 60 seconds to complete its handshake once accepted, as nginx's `ssl_handshake_timeout` (Node waits 120): otherwise the listener closes the connection, which never reaches the handler, so a client that connects and sends nothing (or half a ClientHello) cannot hold a connection. `listener_options::handshake_timeout` changes it (0 disables it), on a listener of a server too. `stop()` also closes the connections whose handshake is still pending.

`max_pending_handshakes` bounds the handshakes in progress at once on each IO thread (each serve executor), so that a storm of new clients cannot take every thread with the cryptography of their handshakes; 0, the default, is unbounded. A connection whose thread is at the bound waits for a slot before its handshake starts, while accepting goes on, and the wait counts against its deadline: a connection still waiting when it expires is closed then, without reaching the handler.

```cpp
options.handshake_timeout = std::chrono::seconds(10);  // from the connection accepted
options.max_pending_handshakes = 64;                   // per IO thread; 0: no bound
```

Handshakes that fail or time out are logged as warnings and at debug level, not as errors of the server: any client, or scanner, causes them.

#### Session Cache

`listener_options::session_cache` sets the resumption of earlier sessions on the contexts of the listener's manager: enabled by default, with 20,480 sessions in the server's cache (OpenSSL's default) and tickets; disabled, no session is resumed, by session id or by ticket, in TLS 1.2 and 1.3. It applies to every context of the manager, so the listeners sharing a manager must agree: `open()` returns `invalid_argument`, and binds nothing, when the cache of its options differs from the one another open listener of the manager applied (or when it is enabled with a `size` of 0). Once those listeners stopped, a new one may apply another.

```cpp
options.session_cache.enabled = false;   // no resumption
// options.session_cache.size = 20480;   // sessions in the server's cache
```

### Server Lifecycle

```cpp
// Blocking start
server.start("0.0.0.0", 8080);

// Non-blocking: an error_code, empty on success (see Listeners)
if (auto ec = server.listen("0.0.0.0", 8080)) {
    // cannot listen
}
// ... do other work ...
server.wait();  // Wait for shutdown

// Stop server
server.stop();
```

`start()` listens on plain TCP and waits, returning `false` if it cannot listen. `stop()` stops the server right away: it stops listening on every listener, ends a [shutdown](#shutdown) in progress, and closes every connection still open, idle keep-alive ones, those handling a request and those taken over (WebSockets, event streams, tunnels) alike; on `http::server` its io_context stops too. The handlers still running whose response is not written yet are cancelled with `cancel_reason::shutdown`, whatever their route declared (their next cancellable await ends with `operation_aborted`; see [Cancellation](#cancellation)), and none is waited for: each connection closes on its own thread, and what a handler sends afterwards is dropped. Destroying the server does the same. `stop_listening()` only stops listening: new connections are refused, and those open go on being served. For a graceful stop, see [Shutdown](#shutdown).

Once `stop()` or the destructor returned, the library touches nothing of the server: each connection, and each handler running on it, shares what the server serves with (routes, virtual hosts, middlewares, hooks and settings), so a handler still running may finish its current step against them, which stay alive until it ends. What the handler captured itself (the server, objects of the application) is the application's to keep alive, or not to touch once the server is stopped. Both may be called from one of the server's own handlers, on any worker thread, as they do not wait: that handler goes on, and must not touch a server it destroyed. The connections closed are never served again, even if the server listens again.

Configure the server before it serves requests: its [connection options](#connection-options) before `listen()`, and routes, [virtual hosts](#virtual-hosts), middlewares, the [error formatter](#error-format), [trusted proxies](#client-ip-behind-proxies), body size, decoders, types and limits, and CORS before `listen()` too, or after it but before any request arrives (an `http::server` serves nothing until `wait()`). Every request reads them, from any worker thread, without locks (they cost nothing per request), so changing them while the server is serving is a data race.

### Connection Options

How a server serves its connections is set per server with `set_connection_options()`: two servers of one process may set different values. Set it before `listen()`: it is copied once and read by each connection the server accepts afterwards, so a later call never changes the connections open.

```cpp
thinger::http::connection_options options;            // the defaults:
options.idle_timeout = std::chrono::seconds(60);      // 120 s: an idle connection is closed
options.header_timeout = std::chrono::seconds(30);    // 60 s: a request head not complete, 408
options.request_timeout = std::chrono::minutes(10);   // 300 s: a request not received, 408
options.write_timeout = std::chrono::seconds(30);     // 60 s: a response the client stopped reading
options.linger_max_bytes = 1024 * 1024;               // 8 MiB  } closing with the body unread,
options.linger_timeout = std::chrono::seconds(2);     // 5 s    } see Limits and Timeouts per Route
options.logging = {true, 512};                        // off: see What the Message Logs Show
options.logging.redact.add("invite");                 // redacted_parameters::defaults()
server.set_connection_options(options);
server.listen("0.0.0.0", 8080);
```

- `idle_timeout`: a connection with no request arriving and no response being written for this long is closed. A request received whole (its body read to its end, or none) whose handler is running does not count as idle, however long the handler takes, as in Node and Go; nor does a request waiting for its response within the timeout of its route (see [Limits and Timeouts per Route](#limits-and-timeouts-per-route)). A request whose handler returned without answering it (to answer later, from a copy of the response) counts as idle, unless its route sets that timeout. Every byte received moves it, a body still arriving included: alone, it does not bound a client trickling its request, which is what the next two deadlines do.
- `header_timeout` (60 s, Node's `headersTimeout`): the maximum time from the start of a request to the end of its head, whatever the pace of its bytes (Slowloris). A request starts at its first byte; the first one of a connection, when the server starts serving it (after the TLS handshake), and a pipelined one, when the server starts parsing it. A head not complete in time is answered `408 Request Timeout` with `Connection: close`, and the connection is closed at once; a connection on which no byte of a request arrived is closed without a response (as Go and nginx do: a `408` there could be taken for the answer to a request the client was just sending). Between two requests of a keep-alive connection, `idle_timeout` applies. Capped by `request_timeout`, as in Node.
- `request_timeout` (300 s, Node's `requestTimeout`): the maximum time from the start of a request until its body was read to its end. Not received in time, it is answered `408 Request Timeout` with `Connection: close` (or its connection closed if its response already started), and its handler cancelled with `cancel_reason::timeout`. A route, a group or a virtual host sets its own: see the request deadline in [Limits and Timeouts per Route](#limits-and-timeouts-per-route).
- `write_timeout` (60 s, nginx's `send_timeout`): a response write of which the socket accepts no 64 KiB slice (the whole write, if smaller) for this long closes the connection, as a client that stopped reading, whether it was to be kept alive or closed after the response. It bounds a stall, not the length of a response: a response larger than a slice is written slice by slice, each one moving the deadline, and a response being written does not count as idle. Unlike SSE and WebSocket writes, the client taking bytes from the socket is not checked: a slow client with a large send buffer (the system may wake the writer only once a third of it is free) can be cut while it still reads, if the socket accepts no slice for this long. Raise it, or turn it off, for slow downloads.
- `0` disables any of the three; with the three at 0, a connection is served as in 0.8, with `idle_timeout` as its only deadline.
- `linger_max_bytes` and `linger_timeout`: the bounds of a lingering close (RFC 9112, section 9.6, see [Limits and Timeouts per Route](#limits-and-timeouts-per-route)); 0 for either closes at once. During a graceful shutdown a connection lingers 0.5 s at most.
- `logging`: the header values and the start of the bodies of the requests and responses, for debugging (see [What the Message Logs Show](#what-the-message-logs-show)).
- `logging.redact`: the query parameters whose values this server hides from the URIs it logs, whether the values above are logged or not (see [Query Parameters in the Logs](#query-parameters-in-the-logs)). Assigning `options.logging = {true, 512}` sets it back to the defaults, so set it afterwards, as above.

### Workers

A `pool_server` and an `async_client` run on `asio::workers`: a pool of threads, each running an io_context of its own, that the application creates, starts, hands to them by reference and stops. Nothing starts or stops the workers by themselves. A `pool_server` accepts on a thread of its own and serves each connection on the next thread of the pool; several servers and clients may share one `workers`, and several `workers` may live in one process, sharing nothing. The workers must outlive what runs on them: stop the servers and the clients (or destroy them) before stopping the workers. A server stopped or destroyed this way has closed its connections, keep-alive ones included, and cancelled its handlers (see [Server Lifecycle](#server-lifecycle)), and a client stopped or destroyed from a thread that is not one of the workers has ended its requests in flight, so stopping the workers then drops none of them (see [Async Client](#async-client-async_client)). Handlers still running on the workers end there, without the server.

```cpp
thinger::asio::workers workers;
workers.start();  // a thread per core; start(4) for four

thinger::http::pool_server server(workers);
// ... routes ...
server.listen("0.0.0.0", 8080);

thinger::http::async_client client(workers);  // the same threads

// ... until the application ends
server.stop();
client.stop();
workers.stop();
```

`start()` returns an `error_code`: `already_started` when they run (no thread is added), `invalid_argument` for 0 threads. `stop()` stops every thread and destroys the operations still pending; it can be called from any thread, one of the workers included, and more than once. For the application's own work, `next_executor()` returns the executor of the next thread of the pool (round-robin, without a lock), `isolated_executor(name)` the executor of a new thread out of the pool, named `name`, which runs until the workers stop (e.g. for work that blocks), and `in_worker_thread()` tells whether the calling thread is one of these workers' (a thread-local, without a lock). Both return a null executor when the workers do not run. `wait()` waits for a signal: see [Shutdown](#shutdown).

### Shutdown

A server stops in three steps: `stop_listening()` (quiesce), `shutdown(by)` (drain, until a deadline) and `stop()` (release). `co_await server.shutdown(by)`, with `by` a `std::chrono::steady_clock::time_point`, stops the server gracefully, e.g. before a deploy or on `SIGTERM`. `by` is the whole budget of the shutdown: nothing waits past it.

- it stops listening right away, if `stop_listening()` did not already: new connections are refused, on every listener (TCP and Unix sockets; the socket file is removed), those a `listen()` on another thread is still opening too. Until the shutdown ends, `listen()` fails with `shut_down` (from `on_shutdown()` too); once it ended, `listen()` serves again;
- idle keep-alive connections are closed at once;
- a connection handling a request finishes it: its response tells the client with `Connection: close` (unless the response already started: then the connection is closed after it), and the connection closes after it;
- connections taken over are told (once, on their thread): a `websocket_connection` closes with 1001 (Going Away), an `sse_connection` closes, and others call what the application set with `on_shutdown()` (`websocket_connection`, `sse_connection`), or with `scope.on_shutdown(callback)` for the connection of `take_over()` (`upgraded_connection`) or `upgrade_websocket_stream()` (`upgraded_websocket`), see [Taking Over the Connection](#taking-over-the-connection). Their `on_close()` tells when they closed. Proxied WebSockets (reverse proxy tunnels) are not told: they end when either side closes, or at their [idle timeout](#reverse-proxy). The server calls `on_shutdown()` once when the shutdown starts, for anything else the application runs (what it throws is logged, and the shutdown goes on);
- once every connection is gone it returns `true`, at once for a server with none open; at `by`, every handler still running whose response is not written yet is interrupted (its [cancellation scope](#cancellation) is cancelled with `cancel_reason::shutdown`, whatever its route declared: its next cancellable await ends with `operation_aborted`), whatever is left is closed and it returns `false`, without waiting for the handlers it interrupted. Then the server stops as with `stop()` (`wait()` returns), once the connections closed on their threads. It does not call `stop()` itself, which may end on a thread of the server while a subclass is being destroyed: a `stop()` overridden in a subclass of `pool_server` or `server` runs only when the application calls it.

Every listener drains at once, under the same `by`. `shutdown(by)` may be awaited from any executor, a thread of the server included (a handler, or a worker of a `pool_server`), which it does not block: the other connections of that thread go on being served. It may be called several times, concurrently too: every call shares the same end and result, and the first one sets `by`. A handler starting the shutdown does not await it, as the drain waits for its own request: it spawns it, as the [server shutdown example](examples/http_server/server_shutdown_example.cpp) does:

```cpp
server.post("/api/shutdown", [&](http::response& res) -> awaitable<void> {
    res.json({{"message", "shutting down"}});
    auto by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    co_spawn(co_await boost::asio::this_coro::executor, server.shutdown(by), [](std::exception_ptr, bool clean) {
        LOG_INFO("shut down{}", clean ? "" : " (some connections were cut)");
    });
});
```

`shutdown(grace)` does the same with `by` `grace` from now (30 s by default), blocking until it ends, for a thread that does not serve the server (e.g. `main()`); it returns the same result. On a thread of the server it would block the drain it waits for, so there it logs an error, starts the shutdown and returns `false` at once. Both may be called from an `asio::signal_set` handler, not from a raw signal handler (they lock and allocate). `stop()` during a shutdown ends it at once (`false`), closing the connections still open and cancelling their handlers, without waiting for them (see [Server Lifecycle](#server-lifecycle)). A server that is not running (an `http::server` whose `wait()` is not running, a `pool_server` whose workers stopped, or a server stopped) ends a shutdown at once: `true` if no connection is open. Workers stopping during a shutdown (`workers.stop()` on another thread, or on a worker awaiting it) make it end by its deadline, never throw.

The signals are the application's. With a `pool_server`, `workers::wait()` blocks until `SIGINT`, `SIGTERM` or `SIGQUIT` arrives (or one of the signals given, as `workers.wait({SIGTERM})`) and returns it, and stops nothing: the workers keep running, so the requests in flight finish, with the `async_client`s they use. The application then shuts its servers down and stops the workers. `wait()` returns 0 when `stop()` releases it instead (from another thread), and at once when the workers do not run; with an empty set it waits for `stop()`. While it waits the signals do not take their default action; once it returned they do again, so a second Ctrl+C during the shutdown ends the process.

```cpp
thinger::asio::workers workers;
workers.start();

thinger::http::pool_server server(workers);
// ... routes ...
server.on_shutdown([&] { devices.disconnect_all(); });
server.listen("0.0.0.0", 8080);

int signal = workers.wait();  // SIGINT, SIGTERM or SIGQUIT
LOG_INFO("signal {}: shutting down", signal);
bool clean = server.shutdown(std::chrono::seconds(30));
LOG_INFO("shut down{}", clean ? "" : " (some connections were cut)");
workers.stop();

// a WebSocket that says goodbye its own way
res.upgrade_websocket({}, [](std::shared_ptr<thinger::http::websocket_connection> ws) {
    ws->on_shutdown([ws] { ws->send_text("{\"event\":\"restarting\"}"); ws->stop(1012, "restarting"); });
});
```

An `asio::signal_set` of the application's own works too, as on an `http::server`: its handler spawns `shutdown(by)` (`co_spawn(io_context, server.shutdown(by), boost::asio::detached)`), and `server.wait()` returns once the shutdown ends.

Compared with Node.js: `server.close()` also stops listening, closes idle connections (since Node 19) and waits for the others, but has no deadline (that is `closeAllConnections()`, called by the application) and does not tell upgraded connections, which keep it waiting until they close. Here `shutdown(by)` does `close()`, `closeIdleConnections()` and, at `by`, `closeAllConnections()`, and new connections accepted meanwhile (e.g. in the middle of a TLS handshake) are closed at once.

### Statistics

`stats()` returns the connections open by what they serve (`http_connections`, `websocket_connections`, `sse_connections`, `tunnel_connections` for reverse proxy upgrades, `taken_over_connections` for other `take_over()`s, and `active_connections()`), and the connections served since the server was created (`total_connections`). Counting costs nothing per request: a connection joins or leaves a list (picked by its io_context, so threads do not contend for it) only when it opens, changes what it serves and closes; `stats()` reads those lists, so it is meant for a monitoring endpoint, not for every request. There is no request counter, nor bytes in and out: they would cost on every request (the [response hook](#response-hook) can count them). A connection taken over counts until its socket is destroyed or its scope says it closed (`scope.closed()`), which `websocket_connection` and `sse_connection` do once closed; `scope.set_type()` changes what it counts as.

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
client.log_bodies(true, 512);  // Header values and start of the bodies in the logs, for debugging
client.redact_parameters(names);  // Query parameters hidden from the logs, instead of the defaults
```

See [What the Message Logs Show](#what-the-message-logs-show) for the last two.

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
- **Lifetime**: the `async_client` closes idle connections with a timer on its workers. The synchronous `client` runs its io_context only during a call, and nothing of it runs between calls: it closes the expired ones whenever a request starts or ends, and never reuses them. So after a call, the connections it used in the last idle timeout stay open until the next call, half-closed (`CLOSE_WAIT`) once their servers close them: usually one per server, never more than the limits. curl's easy interface and Python's requests (urllib3) keep them the same way; Go and Node close them on time, as their event loop always runs, and Java's `HttpURLConnection` with a thread of its own. A long-lived `client` making sporadic calls can call `clear_connections()` after them, or disable the pool. Destroying the client, or stopping the `async_client`, closes its idle connections; stopping its workers does not (stop the client first).

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

### Client Certificates

A client presents a certificate to the servers that ask for one (mutual TLS) once it is given one, with its private key (PEM, not encrypted) and, optionally, the intermediate certificates sent along with it: on every TLS connection of the client, requests, WebSockets (`websocket()`, `upgrade_websocket()`) and proxy tunnels alike. From PEM text or from files:

```cpp
thinger::http::client client;
if (auto ec = client.client_certificate(certificate_pem, key_pem, intermediates_pem)) {
    // client_error::invalid_client_certificate, invalid_client_key or client_key_mismatch
}
auto res = client.get("https://api.example.com/devices");

// or files, read now
client.client_certificate_file("/etc/agent/device.pem", "/etc/agent/device.key", "/etc/agent/chain.pem");
if (auto ws = client.websocket("wss://devices.example.com/agent")) {
    ws->send_text("hello");
}
client.clear_client_certificate();   // present none again
```

They are checked when set, not at the first handshake: a certificate, chain or key that cannot be read (an encrypted key included: it fails, the password is never asked for on the terminal) or a key that is not the certificate's returns the error, and the current certificate is kept. The key itself is never logged. Files are read once, when set: set them again to read them again. Replacing the certificate while requests run is safe: the connections already open, and kept alive in the pool, keep the one they were opened with, and new connections take the new one. A server that asks for no certificate gets none.

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
- **Early responses**: a server may answer before the request is written whole and close the connection (e.g. `413` for a body over its limit). If writing the request then fails (`EPIPE` or a reset), the response the server sent is still read and returned, as curl does; the connection is not reused. A `408` read that way on a reused connection is taken as the server closing it while idle: it was not processed, so the request is sent once more on a new connection, whatever its method (see Retries in [Connection Pool](#connection-pool-keep-alive)).

### Async Client (async_client)

The async client supports three usage patterns: callbacks, coroutines, and the request builder. It runs its requests and callbacks on the [workers](#workers) it is given, which the application starts and keeps alive longer than the client (the snippets below use these `workers`).

`stop()` and the destructor cancel the requests in flight, in any phase, and close their connections and the idle ones. The callback of a request cancelled is not called (one running, of a request that completed before, is not interrupted). A coroutine, of `run()` or a callback that is one, sees its requests fail with `operation_aborted`, and any further one fail at once, without connecting. Requests started while `stop()` or the destructor runs are not run, and their callbacks are never called; those started after `stop()` returned run as before. `stop()` also releases `wait()` and `wait_for()` and leaves `pending_requests()` at 0. WebSockets already handed out (`websocket()`) belong to the application and stay open, and so does what it spawned on the executor of a coroutine of `run()`, which is the plain executor of its worker (`co_await this_coro::executor`).

What they wait for depends on the thread they are called from:

- **From a thread that is not one of the workers**, with the workers running, they return once no callback of the client runs and the requests cancelled ended, so whatever their callbacks and coroutines captured may be destroyed. A callback running is waited for as long as it takes, not interrupted. A coroutine that ignores the cancellation is waited for up to 1 s and then left behind: it holds what it uses of the client, ends by itself, and may resume after the call returned.
- **From one of the workers** (e.g. in a callback of the client, which may destroy it), they do not wait for the requests: `stop()` waits for nothing, and the destructor only for the callbacks running on other workers. No callback of a request cancelled is called once they returned, but coroutines are not waited for: those of other workers run at the same time, and the cancellation reaches them through the queue of their worker. A coroutine may resume after the call returned, and see `operation_aborted` or the correct response of a request that completed before the cancellation reached it. Its frame is destroyed on its worker.

Each request keeps what it uses of the client alive (its settings, connections and TLS contexts), so one that outlives the client touches no freed memory. The `async_client` object is not among those, and neither is what a coroutine uses itself: they are the application's, as an object is in any other language. A coroutine must not use a client that may be destroyed while it runs: not after a request was aborted, but also not before its first request or after a correct response, as it may be running on another worker at that very moment. If a callback ends the client, have the coroutines hold it by `std::shared_ptr`, captured by value, and have the callback call `stop()` and release its reference, so the client goes with the last one holding it, on a worker:

```cpp
auto client = std::make_shared<thinger::http::async_client>(workers);
auto done = std::make_shared<std::atomic<int>>(0);

client->run([client, done]() -> thinger::awaitable<void> {
    auto res = co_await client->get("https://api.example.com/data");
    if (res.ok()) ++*done;   // a response that completed before the cancellation reached it
});
client->get(close_url, [client](auto&) { client->stop(); });
client.reset();              // it goes with the last request holding it
```

The workers still have to outlive it, and destroying them with a coroutine still holding it is a memory error: before stopping them, keep a reference on that thread and call `stop()` there, which waits for those coroutines. The other way is to stop or destroy the client from a thread that is not one of the workers, also after a `stop()` from a callback, as that call waits for the coroutines.

#### Callback-based

```cpp
thinger::asio::workers workers;
workers.start();

thinger::http::async_client client(workers);

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
thinger::http::async_client client(workers);

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
thinger::http::async_client client(workers);

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

### Executor Client (executor_client)

`executor_client` runs on an io_context the caller already runs, usually the one of the coroutine making the call: its requests run on that thread, with no hop to another one, and it has no threads of its own and no idle timers. Its requests are awaited, as with `async_client`, and take the same settings (timeouts, redirects, TLS, CAs, proxies, the request builder). It keeps no idle connection by default, so each request opens its own, and it closes its connections when it is destroyed.

Use it for occasional calls from server coroutines (a token refresh, a webhook, an order to another service), created where it is needed:

```cpp
server.get("/weather", [](thinger::http::response& res) -> thinger::awaitable<void> {
    auto client = co_await thinger::http::executor_client::for_current_coroutine();
    auto forecast = co_await client->get("https://api.example.com/forecast");
    res.json(forecast.json());
});

// Or on a given io_context
thinger::http::executor_client client(io_context);
```

`for_current_coroutine()` takes the io_context of the calling coroutine, an io_context executor (as a coroutine of `async_client::run()` has, its worker's) or a strand of one (as server handlers have), and throws `std::logic_error` on any other executor (e.g. a `thread_pool`); `thinger::asio::io_context_of(executor)` (in `<thinger/asio/io_context_of.hpp>`) tells which io_context an executor runs on, or null. Use the client on the threads running its io_context (with several threads, from a strand of it), and destroy it there, before the io_context. Cancelling the coroutine awaiting a request (a cancellation slot, or `||` with a timer) cancels the request and closes its connection. WebSockets opened through it (`websocket()`) run on the executor of the caller, its strand included: their keepalive and closing handshake never run at the same time as the caller.

For many calls to the same host, as a gateway sending most requests to one backend, use a pooled client instead (`async_client`), whose connections are kept alive and shared by every request. `max_idle_connections_per_host()` enables keep-alive here too, but a client created per call does not keep its connections for the next one.

## WebSocket

### WebSocket Server

```cpp
server.get("/ws", [](auto& req, auto& res) {
    res.upgrade_websocket({}, [](std::shared_ptr<http::websocket_connection> ws) {
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

Frames the client sends right after its request, without waiting for the `101` (as Node's `ws` and Go's gorilla/websocket accept), are delivered as messages once the connection starts reading, with the same checks as any other.

### WebSocket Server with Protocols

```cpp
res.upgrade_websocket({.protocols = {"chat", "json"}}, [](auto ws) {
    ws->on_message([ws](const std::string& msg, bool binary) {
        ws->send_text(msg);
    });
});
```

A client offering several subprotocols (`Sec-WebSocket-Protocol: a, b`) gets the first one it offers that is in the list. One offering only others is answered with `400`, and so is one offering any when the list is empty. One offering none is answered with `400` when the list is not empty, unless `protocol_required` is `false`: then it is accepted without a subprotocol.

Both WebSocket upgrades take the same options, a `websocket_accept_options` (in `thinger/http/server/upgrade.hpp`):

| Option | Default | Description |
|---|---|---|
| `protocols` | none | Subprotocols spoken, alternatives included. The first one the client offers (in its order of preference) that is in the list is accepted, returned in the response (and in `protocol` to the handler of the [byte stream](#websocket-as-a-socket)) |
| `protocol_required` | `true` | With `protocols`: a client offering none of them is answered with `400`. If `false`, it is accepted without a subprotocol (and the handler gets an empty one) |
| `binary` | `true` | As a byte stream: messages written are binary, or text (`ws->set_binary()` changes it later). The message interface writes each message as it is sent |
| `max_message_size` | 16 MiB | With the message interface: the largest message received (at least 1 byte). A larger one closes the connection with `1009` (Message Too Big). A byte stream has no messages, and no limit |
| `ping_interval` | unset | Ping the client once nothing was received from it for this long; `0` turns it off. Unset: 60 s with the message interface, off as a byte stream |
| `idle_timeout` | unset | Close the connection once nothing was received for this long, pongs included; `0` turns it off. Unset: 120 s with the message interface, off as a byte stream |

Without `protocols`, a client offering a subprotocol is answered with `400` by `upgrade_websocket()`, and accepted without one by `upgrade_websocket_stream()`.

### WebSocket Close

`on_close()` is called once the WebSocket closed, whatever closed it (the client, a lost connection, `stop()`, the server shutdown, the overflow policy...), once, after its last message: nothing can be sent anymore. It tells how, as Node's `ws` `'close'` event: the status code of the client's close frame and its reason (when the server closed first and the client answered, the code of the answer, normally its echo), or the code the server closed with, or `1006` when no close frame was exchanged (the connection was lost, closed by the server at the deadline of a shutdown, or the close frame of `stop()` was not written whole by its deadline).

```cpp
res.upgrade_websocket({}, [&](std::shared_ptr<http::websocket_connection> ws) {
    auto id = subscribers.add(ws);
    ws->on_close([&, id](const http::websocket_close& close) {
        // close.code (1000, 1001, 1006...), close.reason, close.remote (the client closed first)
        subscribers.remove(id);
    });
});
```

It is always posted to the thread of the connection, never called from within the call that closed it, so a loop sending to every connection can remove the ones closing; what it throws is logged. Set it in the handler, or later on the thread of the connection: set once the WebSocket closed, it is posted at once. It is not called once the socket is released (`release_socket()`), nor when the io_context is stopped and destroyed with the connection open (e.g. by `stop()` of an `http::server`, which stops its io_context). A server WebSocket whose `on_message()` throws is closed with `1011`.

`stop()` closes with `1000`. `stop(code, reason)` closes with a code and a reason of your own, from any thread, as `ws.close(code, reason)` in Node:

```cpp
// The credential the connection was opened with has expired
ws->stop(1008, "token expired");
```

It takes the same path as any other close: the closing handshake, the keepalive stopped, the awaited sends told `false`, and `on_close()` called once, so `release_socket()` is not needed to choose the code. The close frame goes after the message being written, ahead of the messages still queued, which are not sent; what must arrive first is awaited with `send_text_async()` or `send_binary_async()`. Nothing else is sent after it.

- **Arguments**: a `code` that a close frame may not carry (anything but 1000–1014 without 1004, 1005 and 1006, and 3000–4999), a `reason` above 123 bytes (bytes of UTF-8, not characters) or a `reason` that is not valid UTF-8 throw `std::invalid_argument` in the calling thread, whatever the state of the connection: nothing is sent and the connection stays as it was. An empty `reason` sends the code alone.
- **One close**: once closing or closed, a call with valid arguments does nothing else: there is one close frame and the first code decides. Calling it in `on_shutdown()` replaces the `1001` of a [shutdown](#shutdown), as with `ws->stop(1012, "restarting")`.
- **Deadline**: the connection is closed and `on_close()` called within `thinger::asio::websocket::CLOSE_TIMEOUT` (5 seconds) of the call, with or without arguments, whatever the client does: not answering, not reading with a large message being written, or reading a few bytes at a time. At the deadline the socket is closed without the handshake, as `Conn.Close()` of Go's `coder/websocket` (Node's `ws` waits 30 seconds). It does not wait for the [write timeout](#websocket-flow-control), and never ends in `1013`. A message still being written then is cut, even to a slow client that keeps reading.
- **What `on_close()` gets**: the code and reason of the client's answer, normally its echo, with `remote` false; if the client did not answer (or ended the connection without answering), the code sent, with an empty reason; `1006` if the close frame was not written whole. If the close frame of the client came first, its code, with `remote` true.

The WebSocket used as a socket (`upgrade_websocket_stream()`, `thinger::asio::websocket`, see [WebSocket as a Socket](#websocket-as-a-socket)) has the same `on_close()`, with an `asio::websocket_close` (`http::websocket_close` is the same type): called once, whatever closed it, with the same guarantees. It has no read loop of its own: the connection is noticed lost by the operations running on it (a protocol always has a read pending; the keepalive), with `1006` when no close frame was exchanged, and closing the socket under it (the end of a server's shutdown) counts too. The callback must not own the WebSocket, which nothing would release: capture a `std::weak_ptr`.

```cpp
res.upgrade_websocket_stream({}, [&](http::upgraded_websocket upgraded) {
    auto ws = std::move(upgraded.stream);
    ws->on_close([&sessions, id = sessions.add()](const thinger::asio::websocket_close& close) {
        sessions.remove(id);   // close.code, close.reason, close.remote
    });
    co_spawn(ws->get_io_context(), run_protocol(ws), thinger::detached);
});
```

### WebSocket Flow Control

`send_text()` and `send_binary()` queue the message and return. The queue holds up to 100 messages; one sent to a full queue is dropped (the newest), which suits sampled telemetry but not a stream that must arrive whole. For those, the server can follow the pace of the client, and stop reading a client whose data it cannot forward yet:

```cpp
res.upgrade_websocket({}, [](std::shared_ptr<http::websocket_connection> ws) -> thinger::awaitable<void> {
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
- **Dropped messages**: `dropped_messages()` counts what `drop_newest` and `drop_oldest` dropped since the connection was created, the new message or the older ones, as a 64-bit number that never decreases, e.g. for metrics or to tell the client it lost data. Any thread may read it. Awaited sends are not counted, as they are never dropped; nor is what is sent once the connection is closing, what was queued when it closed, or an overflow with the `close` policy. A send from another thread counts once the thread of the connection took it, so once all were taken (e.g. after `execute()`), written + queued + dropped is what was sent while open.
- **Write timeout** (`write_timeout`, 60 s by default, `0` disables it): a write that makes no progress for that long closes the connection with `1013`, as a client that stopped reading; awaited sends and `wait_writable()` then complete with `false`, and the queue is released. It bounds a stall, not the time a message takes: a write progresses while the socket takes bytes of it (a message over 64 KiB is written in 64 KiB slices, each one progress), or the client takes bytes from the socket, which is checked only once the socket took nothing for the timeout, for a quarter of it more. So a slow client that keeps reading gets every message, however large, and is never cut, and one that stopped reading is closed within the timeout and a quarter (75 s by default). 60 s is nginx's `send_timeout` and `proxy_send_timeout` (the time between two successive writes), and the server's own [`write_timeout`](#connection-options). `set_write_limits()` replaces every limit, but limits that do not name `write_timeout` keep the 60 s (as the examples above do). Changed while a write is stalled, the new value applies at once: 0 cancels the deadline, and another one counts from the last progress (from the call, if the deadline was off), so a shorter one may close the connection right away. 0.8 had no deadline by default: a client that stopped reading kept its connection, and its newest messages were dropped, until it left. Once `stop()` was called, the deadline of the [close](#websocket-close) applies instead, 5 seconds.
- **Pausing reads**: after `pause_reading()` nothing more is read from the socket: called from `on_message()`, the message being delivered is the last one; from another thread, one more may be delivered, so the TCP window of the client fills and its writes block, as `socket.pause()` in Node; `resume_reading()` goes on where it stopped. While paused, the pings and the close frame of the client are not seen (as in any WebSocket not being read: the client may time out), and the keepalive is suspended, as its pongs could not be read: a dead client is not noticed until reading resumes, or until what the server sends it stalls for the write timeout (60 s by default), which then closes it. A paused connection that sends nothing has no deadline, so bound the pause. `stop()` still closes it: the closing handshake reads up to the close frame of the client, discarding the messages before it.

### WebSocket as a Socket

`res.upgrade_websocket_stream()` hands the accepted WebSocket out as a socket (`thinger::asio::websocket`, a `thinger::asio::socket`), with the same interface as TCP, TLS and Unix sockets: a protocol written for a socket (MQTT, a custom binary protocol...) runs over WebSocket unchanged, without telling the difference, and a tunnel connects it to any other socket.

```cpp
server.get("/mqtt", [](http::request& req, http::response& res) {
    res.upgrade_websocket_stream({.protocols = {"mqtt", "mqttv3.1", "mqttv3.1.1"}},
        [](http::upgraded_websocket upgraded) {
            // upgraded.protocol: the subprotocol accepted, e.g. "mqttv3.1"
            auto ws = std::move(upgraded.stream);
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

The handler runs on the io_context of the socket once the `101` response is written, with an `http::upgraded_websocket`: the WebSocket (`stream`), the subprotocol negotiated (`protocol`, empty if none) and the `scope` of the connection (see [Taking Over the Connection](#taking-over-the-connection)), e.g. to close it when the server shuts down. Like any socket, the WebSocket is used there (see [Thread Safety](#thread-safety)). Its options are those of [`upgrade_websocket()`](#websocket-server-with-protocols), with no keepalive by default and no message limit.

The handshake is checked as RFC 6455 requires: a request that is not a WebSocket upgrade, or of another version than 13, gets `426` (with `Sec-WebSocket-Version: 13`); one that is not a `GET`, lacks `Connection: Upgrade` or has an invalid `Sec-WebSocket-Key`, `400`. Frames the client sends right after its request, without waiting for the response, are kept for the first reads.

As a stream:

- Reads return the payload of the data messages received, binary or text, as one continuous stream: neither messages nor the frames of a fragmented message are delimited. `read()` waits for exactly the bytes asked, across frames and messages; `read_some()` returns what one frame holds at most, and never 0 bytes (empty frames are skipped); `read_until()` and the `streambuf` reads work as on TCP. Text is not validated as UTF-8 (the stream may cut a character anywhere), nor are close reasons; `upgrade_websocket()` validates whole text messages.
- Each write sends one whole message (unfragmented). Writes may be called while others are pending, from several coroutines: they are sent one after another, in call order, never interleaved. Writing 0 bytes sends nothing. A write interrupted in the middle of its frame (`cancel()`, or the cancellation of the operation) leaves the rest of the frame to be sent before anything else, so the framing is never broken.
- Pings are answered (and unsolicited pongs ignored) while reading, so some read must be pending, as a protocol reading its socket has. The pong goes out as soon as no frame is being written, without holding the reads. The keepalive counts on reading too: what is not read is not received. It sends one ping at a time, and closes the connection if a ping cannot be sent.
- A close frame from the client is answered, the connection closed, and reads end with `boost::asio::error::eof`, as on a TCP connection closed by its peer. Writes fail afterwards.
- `close()` returns at once: pending operations end, the socket is closed for its user, and the closing handshake finishes on its own: the rest of a frame being written, a close frame (code 1000), and the connection closed when the client answers, or after 5 seconds. Called anywhere else than on the executor of the WebSocket (e.g. by the synchronous client between calls, while nothing runs its io_context), it closes the connection at once instead. `co_await ws->close_graceful(code, reason)` does the handshake, waiting, with `1000` and no reason by default, and within 5 seconds whatever the client does: at the deadline the socket is closed without it, and `on_close()` gets the code sent, or `1006` if its frame was not written whole. A WebSocket destroyed open on its executor sends 1001 (going away), and so does the idle timeout; protocol errors (an unmasked frame, an unknown opcode...) are answered with 1002.

For message-oriented code `upgrade_websocket()` (above) remains; `ws->read_message_some()` with `is_message_complete()` and `is_binary()` gives the message boundaries over the same socket.

### Tunnels

`thinger::asio::socket_pipe` copies data both ways between two sockets until either end closes: a WebSocket and a TCP, TLS or Unix socket (a VNC server, a Docker container, a service behind a proxy), or a socket of your own (see [Custom Sockets](#custom-sockets)):

```cpp
server.get("/vnc", [](http::request& req, http::response& res) {
    res.upgrade_websocket_stream({.protocols = {"binary"}}, [](http::upgraded_websocket upgraded) {
        auto ws = std::move(upgraded.stream);
        co_spawn(ws->get_io_context(), [ws]() -> thinger::awaitable<void> {
            // the other end, on the io_context of the WebSocket
            auto target = std::make_shared<thinger::asio::tcp_socket>("vnc", ws->get_io_context());
            if (co_await target->connect("10.0.0.5", "5900", std::chrono::seconds(10))) {
                ws->close();
                co_return;
            }
            auto pipe = std::make_shared<thinger::asio::socket_pipe>(ws, target);
            pipe->set_idle_timeout(std::chrono::minutes(5));   // optional: none by default
            co_await pipe->run();   // until either end closes (or pipe->start() to run it detached)
        }, thinger::detached);
    });
});
```

- Each direction writes what it read before reading more, so a slow reader slows its writer down (flow control): nothing piles up in memory.
- When either end closes (or fails), the pipe closes both: the WebSocket client gets a close frame when the other end closes, and the other end is closed when the client closes. `pipe->cancel()` ends it from any thread.
- **Idle timeout**: `set_idle_timeout(timeout)`, before `run()` or `start()`, ends the pipe as `cancel()` does (both sockets closed, the on-end callback called once) once nothing was forwarded in either direction for that long. A byte counts once written to the other socket, so a peer that stops reading ends the pipe too. There is none by default, as in 0.8: a generic tunnel is often long-lived and quiet, with a keepalive of its own, so the deadline is the application's to choose (the [reverse proxy](#reverse-proxy) sets one on its tunnels). Each write moves one timer lazily, without a timer operation per chunk; as both directions move it, the pipe must then run on an io_context run by one thread (as the library's workers are) or on a strand.
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

It is used on its executor (`socket->get_executor()`): the one of the coroutine that opened it, a strand of it included, where the WebSocket also runs its keepalive and closing handshake.

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

The synchronous calls (`send_text()`, `send_binary()`, `receive()`, `close()`, and `set_keepalive()` outside the executor of the WebSocket) are for `http::client`, and for threads that do not run the io_context of the WebSocket. With `http::client` they wait running its io_context, until their own operation completes. On a thread running the io_context of the WebSocket (an `async_client` worker, so its callbacks, a coroutine of `executor_client`, a strand of either), where they could only wait for a handler they hold up or run other connections' handlers within their caller, they throw `std::logic_error` before starting anything. On any other thread they wait for the operation without running the io_context, so something else must be running it (the workers of the `async_client`, the threads of the `executor_client`): prefer the coroutine API on workers too, as a worker waiting on another one serves nothing meanwhile, and two waiting on each other never return. Inside callbacks and coroutines use the coroutine API (`send_text_async()`, `receive_async()`, `close_async()`), `run()` with `on_message()`, or `stop()` to close without waiting.

Empty messages are messages, both ways (RFC 6455 allows them): `send_text("")` sends one, and `receive()` returns one as an empty string while `is_open()` stays `true` (an empty string with the connection closed is the end). With `run()`, `on_message()` gets them, and `on_close()` is called only once the connection closed.

### WebSocket Client (Async Callback)

The `async_client` runs on started [workers](#workers), as in [Async Client](#async-client-async_client). The callback runs on a worker, where the synchronous calls throw (see [WebSocket Client (Sync)](#websocket-client-sync)): make it a coroutine and use the coroutine API, or use `run()` and `stop()` (see [WebSocket Client (Event-Driven)](#websocket-client-event-driven)).

```cpp
thinger::http::async_client client(workers);

// Simple connect
client.websocket("wss://server.com/ws", [](std::shared_ptr<http::websocket_client> ws) -> awaitable<void> {
    if (ws) {
        co_await ws->send_text_async("Hello!");
        auto [msg, _] = co_await ws->receive_async();
        std::cout << "Received: " << msg << std::endl;
        co_await ws->close_async();
    }
});

client.wait();

// With custom headers (using request builder)
client.request("wss://server.com/ws")
    .header("Authorization", "Bearer token")
    .protocol("graphql-ws")
    .websocket([](std::shared_ptr<http::websocket_client> ws) -> awaitable<void> {
        if (ws) {
            co_await ws->send_text_async("Hello!");
            co_await ws->close_async();
        }
    });

client.wait();
```

### WebSocket Client (Coroutine)

```cpp
thinger::http::async_client client(workers);

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
thinger::http::async_client client(workers);
std::shared_ptr<http::websocket_client> socket;   // kept while its message loop runs

client.websocket("wss://server.com/ws", [&socket](std::shared_ptr<http::websocket_client> ws) -> awaitable<void> {
    if (!ws) co_return;
    socket = ws;

    ws->on_message([ws = ws.get()](const std::string& msg, bool binary) {
        std::cout << "Received: " << msg << std::endl;
        ws->stop();  // Done: close without waiting
    });

    ws->on_close([]() {
        std::cout << "Connection closed" << std::endl;
    });

    ws->on_error([](const std::string& error) {
        std::cerr << "Error: " << error << std::endl;
    });

    co_await ws->send_text_async("Hello!");
    ws->run();  // Start message loop
});

client.wait();
```

`run()` returns at once: the loop reads the messages on the executor of the WebSocket (a worker), calls `on_message()` there for each, and `on_close()` once the connection closed or `stop()` was called. It runs while the `websocket_client` lives (its destructor stops it), so the application keeps the `shared_ptr` the callback got, beyond the callback, which ends the request: `client.wait()` does not wait for the loop.

## Custom Sockets

Protocols and tunnels work on `thinger::asio::socket`, so an application can define its own socket type and use it wherever a socket is used, e.g. a virtual stream multiplexed inside another connection as an end of a `socket_pipe`, or under a protocol. A socket must implement:

| Method | Guarantee |
|---|---|
| `read_some(buffer, max_size)` | Waits until at least one byte is available and returns it: never 0 bytes without an error (unless `max_size` is 0). `boost::asio::error::eof` once the other end closed the stream and everything it sent was read |
| `write(buffer, size)` | Returns once all the bytes are written (accepted, as a TCP socket copies them to the kernel), or with an error |
| `close()` | Closes right away, without waiting: `is_open()` is `false` afterwards, pending operations complete with an error and new ones fail. Idempotent, and safe from the destructor |
| `cancel()` | Pending operations complete with `operation_aborted`; the socket stays open |
| `is_open()` | Whether it is open |

Everything else has a default: `read()` (exact), `read(streambuf)`, `read_until()` and the other `write()` overloads are built on these; `connect()`, `wait()` and `peer_closed()` answer `operation_not_supported` (then an HTTP server on the socket does not watch for a client leaving while its response is pending); `is_secure()` is `false`, `available()` and `send_backlog()` 0 and the address getters empty (`send_backlog()`, the bytes written that the peer has not taken yet, lets the write timeout of an SSE or WebSocket connection on the socket see a slow reader taking them). Override them when the socket can do better. As C++ hides the overloads of a name a class declares, add `using socket::read;` and `using socket::write;` to call the default overloads on the derived type.

Sockets are not thread-safe: every call, `close()` and `cancel()` included, happens on a thread running the io_context passed to the constructor, and operations complete there. Data arriving from another thread (e.g. from the connection the virtual stream runs inside) is handed over by posting it to that io_context. At most one read and one write are pending at a time. Pending operations should support per-operation cancellation (asio operations and timers do), used by the awaitable operators (`||`); `close()` must end them anyway.

```cpp
// A virtual stream: what is written goes to `send`; what is received comes from push(),
// called from any thread (an empty string ends the stream)
class virtual_stream : public thinger::asio::socket, public std::enable_shared_from_this<virtual_stream> {
public:
    virtual_stream(boost::asio::io_context& io_context, std::function<void(std::string)> send)
        : socket("virtual", io_context), signal_(io_context.get_executor()), send_(std::move(send)) {}

    // keep the default overloads visible next to the ones implemented
    using socket::read;
    using socket::write;

    void push(std::string data) {
        boost::asio::post(io_context_, [self = shared_from_this(), data = std::move(data)] {
            if (data.empty()) self->eof_ = true;
            self->inbox_ += data;
            self->signal_.set();        // wakes the pending read
        });
    }

    thinger::awaitable<thinger::io_result> read_some(uint8_t buffer[], size_t max_size) override {
        cancelled_ = false;   // one read pending at a time: a cancel() before it does not count
        for (;;) {
            if (!open_) co_return thinger::io_result{boost::asio::error::bad_descriptor, 0};
            if (!inbox_.empty()) {
                size_t bytes = std::min(max_size, inbox_.size());
                std::memcpy(buffer, inbox_.data(), bytes);
                inbox_.erase(0, bytes);
                co_return thinger::io_result{boost::system::error_code{}, bytes};
            }
            if (eof_) co_return thinger::io_result{boost::asio::error::eof, 0};
            // wait for push(), close() or cancel(); false if this read was cancelled (`||`, a slot)
            signal_.reset();
            if (!co_await signal_.wait() || cancelled_)
                co_return thinger::io_result{boost::asio::error::operation_aborted, 0};
        }
    }

    thinger::awaitable<thinger::io_result> write(const uint8_t buffer[], size_t size) override {
        if (!open_) co_return thinger::io_result{boost::asio::error::bad_descriptor, 0};
        send_(std::string(reinterpret_cast<const char*>(buffer), size));
        co_return thinger::io_result{boost::system::error_code{}, size};
    }

    void close() override { open_ = false; signal_.set(); }
    void cancel() override { cancelled_ = true; signal_.set(); }
    bool is_open() const override { return open_; }

private:
    thinger::asio::async_event signal_;   // <thinger/asio/async_event.hpp>
    std::function<void(std::string)> send_;
    std::string inbox_;
    bool eof_ = false;
    bool open_ = true;
    bool cancelled_ = false;
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

The queue of an SSE connection has the same flow control as a WebSocket, counting fields (`send_event()`, `send_data()`, `send_retry()`): `set_write_limits()` (up to 101 fields by default, dropping the newest), `queued_bytes()`, `queued_messages()`, `dropped_messages()` (in the same unit: each field dropped counts one, also when `drop_oldest` drops a whole event, and the keep-alive comments never count), the watermarks, the [write timeout](#websocket-flow-control) and `wait_writable()`. `drop_oldest` drops whole events, never the rest of an event being written (fields of an event without its data yet are not dropped: the newest field is, then); `close` and the write timeout close the connection, and `EventSource` reconnects.

A client that stops reading is closed by the write timeout: `write_timeout`, 60 s by default, as nginx's `send_timeout` and the server's own [`write_timeout`](#connection-options). It bounds a stall, the time a field being written goes without the socket taking any byte of it nor the client taking any byte from the socket (checked once the socket took nothing for the timeout, for a quarter of it more: a stalled write closes within the timeout and a quarter), not the time an event or the stream takes: an event over 64 KiB is written in 64 KiB slices, each one progress, so a slow client that keeps reading gets every event, however large, and is never closed by it. When it passes, the connection closes, a warning is logged, `on_close()` is called, and the pending `send_async()` and `wait_writable()` complete with `false`, so a producer awaiting them ends. Limits given to `set_write_limits()` that do not name `write_timeout` (as in the example below) keep the 60 s; `write_timeout = 0` disables it, as in 0.8, where a client that stopped reading kept its connection, and its newest events were dropped, until the process ended.

```cpp
res.start_sse([](std::shared_ptr<http::sse_connection> sse) -> thinger::awaitable<void> {
    sse->set_write_limits({.max_messages = 0, .high_watermark = 1 << 20, .low_watermark = 256 << 10});
    for (auto& update : updates) {
        sse->send_data(update);
        if (!co_await sse->wait_writable()) co_return;   // closed
    }
});
```

`send_data()` sends each line of its value (split on CRLF, CR or LF) as a `data:` field of its own, so the client gets the value as sent (pretty-printed JSON included), with its line breaks as LF; a value cannot inject fields or events. `send_event()` and `send_id()` take one line: a value with CR or LF (or NUL, for an ID) is refused, an error logged, and they return `false`.

A quiet stream is kept alive with a comment (`: keep-alive`, ignored by `EventSource`) every 15 seconds nothing else was sent, so proxies and load balancers with an idle timeout do not close it. The `keepalive` of the `http::sse_options` given to `start_sse()` sets it, and `set_keepalive(interval)` changes it later; 0 disables it. The keep-alive neither resets nor escapes the write timeout: it is written only when nothing else is queued or being written, so one coming due while a write is stalled is skipped, not queued behind it, and the deadline of that write is not moved; a keep-alive that cannot be written is bounded by the write timeout as any event is, and one that is written is progress like any other. The stream is not closed for being quiet: a client that left is noticed by reading the connection, and one that stopped reading by the write timeout. On a quiet stream that takes days, though: only keep-alives are written, about 13 bytes every 15 seconds, so the socket buffers of a client that stopped reading fill, and a write stalls, only after that long (the server does not set TCP keep-alive on its connections).

```cpp
res.start_sse([](std::shared_ptr<http::sse_connection> sse) {
    // ...
}, {.keepalive = std::chrono::seconds(30)});
```

`on_close()` tells when the stream closed: the client left (the connection reads the client, which sends nothing after its request, only to notice it closing), `stop()`, the server shutdown, the overflow policy or the write timeout. It is called once, as the WebSocket one: posted to the thread of the connection, never from within the call closing it (a send overflowing the queue, `stop()`), what it throws logged; set once the connection already closed (e.g. by a coroutine handler running late), it is posted at once. A client half-closing the connection has left.

```cpp
res.start_sse([&](std::shared_ptr<http::sse_connection> sse) {
    auto id = feed.subscribe([sse](const std::string& event) { sse->send_data(event); });
    sse->on_close([&, id] { feed.unsubscribe(id); });
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

At `info`, a server logs one line for each listener it opens, one when a graceful shutdown starts (with the connections open and the time left until its deadline) and one when it stopped, as Node and Go servers do; `workers::wait()` logs the signal it returns. Warnings, as a self-signed certificate generated for want of one, stay. Each connection accepted, the threads and the steps of starting and stopping (workers, acceptors, clients) log at `debug`; the requests and responses are logged as described below.

```text
listening on http://0.0.0.0:8080
listening on https://0.0.0.0:8443
received signal 15
shutting down the server on http://0.0.0.0:8080, https://0.0.0.0:8443: 3 connections open, grace period of 30000 ms
server on http://0.0.0.0:8080, https://0.0.0.0:8443 stopped
```

### What the Message Logs Show

The client and the server log each request and response: the request line or the status (`info`), the header names (`debug`) and the body size (`trace`). In every URI logged, the values of the query parameters that carry credentials are shown as `[redacted]` (see [Query Parameters in the Logs](#query-parameters-in-the-logs)). Header values, bodies and WebSocket messages are never logged by default, as they may hold credentials: tokens, cookies, passwords, private keys.

For debugging, the `logging` of the server's [connection options](#connection-options) (`log_bodies()` on a client) also logs the header values (`debug`) and the start of the bodies (`trace`), up to a maximum length, with control characters escaped. The values of `Authorization`, `Proxy-Authorization`, `Cookie` and `Set-Cookie` are redacted even then, the query of `Location`, `Content-Location` and `Referer` is redacted as in any other URI, before it is truncated, and WebSocket messages are still logged by their size alone.

```cpp
http::server server;
http::connection_options options;
options.logging = {true, 512};
server.set_connection_options(options);   // before listen()

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

#### Query Parameters in the Logs

A URI logged by the client or the server keeps everything (the other parameters, their order, the separators, the fragment) except the values of some query parameters:

```text
[SERVER REQUEST] GET /cb?code=[redacted]&state=s
```

By default, those of `access_token`, `api_key`, `apikey`, `authorization`, `client_secret`, `code`, `id_token`, `key`, `password`, `refresh_token`, `secret`, `sig`, `signature`, `token`, `x-amz-security-token`, `x-amz-signature` and `x-goog-signature`: `http::util::url::redacted_parameters::defaults()`. A name is compared whole, so `codec` is not `code`, without case and percent-decoded; there are no wildcards. The password of the user info of a URL is always redacted, whatever the names.

This applies to the request line of the server and the client, to the WebSocket a client connects to, the redirects it follows (`debug`) and its errors about a URL (an invalid WebSocket URL, a proxy it cannot use), and to the values of `Location`, `Content-Location` and `Referer` when header values are logged. The reverse proxy logs the path of a request, without its query. The names are those of query parameters alone: a secret in a path (`/reset/<token>`) or a body is not recognized, so keep it in a parameter, or out of the logs.

Each server and each client has its own names. Whatever you set replaces the defaults, as the `redact` of pino does: start from them to add or remove a name.

```cpp
using http::util::url::redacted_parameters;

// A server: in its connection options, before listen(). A later call only reaches new connections
http::connection_options options;
options.logging.redact.add("invite").remove("key");   // the defaults, with one more and one less
server.set_connection_options(options);

// A client, an async_client too: from its next request, on a pooled connection too
auto names = redacted_parameters::defaults();         // a copy: defaults() itself is const
names.add("invite");
client.redact_parameters(names);

options.logging.redact = {"invite"};                  // only this one: code=abc is logged
options.logging.redact = {};                          // no parameter at all
```

For your own log lines, `http::util::url::loggable_uri(uri, names)` redacts as the library does, and `loggable_uri(uri)` does it with the defaults.

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

- `http::server`, `http::pool_server` - Thread-safe, can handle concurrent requests. `listening_on()`, `is_listening()`, `stats()`, `stop_listening()`, `stop()` and `co_await shutdown(by)` from any thread, a thread of the server included; the blocking `shutdown(grace)` from a thread that does not serve the server (on one that does, it returns `false` at once, see [Shutdown](#shutdown)); `set_connection_options()`, `listen()` and the application (routes, middlewares...) from the thread setting the server up, before it serves (see [Server Lifecycle](#server-lifecycle)); a `listen()` of a `pool_server` may also run while other threads call `stop_listening()`, `stop()` or `shutdown()` (see [Listeners](#listeners))
- `http::request_context` - Not synchronized: used by the coroutine processing its request (middlewares, then handler)
- `http::response` - Copies share the response and can answer from any thread, even at the same time: only the first answer is sent. Chunks (`write_chunk()`, `end_chunked()`) are written in call order once `start_chunked()` returned `true`. Set the status and headers (`status()`, `header()`) from the thread running the handler, before answering, also on routes with a timeout: a timeout answer (504) copies the headers set so far
- `http::client` - Single-threaded, create one per thread or use `async_client`
- `http::async_client` - Thread-safe, runs on the `asio::workers` it is given
- `asio::workers` - `next_executor()`, `isolated_executor()`, `in_worker_thread()`, `stop()` and `wait()` from any thread (`stop()` from one of its own threads too); never destroy them from one of their threads
- `http::websocket_client` - Not synchronized, sends included. The coroutine API (`send_text_async()`, `receive_async()`, `close_async()`), `run()`, `stop()` and `set_keepalive()` on the executor of the WebSocket, where `run()` calls its callbacks: the `async_client` callback or coroutine that got it, the `executor_client` coroutine that opened it. The synchronous calls (`send_text()`, `send_binary()`, `receive()`, `close()`) from a thread that does not run its io_context; on one that does they throw `std::logic_error` (see [WebSocket Client (Sync)](#websocket-client-sync)). The synchronous calls of an `http::client` WebSocket are made by one thread at a time, as for `http::client` itself
- `http::websocket_connection` - Server-side WebSocket, thread-safe send and `stop()`, `queued_bytes()`, `queued_messages()` and `dropped_messages()` from any thread, callbacks on IO thread
- `http::sse_connection` - Runs on its executor (`get_executor()`, a strand if given one), where its callbacks run: `start()`, the `send*()` methods, `stop()`, `execute()`, the flow control setters, `queued_bytes()`, `queued_messages()` and `dropped_messages()` from any thread
- `http::server_scope` - On the thread of the socket it came with
- `http::http_application::on_response()` - The hook runs on the thread of the connection, once per request
- `asio::socket` (TCP, TLS, Unix, `asio::websocket`, custom sockets) - Not thread-safe, like the asio objects they wrap: use a socket on a thread running its io_context (`get_io_context()`), dispatching there from other threads; an `asio::websocket` on its executor (`get_executor()`: the io_context of its socket, or the strand of the coroutine that opened it from a client), where it runs its keepalive and closing handshake too. `socket_pipe::cancel()` may be called from any thread
- `asio::listener` - `stop()` from any thread, its handler included; `open()`, then `start()`, from one thread at a time. The handler runs on the serve executor of each connection

## License

MIT License - see [LICENSE](LICENSE) for details.

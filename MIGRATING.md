# Migrating

How to move code from one minor version to the next while the version is 0.x (see [Versioning](README.md#versioning)). Each entry of the [changelog](CHANGELOG.md) under **Changed** and **Removed** says what changed and how to adapt; this guide puts the breaking ones in the order a migration meets them, with code before and after.

## From 0.10 to 0.11

0.11.0 adds what servers need to read large bodies (a read deadline the handler moves, blocks of a fixed size from a `multipart_reader`, 64-bit sizes) and changes what a file of an unknown type is sent as. Almost all code compiles as it is. What needs a closer look is what a server sends for files without a known extension, and the headers a handler sets before answering.

A migration usually goes in this order:

1. Fix what no longer compiles: a `switch` over `body_error` ([A new `body_error` value](#a-new-body_error-value)), and code that names `size_t` for a body or part size on macOS or a 32-bit build ([64-bit sizes](#64-bit-sizes)).
2. Check what files are sent as: [files of an unknown type](#files-of-an-unknown-type) are `application/octet-stream`, from a server and from a client `form`.
3. Check the [headers set before the response](#headers-set-before-the-response): a `Content-Type` set before `send_file()` now wins, and `send_response()` sends the headers set with `header()`.
4. Replace the workarounds: a timer of your own around the reads of a body (`req.set_read_deadline()`), a copy to cut the pieces of a `multipart_reader` (`reader.read(max_size)`), and a header removed before setting it again (`res.set_header()`). Then review the [behaviour changes](#behaviour-changes-to-check-in-011).

### Old → new

| 0.10 | 0.11 | Notes |
|---|---|---|
| `switch (req.get_body_error())` with a case for each value | add `case body_error::timeout:` | only reached after `set_read_deadline()`; see [below](#a-new-body_error-value) |
| `size_t` limits and sizes of bodies and parts | `std::uint64_t` | assigning a `size_t` still compiles; see [64-bit sizes](#64-bit-sizes) |
| a file of an unknown type sent as `text/plain` | `application/octet-stream`, not compressed on the fly | `res.set_header("Content-Type", "text/plain")` before `send_file()`; see [below](#files-of-an-unknown-type) |
| `form::file(name, path)` of a file without extension as `text/plain`, a `.txt` as `application/octet-stream` | the other way round | `form::file(name, path, content_type)` to choose |
| a `Content-Type` set before `send_file()` replaced by the file's | kept | see [Headers set before the response](#headers-set-before-the-response) |
| headers set with `header()` dropped by `send_response()` | sent, the message's own winning | |
| `header(key, value)` to replace a header | `set_header(key, value)` | `header()` adds a field |
| a timer of your own cancelling the reads of a body | `req.set_read_deadline(deadline)`, moved as it reads | `get_body_error()` is `timeout` past it |
| copying the piece of `multipart_reader::read()` to cut it in blocks | `reader.read(max_size)` | a view, no copy |
| `dispatch()` returning before the `on_response()` hook ran | returns once it ran, within the dispatch timeout | see [Behaviour changes](#behaviour-changes-to-check-in-011) |

### A new `body_error` value

`body_error::timeout` is what `get_body_error()` gives once a read passes the deadline of `req.set_read_deadline()`. A `switch` without a `default` warns for it, and with `-Werror` stops the build:

```cpp
// 0.10
switch (req.get_body_error()) {
    case http::body_error::none: break;
    case http::body_error::too_large: /* ... */ break;
    case http::body_error::malformed:
    case http::body_error::incomplete:
    case http::body_error::bad_encoding: /* ... */ break;
}

// 0.11: one more case, or a default
switch (req.get_body_error()) {
    case http::body_error::none: break;
    case http::body_error::too_large: /* ... */ break;
    case http::body_error::malformed:
    case http::body_error::incomplete:
    case http::body_error::bad_encoding: /* ... */ break;
    case http::body_error::timeout: /* past the read deadline: 408 */ break;
}
```

A read past the deadline only happens if the handler called `set_read_deadline()`, so code that never calls it never sees the new value.

### 64-bit sizes

The maximum body size (`default_max_body_size`, `set_max_body_size()`, `get_max_body_size()`, `max_body_size()` of routes, groups and hosts, `discard_body()`), the byte limits of `multipart_limits`, `multipart_parser::part_size()` and `multipart_file::size()` are `std::uint64_t`, so a 32-bit build holds a limit over 4 GiB. On Linux x86-64 `size_t` and `std::uint64_t` are the same type, and nothing changes. Where they are not (macOS, 32-bit builds), code that names the old type may not compile:

```cpp
// 0.10
const std::optional<size_t>& limit = route.get_max_body_size();
void (http::request::*set)(size_t) = &http::request::set_max_body_size;

// 0.11
const auto& limit = route.get_max_body_size();            // std::optional<std::uint64_t>
void (http::request::*set)(std::uint64_t) = &http::request::set_max_body_size;
```

Assigning a `size_t` to these limits compiles as before. In a 32-bit build, reading one into a `size_t` narrows it: keep it as `std::uint64_t`. The count limits `max_parts` and `max_files` stay `size_t`.

### Files of an unknown type

`send_file()`, `serve_static()` and `file_server::serve()` send a file whose extension is unknown or missing as `application/octet-stream`, where 0.10 sent `text/plain`, as Node's `send` does. A browser now downloads such a file instead of showing it, and it is no longer compressed on the fly. To keep `text/plain` for a file, set the type before answering:

```cpp
// 0.10: a LICENSE file went as text/plain
res.send_file(path);

// 0.11: they go as application/octet-stream unless the handler says otherwise
res.set_header("Content-Type", "text/plain");
res.send_file(path);
```

For `serve_static()`, set it in a middleware for the paths that need it, or rename the files to an extension the library knows.

A client `form` uses the same lookup: `form::file(name, path)` sends a file without extension as `application/octet-stream` (it was `text/plain`) and a `.txt`, `.log`, `.conf` or `.text` file as `text/plain` (it was `application/octet-stream`). To send the type a server expects, give it:

```cpp
// 0.10
form.file("config", "/etc/app/settings");

// 0.11: the type of your choice
form.file("config", "/etc/app/settings", "text/plain");
```

### Headers set before the response

Two headers a handler or a middleware set before answering now reach the client, where 0.10 replaced or dropped them.

A `Content-Type` set before `send_file()` or `serve_static()` is kept, as Node's `send` keeps it, and whether the file is compressed on the fly follows that type. Code that set one and relied on the library replacing it should stop setting it:

```cpp
// 0.10: this header was replaced by the type of the file
res.header("Content-Type", "application/json");
res.send_file("/srv/report.csv");

// 0.11: it is kept, so set none to get the type of the file
res.send_file("/srv/report.csv");
```

`send_response()` sends the headers set before with `header()` or `set_header()`, as `header()` promised. The message's own headers win over those of the same name, and `Content-Length` and `Transfer-Encoding` set before are ignored. Remove a header set with `header()` that was not meant to go with that response:

```cpp
// 0.10: a header set before send_response() was dropped
res.header("Cache-Control", "no-store");   // not sent
res.send_response(message);

// 0.11: it is sent, unless the message has one of the same name; drop the call if it was not meant for this response
res.send_response(message);
```

To replace a header set before, `set_header()` replaces every field of that name, while `header()` still adds one, as `Set-Cookie` needs.

### Behaviour changes to check in 0.11

These change what a server or a client does without changing a signature (see the changelog for the details):

- **Files of an unknown type**: `application/octet-stream`, not compressed on the fly, and streamed over 1 MiB. See [Files of an unknown type](#files-of-an-unknown-type).
- **A client `form`**: a file without extension goes as `application/octet-stream`, and a `.txt`, `.log`, `.conf` or `.text` file as `text/plain`. See [Files of an unknown type](#files-of-an-unknown-type).
- **`Content-Type` before `send_file()`**: kept instead of replaced. See [Headers set before the response](#headers-set-before-the-response).
- **`send_response()`**: sends the headers set before with `header()` or `set_header()`. See [Headers set before the response](#headers-set-before-the-response).
- **`dispatch()` with an `on_response()` hook**: the dispatch completes once the hook has run, within what is left of `dispatch_options::timeout`, where 0.10 resumed the caller first. A dispatch that waits for a slow hook now takes longer. When the timeout ends with the handler still running, the hook runs when the handler ends, so what it captures must live as long as the handler. Without a hook nothing changes.

## From 0.9 to 0.10

0.10.0 replaces what servers and clients had to work around in 0.9: CORS takes options, a directory of static files can leave a path to the application, a server WebSocket closes with a code of its own and counts what it drops, and each server and client choose what their logs hide. Most code only has to compile again. What needs a closer look is what CORS sends to browsers, the logs, and the code that stops an `async_client` or destroys an application while they have work in flight.

A migration usually goes in this order:

1. Update the build: [spdlog and fmt](#spdlog-and-fmt), removing `FMT_CONSTEVAL`, and fixing the code that formats types of its own.
2. Fix what no longer compiles: `enable_cors(bool)` and the constructors of `response` ([CORS](#cors)), `file_server::serve()` and `handle_unmatched()` ([Static files](#static-files)), and the uses of `async_client` as its [base class](#async_client-is-no-longer-an-http_client_base).
3. Check what [CORS](#cors) sends now: a browser notices first, and a server with credentials has to list its origins.
4. Replace the workarounds: a wildcard route for [static files](#static-files), `release_socket()` and the inference from `queued_messages()` on a [WebSocket](#websocket-close-codes-and-dropped-messages), and the redaction of a custom logger ([Query parameters in the logs](#query-parameters-in-the-logs)).
5. Check the lifetimes: [coroutines of `async_client`](#async_client-coroutines-that-may-resume-after-the-client) stopped from a worker, and [`dispatch()` when the application is destroyed](#dispatch-when-the-application-is-destroyed). Then review the [behaviour changes](#behaviour-changes-to-check-in-010).

### Old → new

| 0.9 | 0.10 | Notes |
|---|---|---|
| `void enable_cors(bool enabled = true)` | `boost::system::error_code enable_cors(cors_options options = {})` | empty when applied; see [CORS](#cors) |
| `enable_cors()`, `enable_cors(true)` | `enable_cors()` | `enable_cors(true)` no longer compiles, and `enable_cors()` means something else |
| `enable_cors(false)` | `disable_cors()` | off is still the default |
| `Access-Control-Allow-Credentials: true` with `*` on every response | `.credentials = true` with a list or a filter | `*` with credentials is `invalid_argument` |
| `Access-Control-Allow-Methods` and `Access-Control-Allow-Headers` on every response | only on the preflights | |
| preflights ended in `404`/`405` | `204` before routing, without middlewares | `.preflight_continue = true` sends them through them |
| `router().enable_cors()` (`route_handler::enable_cors()`) | `server.enable_cors()` | removed; no route of its own |
| `Access-Control-Max-Age: 86400` of `route_handler::enable_cors()` | not sent by default | `.max_age = std::chrono::hours(24)` |
| `response(connection, stream, request, cors_enabled)`, `response(sink, request, cors_enabled)` | without the `bool` | |
| `file_server::serve(req, res, directory, options)` | `file_server::serve(req, res, directory, req["path"], options)` | returns whether it answered; the path is raw, and any parameter or string will do |
| `static_options`, `dotfiles_policy` from `virtual_host.hpp` | also from `file_server/static_files.hpp` | `virtual_host.hpp` still brings them in |
| `router.handle_unmatched(request_ptr, res)` | `router.handle_unmatched(*request_ptr, res)` | takes `request&` |
| a wildcard route of your own for rules on paths without a file | `serve_static(prefix, directory, {.fallthrough = true})` and a not-found handler | see [Static files](#static-files) |
| `http_client_base& c = async_client;`, a function taking `http_client_base&` | a template on the client | see [below](#async_client-is-no-longer-an-http_client_base) |
| `client.http_client_base::get(url)` | `client.get(url)` | on an `async_client` |
| a class deriving from `async_client` using `timeout_`, `get_connection()` or `get_io_context()` | the public methods of the client; or derive from `executor_client` or `http_client_base` | |
| `#include <condition_variable>` only through `async_client.hpp` | include it | |
| `release_socket()` + `close(code)` on the `asio::websocket` | `ws->stop(code, reason)` | from any thread; `on_close()` is called, and the queue and the keepalive are handled |
| no reason in a close frame | `stop(code, reason)`, `close_graceful(code, reason)` | up to 123 bytes of UTF-8 |
| comparing `queued_messages()` before and after each send | `ws->dropped_messages()` | from any thread, also on `sse_connection` |
| `stop()` with a peer that does not read: `1013` at the write timeout | at most 5 seconds; the code asked for, or `1006` | see [WebSocket](#websocket-close-codes-and-dropped-messages) |
| `GET /cb?code=abc` in the logs and from `loggable_uri()` | `GET /cb?code=[redacted]` | the same for the eight other names added |
| eight names, fixed | `options.logging.redact` on a server, `client.redact_parameters()` on a client | each one its own |
| `options.logging = {true, 512}` | the same | it also sets the names back to the defaults: set `redact` afterwards |
| `WebSocket connected to http://host:8080/ws?token=t` | `WebSocket connected to ws://host:8080/ws?token=[redacted]` | the scheme is the one of the WebSocket |
| reverse proxy lines and `proxy_result::error` with the query | with the path alone | |
| a `dispatch()` in flight when its application is destroyed: use-after-free | `operation_aborted` when awaited, `503` with a callback | see [`dispatch()`](#dispatch-when-the-application-is-destroyed) |
| spdlog 1.12.0 and fmt 9.1, and `FMT_CONSTEVAL` to build with clang 20 | spdlog 1.15.1 and fmt 11.1, without it | see [spdlog and fmt](#spdlog-and-fmt) |

### spdlog and fmt

thinger-http now brings spdlog 1.15.1 with fmt 11.1, where 0.9 brought spdlog 1.12.0 with fmt 9.1. `thinger/util/logger.hpp` does not change, and a log call that formats numbers, strings and other types fmt knows compiles as it did. What may stop compiling is code that formats types of its own, in a `LOG_*` call or in a `fmt::format()` of its own:

| 0.9 (fmt 9.1) | 0.10 (fmt 11.1) | The compiler says |
|---|---|---|
| `LOG_INFO("{}", value)` with an unscoped `enum`, which printed its number | `fmt::underlying(value)`, or a `format_as()` for the enum | `type_is_unformattable_for` |
| `LOG_INFO("{}", value)` with a class that converts to a number or a string | a `format_as()` for the class, or a cast in the call | `type_is_unformattable_for` |
| `fmt::formatter<T>::format()` without `const` | with `const` | `function is not marked const` |
| `fmt::join()` with `<spdlog/fmt/fmt.h>` | also `#include <spdlog/fmt/ranges.h>` | `no member named 'join'` |
| `FMT_CONSTEVAL` defined to build with clang 20 | remove it | |

**Enums and classes that convert** are no longer formatted through the conversion. A `format_as()` in the namespace of the type keeps every call as it is, with the same text:

```cpp
enum color { red, green };

// 0.9: prints 1
LOG_INFO("{}", green);

// 0.10: in the call
LOG_INFO("{}", fmt::underlying(green));

// 0.10: or once for the type, and the call stays
inline auto format_as(color c) { return fmt::underlying(c); }
LOG_INFO("{}", green);
```

```cpp
struct name {
    operator std::string_view() const;
};

// 0.9: prints the string
LOG_INFO("{}", name{});

// 0.10
inline std::string_view format_as(const name& n) { return n; }
LOG_INFO("{}", name{});
```

An `enum class` was already rejected by fmt 9.1, so there is nothing new to do for one.

**A formatter's `format()` must be `const`**:

```cpp
// 0.9
template<> struct fmt::formatter<point> : fmt::formatter<std::string> {
    auto format(const point& p, fmt::format_context& ctx) {
        return fmt::format_to(ctx.out(), "({}, {})", p.x, p.y);
    }
};

// 0.10
template<> struct fmt::formatter<point> : fmt::formatter<std::string> {
    auto format(const point& p, fmt::format_context& ctx) const {
        return fmt::format_to(ctx.out(), "({}, {})", p.x, p.y);
    }
};
```

**`fmt::join()` needs its own header**:

```cpp
// 0.9
#include <thinger/util/logger.hpp>
LOG_INFO("{}", fmt::join(values, ", "));

// 0.10
#include <thinger/util/logger.hpp>
#include <spdlog/fmt/ranges.h>
LOG_INFO("{}", fmt::join(values, ", "));
```

`fmt::underlying()`, the `const` and the new include also compile with fmt 9.1, so they can go in before the upgrade.

**An installed spdlog** is used only from 1.15.1 on; an older one is ignored and `v1.15.1` is downloaded. One of 1.15.1 or later is used with the fmt it was compiled against, and that one is your choice: built with `SPDLOG_FMT_EXTERNAL` against a fmt older than 11.1, it fails with clang 20 as fmt 9.1 does, and thinger-http is only tested with the fmt `v1.15.1` brings. To leave the installed one out and download `v1.15.1`:

```bash
cmake -DCMAKE_DISABLE_FIND_PACKAGE_spdlog=ON ..
```

A `spdlog::spdlog` target or a `FetchContent_Declare(spdlog)` your project made before adding thinger-http stays, whatever its version: to build with clang 20, it has to bring a fmt of 11.1 or later.

### CORS

`enable_cors()` takes options (`http::cors_options`) whose defaults are those of `cors()` in Express, and returns a `boost::system::error_code`, empty when applied, where it took a `bool` and returned nothing. See [CORS](README.md#cors) for every option.

**Credentials, and a list of origins.** `enable_cors()` no longer says `Access-Control-Allow-Credentials: true`. In 0.9 it said so next to `Access-Control-Allow-Origin: *`, and browsers reject that pair, so no browser could send credentials to it anyway: a cross-origin request with cookies or `credentials: 'include'` needs its origin listed, which 0.9 could not do. A page sending only `Authorization`, without `credentials: 'include'`, needs no credentials: `enable_cors()` alone allows it.

```cpp
// 0.9: credentials announced to anyone, and rejected by browsers
server.enable_cors();

// 0.10: the origin of the request, if listed, is reflected with Vary: Origin
if (auto ec = server.enable_cors({
        .origins = {"https://app.example.com", "https://admin.example.com"},
        .credentials = true,
    })) {
    // invalid_argument, e.g. .credentials = true with the default origins ("*"): nothing
    // changes and the reason is in the log
}
```

A list means that the responses to other origins, and to requests without `Origin`, have no `Access-Control-Allow-Origin`. Origins that a list cannot express go in `origin_filter`, which replaces it:

```cpp
server.enable_cors({
    .origin_filter = [](std::string_view origin, const http::http_request&) {
        return origin.ends_with(".example.com");
    },
    .credentials = true,
});
```

**`Allow-Methods` and `Allow-Headers` out of normal responses.** Only preflights have them now, which is the only place a browser reads them. The methods are `GET, HEAD, PUT, PATCH, POST, DELETE` by default, and the headers are those the preflight asks for, reflected, where 0.9 sent a fixed `Content-Type, Authorization, X-Requested-With`. To send a fixed list:

```cpp
// 0.9: on every response
// Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS, HEAD, PATCH
// Access-Control-Allow-Headers: Content-Type, Authorization, X-Requested-With
server.enable_cors();

// 0.10: on preflights, and only if the defaults are not what you want
server.enable_cors({
    .methods = {"GET", "POST", "PUT", "DELETE", "OPTIONS", "HEAD", "PATCH"},
    .allowed_headers = std::vector<std::string>{"Content-Type", "Authorization", "X-Requested-With"},
});
```

A client that reads those two headers from a normal response, a test included, must ask with a preflight. To keep them on normal responses anyway, a middleware can set them with `res.header()`.

**Preflights without middlewares.** `server.enable_cors()` now answers an `OPTIONS` with `Access-Control-Request-Method` itself, with `204`, on any path, before route matching: middlewares (authorization, rate limits, logging) and `OPTIONS` routes no longer see it, and `on_response` still does. Requests that only look like one, an `OPTIONS` without that header, are routed as before. To send preflights through the middlewares and routes again:

```cpp
// 0.9: an OPTIONS route of the application answered the preflights to its path
server.enable_cors();
server.options("/api/orders", handler).meta("permission", "orders:preflight");

// 0.10: the same route needs preflight_continue. Its response gets the preflight headers,
// and a path with no OPTIONS route ends in 404/405, or the not-found handler
server.enable_cors({.preflight_continue = true});
server.options("/api/orders", handler).meta("permission", "orders:preflight");
```

**`max_age`, to return to 86400.** `route_handler::enable_cors()` sent `Access-Control-Max-Age: 86400`. By default nothing is sent now, as in Express, and each browser applies its own default (5 seconds in the Fetch standard), so a browser sends more preflights:

```cpp
server.enable_cors({.max_age = std::chrono::hours(24)});   // 86400
```

**`route_handler::enable_cors()` is gone.** It registered a hidden `OPTIONS :path(.*)` route with no metadata, so a middleware requiring route metadata rejected the preflights, and it added no header to any other response. Its callers replace it with `enable_cors()` of the application, which needs no route:

```cpp
// 0.9
server.router().enable_cors(true);

// 0.10: also the headers of the other responses, which that call did not add
server.enable_cors();
```

A route of the application at `OPTIONS :path(.*)` still works as any other, with `preflight_continue` if it should get the preflights.

**`enable_cors(bool)`.** It no longer compiles, on purpose, so that nothing changes meaning in silence:

```cpp
// 0.9
server.enable_cors(true);
server.enable_cors(false);

// 0.10
server.enable_cors();
server.disable_cors();
```

**The constructors of `response`** have lost `bool cors_enabled`. They are used by code building a `response` by hand, e.g. with a `memory_response`; requests dispatched by the application get the CORS in force without it, and a `response` built by hand has no CORS headers:

```cpp
// 0.9
http::response res(sink, request, true);

// 0.10
http::response res(sink, request);
```

A CORS header set by a handler or a middleware now stays alone, as `res.setHeader()` after `cors()` in Express: a handler that set its own `Access-Control-Allow-Origin` needs no change, and in 0.9 its response had that header and the one of `enable_cors()`, twice.

**`Vary` in the head of `start_stream()`** now adds to the `Vary` already on the response instead of replacing it, so that CORS keeps `Origin` on streams and on `reverse_proxy` responses. Only a handler that set `Vary` with `header()` and relied on its head replacing it sees a difference: set the fields it wants in one of them.

### Static files

**`file_server::serve()` takes the path from the caller**, instead of reading a route parameter called `path`, and its header is now public. Without `fallthrough` it behaves as it did, so a call only needs the new argument:

```cpp
// 0.9
server.get("/files/:path(.*)", [options](http::request& req, http::response& res) {
    http::file_server::serve(req, res, "/var/www/files", options);
});

// 0.10
server.get("/files/:path(.*)", [options](http::request& req, http::response& res) {
    http::file_server::serve(req, res, "/var/www/files", req["path"], options);
});
```

**A wildcard route of your own is no longer needed** to decide what happens with the paths without a file. It is replaced by `serve_static()` with `fallthrough`, and the rules move to a not-found handler, which only gets what has no file to serve. Rejected paths stay out of it:

```cpp
// 0.9: a route of its own, looking for the file before serve() does
server.get("/:path(.*)", [root, options](http::request& req, http::response& res) {
    const auto& path = req["path"];
    if (!path.starts_with("app/") && !std::filesystem::is_regular_file(root / path)) {
        res.error(http::http_response::status::not_found);
        return;
    }
    http::file_server::serve(req, res, root, options);
});

// 0.10: index.html under /app/ only, and a 404 without body elsewhere
server.serve_static("/", root.string(), {.fallthrough = true});
server.set_not_found_handler([root](http::request& req, http::response& res) {
    auto method = req.get_http_request()->get_method();
    if ((method == http::method::GET || method == http::method::HEAD) &&
        req.get_http_request()->get_path().starts_with("/app/")) {
        res.send_file(root / "index.html");
        return;
    }
    res.error(http::http_response::status::not_found);
});
```

Three things differ from a wildcard route:

- A not-found handler gets every method without a route, where a `GET` route only gets `GET` and `HEAD`, and the `405` of the other methods is not sent for it: check the method, and answer the others as you need.
- It is not a route, so do not read the path from a route parameter: use `get_path()`, the whole path of the request, with its prefix and raw.
- With `fallthrough`, the fallback is only the index of a directory: a path without a file never gets the `index.html` of the root unless the handler sends it.

To keep a route (a prefix among others, another parameter name), call `file_server::serve()` with `.fallthrough = true` from it, and answer when it returns `false`. See [Serving Files from a Handler](README.md#serving-files-from-a-handler).

### `async_client` is no longer an `http_client_base`

Its public methods have not changed, the setters included, which still return `http_client_base&`, so code calling them on an `async_client` compiles as it is. What treats it as its base class does not:

```cpp
// 0.9
void configure(http::http_client_base& client) {
    client.timeout(std::chrono::seconds(5)).verify_ssl(false);
}

// 0.10
template<typename Client>
void configure(Client& client) {
    client.timeout(std::chrono::seconds(5)).verify_ssl(false);
}
```

### `async_client`: coroutines that may resume after the client

Stopping or destroying an `async_client` from a thread that is not one of the workers needs no change: once the call returns, no callback or coroutine of the client runs. Called from a worker (a callback of the client that stops or destroys it), the callbacks do not change either, but a coroutine of `run()`, or a callback that is a coroutine, may resume after the call returned. It sees `operation_aborted`, or the correct response of a request that completed before the cancellation reached its worker, and its frame is destroyed on its worker. In 0.9 it never resumed, and a step of it running on another worker was waited for, so it could use whatever died with the client. The same goes for a coroutine that ignores the cancellation for more than 1 s, which 0.9 dropped at its next resumption, wherever the call came from.

```cpp
// 0.9: the session could go right after the client, even from a callback of the client
struct session {
    http::async_client client;
    int done = 0;
};

session->client.run([s = session.get(), url]() -> awaitable<void> {
    auto res = co_await s->client.get(url);
    ++s->done;  // in 0.10 it resumes here with the session destroyed
});
session->client.get(close_url, [&session](http::client_response&) { session.reset(); });
```

One way is to capture by `std::shared_ptr` what the coroutine uses, the client included: the requests keep alive what they use of it, not the `async_client` object. A reference to it is not enough, even if the coroutine stops at the first request aborted, as on another worker it may be using the client at the moment the callback destroys it. The callback calls `stop()` and releases its reference, and the client goes with the last one holding it, on a worker. The workers still have to outlive it, and destroying them with a coroutine still holding it is a memory error: before stopping them, keep a reference on that thread and call `stop()` there, which waits for those coroutines:

```cpp
// 0.10
auto client = std::make_shared<http::async_client>(workers);
auto done = std::make_shared<std::atomic<int>>(0);
client->run([client, done, url]() -> awaitable<void> {
    auto res = co_await client->get(url);
    if (res.ok()) ++*done;  // a response that completed before the cancellation reached it
});
client->get(close_url, [client](http::client_response&) { client->stop(); });
client.reset();  // it goes with the last request holding it
```

The other is to stop it from a thread that is not one of the workers, which waits for the coroutines as in 0.9. The callback only tells that thread, or calls `stop()`, which waits for nothing on a worker: the destructor called later from that thread still waits for the coroutines it cancelled.

```cpp
// 0.10
std::promise<void> finished;
session->client.get(close_url, [&finished](http::client_response&) { finished.set_value(); });
finished.get_future().wait();
session.reset();  // returns once its coroutines ended: what they use may go now
```

Work spawned on the executor of a coroutine of `run()` (`co_spawn(co_await this_coro::executor, …)`) is no longer dropped when the client stops: that executor is now its worker's, so that work goes on as any other of the application. To have it cancelled with the client, start it with `client.run()`.

### WebSocket close codes and dropped messages

`websocket_connection` closes with a code and a reason of your own, and counts what its queue drops. Both replace workarounds; nothing has to change for code that does not use them, except for what `stop()` does with a peer that does not read, at the end of this section. See [WebSocket Close](README.md#websocket-close) and [WebSocket Flow Control](README.md#websocket-flow-control).

**From `release_socket()` to `stop(code, reason)`.** After `release_socket()` nothing calls `on_close()`, closes the queue or tells the producers waiting, and the keepalive and the writes in flight are in the hands of the caller. `stop(code, reason)` does the close the library does in any other case:

```cpp
// 0.9: on the thread of the connection, repeating by hand what on_close() does
ws->execute([ws, &subscribers, id] {
    auto socket = std::dynamic_pointer_cast<thinger::asio::websocket>(ws->release_socket());
    socket->close(1008);
    subscribers.remove(id);
});

// 0.10: from any thread; the code in on_close() runs for this close as for any other
ws->on_close([&subscribers, id](const http::websocket_close&) { subscribers.remove(id); });
ws->stop(1008, "token expired");
```

`release_socket()` stays as it was, to hand the socket to another handler. Two differences with `close(code)` to check:

- `close(code)` sends a code it may not send as `1000`, while `stop(code, reason)` throws `std::invalid_argument`, as it does with a reason above 123 bytes or not valid UTF-8. A code that comes from outside can be checked first with `thinger::asio::websocket::valid_close_code(code)`.
- `on_close()` gets the code of the client's answer, normally its echo, and the reason of its frame, not the one given to `stop()`. If the client does not answer it gets the code sent, and `1006` if the close frame was not written whole.

**From `queued_messages()` to `dropped_messages()`.** The comparison was exact only on the thread of the connection, as a send from another thread hops to it. The counter needs no comparison, and any thread can read it:

```cpp
// 0.9: on the thread of the connection, around each send
ws->execute([ws, &dropped, message = std::move(message)]() mutable {
    auto before = ws->queued_messages();
    ws->send_binary(std::move(message));
    if (ws->queued_messages() <= before) ++dropped;
});

// 0.10: send from any thread, and read the count when needed
ws->send_binary(std::move(message));
metrics.set("websocket_dropped", ws->dropped_messages());
```

A send from another thread counts once the thread of the connection took it: to read the final value, go through `execute()` after the last send. It counts what `drop_newest` and `drop_oldest` dropped while the connection was open, not what was queued when it closed. On `sse_connection` the unit is the one of its `queued_messages()`: each field dropped counts one.

**`stop()` with a peer that does not read.** `stop()`, with or without arguments, and the close of a server shutdown now end within 5 seconds of the call (`asio::websocket::CLOSE_TIMEOUT`), whatever the peer does. In 0.9 a close frame behind a stalled write waited for the `write_timeout` (60 s by default, up to 75 s, and with no limit if `0`), and the connection ended in `1013`. To check:

- A large message being written when `stop()` is called is cut at the deadline, even to a slow client that keeps reading, where 0.9 went on while the client took bytes. Await the send to get it out first:

  ```cpp
  // 0.9: stop() let the message being written finish, however long it took
  ws->send_binary(std::move(report));
  ws->stop();

  // 0.10: wait until it is written, then close
  if (co_await ws->send_binary_async(std::move(report))) ws->stop();
  ```

- `on_close()` no longer gets `1013` after `stop()`: it gets the code of the answer, the code sent, or `1006` if the close frame was not written whole. Code that looks for `1013` to tell a client that stopped reading still gets it without `stop()`, from the `write_timeout` and `overflow_policy::close`.
- A client that ends the connection without answering the close frame now reports the code sent, not `1006`, so `1006` after `stop()` means that the frame did not get out.

### Query parameters in the logs

No signature changes: `loggable_uri(uri)` is still there, and `options.logging = {true, 512}` still compiles. What changes is what the logs show.

**The default set grows.** To `access_token`, `authorization`, `token`, `api_key`, `apikey`, `key`, `password` and `client_secret` join `code`, `id_token`, `refresh_token`, `secret`, `sig`, `signature`, `x-amz-security-token`, `x-amz-signature` and `x-goog-signature`. A log search or a test that reads one of their values now finds `[redacted]`. Nothing else in a URI changes.

**To see a name again**, start from the defaults and remove it:

```cpp
// 0.9: code was logged, with no configuration
server.set_connection_options(options);

// 0.10: on a server, in its connection options, before listen()
options.logging.redact.remove("code");
server.set_connection_options(options);

// 0.10: on a client, an async_client too
auto names = http::util::url::redacted_parameters::defaults();
names.remove("code");
client.redact_parameters(names);
```

**To hide a name of your own**, add it, and drop what redacted it in your logger:

```cpp
// 0.9: a custom logger or sink that masks invite= in each line
thinger::logging::set_logger(redacting_logger);

// 0.10: the defaults and one more
options.logging.redact.add("invite");
server.set_connection_options(options);

auto names = http::util::url::redacted_parameters::defaults();
names.add("invite");
client.redact_parameters(names);
```

A set assigned replaces the defaults, so `options.logging.redact = {"invite"}` logs `code`; start from the defaults unless that is what you want. To keep exactly the names of 0.9, set `{"access_token", "authorization", "token", "api_key", "apikey", "key", "password", "client_secret"}`. `{}` redacts no parameter. The password of the user info of a URL is redacted in every case.

Your own calls to `loggable_uri(uri)` use the defaults; `loggable_uri(uri, names)` takes the set of your server or client.

### `dispatch()` when the application is destroyed

In 0.9 an application had to outlive its in-memory dispatches: destroying it with one in flight was a use-after-free. The destructor now ends them without waiting, and no signature changes. Code that already kept the application alive sees nothing new. Code that may destroy it first sees one more way for a dispatch to end, from any thread or from one of its handlers:

```cpp
// 0.9: the application had to outlive this await
auto response = co_await application->dispatch(request);

// 0.10: when awaited, a dispatch without a complete response throws operation_aborted
try {
    auto response = co_await application->dispatch(request);
} catch (const boost::system::system_error& e) {
    if (e.code() != boost::asio::error::operation_aborted) throw;
    // the application is gone, or this coroutine was cancelled
}

// 0.10: a callback has no error to report, and is called once with 503
application->dispatch(executor, request, [](std::shared_ptr<http::http_response> response) {
    if (response->get_status() == http::http_response::status::service_unavailable) {
        // the application is gone, unless a handler answered 503 itself
    }
});
```

A response already complete is delivered, and a dispatch not started yet runs no middleware or handler. A handler running is interrupted with `cancel_reason::shutdown`, whatever its route declared, and what it sends afterwards is dropped. What it captured itself is still yours to keep alive. See [In-Memory Dispatch](README.md#in-memory-dispatch).

The same interruption reaches further on a server. A handler that an earlier reason let go on, such as a timeout on a route that does not declare it, is now interrupted when the server reaches its shutdown deadline, is stopped or is destroyed, while its connection still follows the request. In 0.9 it ran on. A handler that must finish its work disables its cancellation for that work:

```cpp
// 0.9: after its timeout, nothing interrupted this handler
server.get("/report", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
    co_await store.save(record);
}).timeout(10s);

// 0.10: to go on through a shutdown, as any handler that must not be interrupted
server.get("/report", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
    co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
    co_await store.save(record);
}).timeout(10s);
```

`req.cancelled()` keeps the first reason, so it says `timeout` in that case. See [Cancellation](README.md#cancellation).

### Behaviour changes to check in 0.10

These change what a server or a client does without changing a signature (see the changelog for the details):

- **CORS without credentials or fixed lists**: `enable_cors()` no longer sends `Access-Control-Allow-Credentials`, nor `Access-Control-Allow-Methods` and `Access-Control-Allow-Headers` on responses that are not preflights. See [CORS](#cors).
- **Preflights**: answered with `204` before routing, without running middlewares or handlers. `preflight_continue` restores the normal path. See [CORS](#cors).
- **CORS headers of a handler**: a header set by a handler or a middleware replaces the one of `enable_cors()`, and the `Vary` of a head given to `start_stream()` adds to the one on the response. See [CORS](#cors).
- **Redaction**: `code` and eight more names show as `[redacted]` in the logs and from `loggable_uri()`; the WebSocket lines of a client show `ws://` or `wss://`, and the lines of the reverse proxy and `proxy_result::error` the path alone. See [Query parameters in the logs](#query-parameters-in-the-logs).
- **`stop()` of a WebSocket**: ends within 5 seconds of the call, and cuts a message still being written at the deadline. See [WebSocket](#websocket-close-codes-and-dropped-messages).
- **`on_close()` after `stop()`**: reports the code sent when the peer ends without answering, not `1006`; `1006` when the frame was not written whole, never `1013`. See [WebSocket](#websocket-close-codes-and-dropped-messages).
- **`close_graceful()`, and `websocket_client::close_async()`**: one deadline of `asio::websocket::CLOSE_TIMEOUT` from the first call, the wait for the turn to write included. A later call does not start it over, and a close frame not written whole by then is reported as `1006`. A long write that must get out goes before the close: await it first.
- **`dispatch()`**: the dispatches in flight end when the application is destroyed, with `operation_aborted` or `503`. See [`dispatch()`](#dispatch-when-the-application-is-destroyed).
- **Handlers at shutdown**: a handler that an earlier reason let go on is interrupted by a shutdown deadline, `stop()` or the destructor while its connection follows the request. See [`dispatch()`](#dispatch-when-the-application-is-destroyed).
- **`async_client` coroutines**: after `stop()` or the destructor called from a worker, a coroutine may resume after the call returned; one that ignores the cancellation for 1 s is left behind and ends by itself instead of being dropped; and `run()` coroutines run on the executor of their worker, so `asio::io_context_of()` finds it and work spawned on it is the application's. See [`async_client`](#async_client-coroutines-that-may-resume-after-the-client).
- **An installed spdlog**: older than 1.15.1, it is ignored and `v1.15.1` is downloaded. See [spdlog and fmt](#spdlog-and-fmt).
- **A Unix listener stopping**: it removes its socket file before closing its acceptor, so a client connecting at that moment may get `ENOENT` where it got `ECONNREFUSED`. Code that retries on one should retry on both.

## From 0.8 to 0.9

0.9.0 makes the configuration an object the application owns and states what happens to a request during its life. There is no process-wide state left: the certificate manager and the workers are objects the application creates, one server serves HTTP and HTTPS, the settings of the connections are per server, a shutdown has one deadline, and a handler learns why its request is no longer awaited. There are no deprecation wrappers: the old names are gone, so the compiler lists every place to change.

A migration usually goes in this order:

1. Create the [`certificate_manager`](#the-certificate-manager) and the [`workers`](#workers-pool_server-and-async_client) the application owns, and pass them to what used them implicitly.
2. Serve [HTTP and HTTPS from one server](#http-and-https-from-one-server), with one `listen()` per listener, and move the connection settings to `connection_options`.
3. Replace `begin_shutdown()` with [`shutdown(by)`](#shutdown), and the signal handling of the workers with `wait()`, which returns the signal.
4. Move the [upgrades](#websocket-upgrades) to their options types, and [connections taken over](#connections-taken-over) to `server_scope`.
5. Review the [behaviour changes](#behaviour-changes-to-check) B1–B11, and declare [`cancel_on()`](#cancellation-opt-in-per-route) on the routes whose handlers should stop.

Watch for the return values whose **meaning is inverted**: `listen()`, `workers::start()` and the setters of `certificate_manager` returned a `bool` (true on success) and now return a `boost::system::error_code`, empty on success. `if (server.listen(host, port))` compiles in both versions, and in 0.9 means that it failed.

### Old → new

| 0.8 | 0.9 | Notes |
|---|---|---|
| `asio::certificate_manager::instance()` | `auto manager = std::make_shared<asio::certificate_manager>();` | one per set of certificates; give it to a listener in `listener_options::tls` |
| `bool` setters of `certificate_manager` (`set_certificate`, `set_default_certificate(cert, key)`, `set_tls_options`, `set_client_verify`, `set_server_ciphers`) | return `boost::system::error_code` | inverted meaning; `invalid_argument` on failure |
| — | `certificate_manager::remove_default_certificate()` | a manager without a default certificate: `listen()` returns `invalid_argument` |
| `set_certificate(pattern, …)`: a name with a `.` was a regex | host names and `*.domain` only, case-insensitive; other names `invalid_argument` | see [The certificate manager](#the-certificate-manager) |
| `remove_certificate()` always `true` | `false` if no certificate had that name | |
| HTTPS requests served whatever their `Host` and SNI | `421 Misdirected Request` when the `Host` gets another client certificate mode or certificate entry than the SNI name | see [The certificate manager](#the-certificate-manager) |
| `set_default_certificate(context)`, returning nothing; `nullptr` dropped the default | returns `boost::system::error_code`: `invalid_argument` for `nullptr`, which keeps the default | drop the default with `remove_default_certificate()` (no self-signed one is generated afterwards) |
| `tcp_socket_server`, `unix_socket_server`, `socket_server_base`, `basic_socket_server<P>`, `socket_server`, `io_context_provider` | `asio::listener`, `asio::listener_options`, `asio::executor_provider` | see [A listener of your own](#a-listener-of-your-own) |
| `set_handler(h)`, then `bool start()` | `error_code open()`, then `start(h)` | `stop()` returns `void`; `get_service_name()` → `url()` |
| `enable_ssl(true, verify)`, `set_ssl_context()`, `set_handshake_timeout()`, `set_tcp_no_delay()`, `set_max_listening_attempts()`, `set_allowed_remotes()`, `set_forbidden_remotes()` on a socket server | the fields `tls`, `client_verify`, `handshake_timeout`, `tcp_no_delay`, `max_listening_attempts`, `allowed_remotes`, `forbidden_remotes` of `listener_options` | a context of its own goes in the manager: `set_default_certificate(context)` |
| `asio::get_workers()` | an `asio::workers` the application creates, `start()`s and `stop()`s | nothing starts or stops by itself |
| `pool_server()`, `async_client()` | `pool_server(workers&)`, `async_client(workers&)` | no default constructor |
| `worker_client`, `register_client()`, `unregister_client()`, `client_count()` | removed | stop the servers and clients before `workers.stop()` |
| `workers::set_auto_manage()`, `is_auto_managed()` | removed | |
| `workers::get_isolated_io_context(name)` → `io_context&` | `workers::isolated_executor(name)` → `any_io_executor` | `post()` or `co_spawn()` on it |
| `void workers::wait(signals)` (stopped the workers) | `int workers::wait(signals)` (returns the signal; stops nothing) | then shut the servers down and call `workers.stop()` |
| `workers::wait_graceful(grace, signals)` | `wait()`, then `server.shutdown(grace)`, then `workers.stop()` | |
| `bool workers::start(n)` | `error_code workers::start(n)` | inverted meaning; `start(0)` is `invalid_argument` |
| `bool workers::stop()` | `void workers::stop()` | `running()` tells it before the call |
| `http_server_base` (`http/server/http_server_base.hpp`) | `server_base` (`http/server/server_base.hpp`) | no alias |
| `server.enable_ssl(true); server.listen(host, port);` | `server.listen(asio::listener_options{.endpoint = asio::tcp_endpoint{host, port}, .tls = manager})` | one `listen()` per listener (B1) |
| one server per listener (an HTTPS port needed a second server with a copy of the routes) | one server, several `listen()` calls | |
| `bool listen(host, port)` | `error_code listen(host, port)` | inverted meaning |
| `listen_unix(path)`, `start_unix(path)` | `listen(options)` with `options.endpoint = asio::unix_endpoint{path}` | |
| `set_connection_timeout(t)` | `connection_options::idle_timeout` | `server.set_connection_options(options)`, before `listen()` |
| static `set_lingering_close(max_bytes, timeout)` (every server of the process) | `connection_options::linger_max_bytes`, `linger_timeout` (per server) | |
| `log_bodies(enabled, max_length)`, `get_body_logging()` | `connection_options::logging` | the client keeps its `log_bodies()` |
| `set_max_listening_attempts(n)` on a server | `listener_options::max_listening_attempts` | per listener |
| `local_port()` on a server | `listening_on()` | one URL per listener, with its port |
| `create_socket_server()`, `create_unix_socket_server()` (protected) | `create_listener(listener_options)` | for classes deriving from `server_base` |
| `begin_shutdown(grace, on_done)` | `co_await shutdown(by)`, or `co_spawn(executor, server.shutdown(by), [](std::exception_ptr, bool clean) {...})` | `by = steady_clock::now() + grace` |
| `bool shutdown(grace)` | unchanged, blocking, for a thread that does not serve the server | |
| `bool stop()`, which only stopped listening | `void stop()`, which also closes the connections open and cancels their handlers | `is_listening()` tells it before the call; `stop_listening()` to stop accepting only (B10) |
| `stop_listening()` (private) | public | quiesce, before `shutdown(by)` |
| a route timeout answers `504` and lets the handler run | the same, and the handler finds `req.cancelled() == cancel_reason::timeout`; `.cancel_on({cancel_reason::timeout})` interrupts it | B2 |
| an in-memory dispatch timeout interrupts every handler | interrupts only a route declaring `cancel_on({timeout})` | B4 |
| a half-close (FIN) after the request counts as the client leaving | the response is written, then the connection closes | B5 |
| — | `route::cancel_on()`, `request::cancelled()`, `request::client_gone()`, `request::cancellation_slot()` | see [Cancellation](README.md#cancellation) |
| `awaitable<bool> socket::peer_closed()` | `awaitable<error_code> socket::peer_closed()` | `eof` for a FIN, the error for a reset; `if (co_await s.peer_closed())` keeps its meaning |
| `sse_connection::handle_write(type, value)` (public) | private: `send_event()`, `send_id()`, `send_retry()`, `send_data()`, `send(sse_event)`, `send_async(sse_event)`, `send_comment()` | |
| `upgrade_websocket(handler, std::set<std::string> protocols = {})` | `upgrade_websocket(const websocket_accept_options&, handler)` | the options first: `upgrade_websocket({}, h)` |
| `upgrade_websocket_stream(websocket_options, (shared_ptr<asio::websocket>, std::string protocol))`, and the forms without options | `upgrade_websocket_stream(const websocket_accept_options&, (http::upgraded_websocket))` | `upgraded.stream` is still a `shared_ptr<asio::websocket>` |
| `http::websocket_options` | `http::websocket_accept_options` | `protocols` is a `std::vector`; `ping_interval`, `idle_timeout` are `std::optional` |
| `start_sse(handler)` | `start_sse(handler, const sse_options& = {})` | the old form compiles unchanged |
| `take_over((shared_ptr<asio::socket>, std::string buffered))` | `take_over((http::upgraded_connection))` | `conn.socket`, `conn.buffered`, `conn.scope` |
| `http::on_server_shutdown(socket, cb)` | `scope.on_shutdown(cb)` | the scope of the `upgraded_connection` or `upgraded_websocket` |
| `http::set_connection_type(socket, type)`, `wrap_connection(socket, wrapper, type)` | `scope.set_type(type)` | |
| `socket::set_attachment()`, `get_attachment()`, `asio::socket_attachment`, `http::taken_over_connection` | removed | keep what lives with the socket next to it (the handler's capture) |
| `request::get_socket()`, `get_http_connection()`, `read_ahead_available()` | `req.connection()` (`peer_ip`, `peer_port`, `local_port`, `secure`), `req.tls()`, `http::connection_sink(req)` | no `request` member reaches a socket |
| `http_stream(id, bool keep_alive)`, `keep_alive()`, `set_keep_alive()` | `http_stream(id, request, executor)`, `disposition()`, `must_close(reason)` | for code driving streams itself (sinks, tests) |
| — | `http_frame::kind()`, pure virtual | a class derived from `http_frame` defines it |
| `response_sink::take_over(response, handler)` | `take_over(response, handler, body_framing)` | a sink derived by the application overrides it |
| `body_reader::source` → `awaitable<size_t>`; `body_reader::read_ahead_available()` | `awaitable<io_result>`; `buffered()` | |
| `response.hpp` included the connection, stream, sink, WebSocket and SSE headers | it declares them; include their headers, or `thinger/http_server.hpp` | |

### The certificate manager

Before:

```cpp
auto& manager = thinger::asio::certificate_manager::instance();
if (!manager.set_certificate("api.example.com", certificate, key)) { /* failed */ }
server.enable_ssl(true);
server.start("0.0.0.0", 443);
```

After:

```cpp
auto manager = std::make_shared<thinger::asio::certificate_manager>();
if (auto ec = manager->set_certificate("api.example.com", certificate, key)) { /* failed: invalid_argument */ }

thinger::asio::listener_options https;
https.endpoint = thinger::asio::tcp_endpoint{"0.0.0.0", 443};
https.tls = manager;
if (auto ec = server.listen(https)) { /* failed */ }
server.wait();
```

Keep the manager where the code that reached `instance()` can reach it (the object that owns the servers). Each manager generates its own self-signed default certificate on first use when none is set; `remove_default_certificate()` turns that off.

**Breaking:** `set_certificate()` takes host names only: an exact name, or `*.domain` for one more label, both matched case-insensitively. 0.8 took any name with a `.` as a regex, so `api.example.com` also matched `apiXexample.com`, and a key such as `^api\..*\.com$` worked as a pattern. Such names are now refused with `invalid_argument`, and nothing is stored: register each host name, or a wildcard. `remove_certificate()` returns `false` when no certificate is registered with that name, where it always returned `true`.

**Breaking:** an HTTPS request whose `Host` would get another TLS configuration than the SNI name of its connection is answered `421 Misdirected Request`, before any middleware or handler runs. The configuration of a name is its client certificate mode (`set_client_verify()`, else the listener's) and the certificate entry that serves it (exact name, wildcard or default). In 0.8 the request was served by the virtual host of its `Host`, so a client could reach a host that requires a certificate through the handshake of another one. Requests whose `Host` is the SNI name, names sharing one configuration (two names of a wildcard, two of the default certificate), and clients without SNI whose `Host` has the listener's mode are served as before. A certificate registered for a host name is an entry of its own even when it is also the default certificate: register a certificate meant for any `Host` as the default only. A client that reuses one connection for hosts with different certificates or modes gets the 421 and must open a connection per host, as browsers do on a 421; a proxy in front of the server should send the `Host` as SNI (nginx: `proxy_ssl_server_name on`), or no SNI.

### HTTP and HTTPS from one server

Before, an HTTPS port was a second server, with the routes added to both:

```cpp
thinger::http::pool_server http_server, https_server;
add_routes(http_server);
add_routes(https_server);
http_server.set_connection_timeout(std::chrono::seconds(60));
https_server.set_connection_timeout(std::chrono::seconds(60));
thinger::http::pool_server::set_lingering_close(1024 * 1024, std::chrono::seconds(2));  // process-wide
https_server.enable_ssl(true);
http_server.listen("0.0.0.0", 80);
https_server.listen("0.0.0.0", 443);
```

After, one server and one set of routes, middlewares and hooks:

```cpp
thinger::http::pool_server server(workers);
add_routes(server);

thinger::http::connection_options options;
options.idle_timeout = std::chrono::seconds(60);
options.linger_max_bytes = 1024 * 1024;
options.linger_timeout = std::chrono::seconds(2);
server.set_connection_options(options);                 // this server only, before listen()

if (auto ec = server.listen("0.0.0.0", 80)) { /* failed */ }
thinger::asio::listener_options https;
https.endpoint = thinger::asio::tcp_endpoint{"0.0.0.0", 443};
https.tls = manager;
if (auto ec = server.listen(https)) { /* failed */ }

server.get("/late", handler);                            // before any request arrives: on both listeners (B1)
```

### Workers, `pool_server` and `async_client`

Before, the process-wide workers started with the first `pool_server` or `async_client`, and `wait()` or `wait_graceful()` stopped everything on a signal:

```cpp
thinger::http::pool_server server;
thinger::http::async_client client;
server.listen("0.0.0.0", 8080);
thinger::asio::get_workers().wait_graceful(std::chrono::seconds(30));
```

After, the application owns the workers and decides what a signal does:

```cpp
thinger::asio::workers workers;
if (auto ec = workers.start()) { /* already_started, or invalid_argument for 0 threads */ }

{
    thinger::http::pool_server server(workers);
    thinger::http::async_client client(workers);
    server.listen("0.0.0.0", 8080);

    int signal = workers.wait();                         // SIGINT, SIGTERM or SIGQUIT; stops nothing
    bool clean = server.shutdown(std::chrono::seconds(30));
    client.stop();
}                                                        // servers and clients go first
workers.stop();
```

Keep the workers alive longer than the servers and clients built on them. `workers.get_isolated_io_context("name")` becomes `workers.isolated_executor("name")`, an executor to `post()` or `co_spawn()` on. `in_worker_thread()` answers from a thread-local, for that `workers` only: code that needed a counter or a registry to tell whether it runs on an IO thread asks it.

### A listener of your own

Before, a protocol served on its own port with a socket server:

```cpp
thinger::asio::tcp_socket_server listener("0.0.0.0", "8883",
    [&]() -> boost::asio::io_context& { return io; },
    [&]() -> boost::asio::io_context& { return io; });
listener.enable_ssl(true, verify);
listener.set_handler([](std::shared_ptr<thinger::asio::socket> socket) { serve(socket); });
if (!listener.start()) { /* failed */ }
```

After:

```cpp
auto on_io = [&]() -> boost::asio::any_io_executor { return io.get_executor(); };

thinger::asio::listener_options options;
options.endpoint = thinger::asio::tcp_endpoint{"0.0.0.0", 8883};   // the port is a number
options.tls = manager;                                              // null: plain TCP
options.client_verify = verify;

thinger::asio::listener listener(options, on_io, on_io);             // or workers.provider()
if (auto ec = listener.open()) { /* invalid_argument, already_open or the error binding */ }
listener.start([](std::shared_ptr<thinger::asio::socket> socket) { serve(socket); });
```

A host name with a mode of its own on the manager (`set_client_verify(host, mode)`) overrides the listener's `client_verify` for that name; every other handshake gets the listener's. `max_pending_handshakes` (unbounded by default) and `session_cache` (on, 20,480 sessions) keep 0.8's behaviour unless set.

### Shutdown

Before:

```cpp
signals.async_wait([&](auto, int) {
    server.begin_shutdown(std::chrono::seconds(30), [](bool clean) { /* ... */ });
});
```

After, one absolute deadline for the whole shutdown:

```cpp
signals.async_wait([&](auto, int) {
    auto by = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    co_spawn(io_context, server.shutdown(by), [](std::exception_ptr, bool clean) { /* ... */ });
});
```

From a coroutine, `bool clean = co_await server.shutdown(by);`, on any thread, a thread of the server included. `stop_listening()` closes the listeners alone, and `stop()` releases everything at once: it closes every connection still open and cancels the handlers still running, without waiting for them (B10), where 0.8 left the connections open. At `by` every handler still running is interrupted (B3) and the connections still open are closed; `shutdown(by)` returns `false` then, without waiting for those handlers.

### WebSocket upgrades

Before:

```cpp
res.upgrade_websocket([](std::shared_ptr<http::websocket_connection> ws) { /* ... */ }, {"chat", "json"});

res.upgrade_websocket_stream({.protocols = {"mqtt"}},
    [](std::shared_ptr<thinger::asio::websocket> ws, std::string protocol) { serve(ws, protocol); });
```

After:

```cpp
res.upgrade_websocket({.protocols = {"chat", "json"}}, [](std::shared_ptr<http::websocket_connection> ws) { /* ... */ });

res.upgrade_websocket_stream({.protocols = {"mqtt"}}, [](http::upgraded_websocket upgraded) {
    serve(upgraded.stream, upgraded.protocol);
    upgraded.scope.on_shutdown([ws = upgraded.stream] { ws->close(); });   // was on_server_shutdown()
});
```

`websocket_accept_options` also sets `max_message_size` (16 MiB), `ping_interval` and `idle_timeout` (unset: 60 s and 120 s for the message interface, off for a byte stream; 0 turns each off), which were fixed before. With the default options both upgrades behave as in 0.8, but for the write timeout of the message interface (B11).

### Connections taken over

Before:

```cpp
res.take_over([](std::shared_ptr<thinger::asio::socket> socket, std::string buffered) {
    socket->set_attachment(std::make_shared<session>());
    http::on_server_shutdown(*socket, [socket] { socket->close(); });
    run(socket, std::move(buffered));
});
```

After:

```cpp
res.take_over([](http::upgraded_connection conn) {
    auto state = std::make_shared<session>();                       // kept with the socket by the code using it
    conn.scope.on_shutdown([socket = conn.socket] { socket->close(); });
    run(conn.socket, std::move(conn.buffered), state);
});
```

`scope.set_type(type)` replaces `set_connection_type()` and `wrap_connection()`, and `scope.closed()` says the connection no longer counts in `stats()`.

### Server-Sent Events

`handle_write(type, value)` is private. Send each field with its method, or the whole event at once:

```cpp
// Before
sse->handle_write("event", "update");
sse->handle_write("data", payload);

// After
sse->send(http::sse_event{.event = "update", .data = payload});
bool written = co_await sse->send_async(http::sse_event{.data = payload});   // waits until written
```

`start_sse(handler, http::sse_options{.keepalive = std::chrono::seconds(30)})` sets the keep-alive interval; `start_sse(handler)` compiles and behaves as before, but for its write timeout (B11).

### The connection of a request

```cpp
// Before
auto port = req.get_socket()->get_remote_port();
auto tls = req.get_socket()->tls();
http::connection_sink sink(req.get_http_connection(), req.get_http_stream());

// After
auto port = req.connection().peer_port;
auto tls = req.tls();
http::connection_sink sink(req);
```

For timers and sockets of its own, a coroutine handler uses its executor: `co_await boost::asio::this_coro::executor`.

### `peer_closed()`

```cpp
// Before
if (co_await socket->peer_closed()) { /* FIN or reset */ }

// After
auto ec = co_await socket->peer_closed();
if (ec == boost::asio::error::eof) { /* the peer closed its sending side (FIN) */ }
else if (ec) { /* reset, or another error */ }
```

A socket defined by the application that overrides `peer_closed()` changes its signature; the base class now returns `operation_not_supported`.

### Cancellation opt-in per route

Nothing interrupts a handler unless its route says so, but at the drain deadline. A long poll that should stop when its client leaves or its timeout expires declares it:

```cpp
server.get("/v1/events", long_poll)
    .timeout(std::chrono::seconds(60))
    .cancel_on({http::cancel_reason::client_gone, http::cancel_reason::timeout});
```

Its pending await ends with `operation_aborted`. On other routes the handler finds the reason in `req.cancelled()`, can await `req.client_gone()` alongside its work, or binds `req.cancellation_slot()` to the waits it can abandon. Work that must not be interrupted on a `cancel_on` route runs outside the handler's cancellation state (see [Cancellation](README.md#cancellation)).

### Behaviour changes to check

These change what a server or a client does without changing a signature (see the changelog for the details):

- **B1**: one server serves its application on every listener. A route, middleware, virtual host or hook answers on all of them; HTTP and HTTPS share one set of routes. Add them before any request arrives (before `listen()`, or after it but before a request comes): requests read them without locks, so changing them while serving is a data race.
- **B2**: route cancellation is opt-in. A route timeout still answers `504` and lets the handler run, which now finds `cancelled() == timeout`; a route declaring `cancel_on({timeout})` is interrupted instead.
- **B3**: at the drain deadline, every handler still running is interrupted (`cancel_reason::shutdown`), whatever its route declared, before its socket closes. A handler that must finish its work runs it outside its cancellation state.
- **B4**: an in-memory dispatch timeout no longer interrupts a handler whose route did not declare `cancel_on({timeout})`; `dispatch()` still returns `504`. A caller cancelling the dispatch is `client_gone`.
- **B5**: a client that closes its sending side (FIN) after a complete request gets its response, with `Connection: close`. Tests that ended a client with a FIN to make the server notice it leaving reset the connection instead.
- **B6**: a client WebSocket has a keepalive by default, the server's: it pings after 60 s with nothing received and closes (1001) after 120 s. A 0.8 client never pinged, and waited for ever on a dead peer. Change it, or turn it off with `(0, 0)`, with `websocket_client::set_keepalive()`. The synchronous calls of `websocket_client` and `http::client` return once their own operation completes, instead of running the `io_context` until it has no work.
- **B7**: `async_client::stop()` ends the requests in flight before returning, as the destructor does: it cancels them, and their callbacks are not called (the coroutines of `run()` see `operation_aborted`). Code that called `stop()` and then waited for the callbacks of its requests calls `wait()` first.
- **B8**: the end of a shutdown no longer calls the virtual `stop()`. A subclass of `pool_server` or `server` that overrides `stop()` to release its own resources at the end of a shutdown releases them once `shutdown()` or `wait()` returned, or in its destructor.
- **B9**: **Breaking:** a server connection has three deadlines on by default (see [Connection Options](README.md#connection-options)), where 0.8 had only the idle timeout, which every byte received moved. `connection_options::header_timeout` (60 s): a request head not complete within it from the start of the request is answered `408 Request Timeout` and the connection closed, and a connection that sends no byte of its first request is closed without a response. `request_timeout` (300 s): a request whose body was not read to its end within it is answered `408` (or its connection closed if its response started), and its handler finds `cancelled() == timeout`; what the handler reads or writes afterwards fails on the closed connection. `write_timeout` (60 s): a response write of which the socket accepts no 64 KiB slice (the whole write, if smaller) for that long closes the connection, and `idle_timeout` no longer cuts a response being written (0.8's idle timeout cut a client taking a single large response frame for over 120 s). The client taking bytes from the socket is not checked, so a slow client with a large send buffer may be cut while it still reads: raise `write_timeout`, or turn it off, for slow downloads. Routes receiving large or slow uploads raise their deadline, or turn it off, with `.request_timeout(duration)` on the route, its group or its virtual host (see [Limits and Timeouts per Route](README.md#limits-and-timeouts-per-route)). To keep 0.8's behaviour, set the three to `0`.
- **B10**: a server's `stop()` and its destructor close every connection still open (idle keep-alive, handling a request, or taken over) and cancel the handlers still running with `cancel_reason::shutdown`, whatever their route declared, without waiting for them. 0.8 only stopped listening and left the connections open, served by a server that may have been destroyed: the next request of a keep-alive connection used freed memory. A handler still running may finish its current step: the routes, hosts, middlewares, hooks and settings it reaches stay alive until it ends, and what it sends is dropped; what it captured itself is the application's to keep alive. Stopping or destroying a server from one of its own handlers does not block. Code that called `stop()` to stop accepting while the connections open finished calls `stop_listening()`, or `shutdown(by)` for a graceful drain (see [Server Lifecycle](README.md#server-lifecycle)).
- **B11**: **Breaking:** SSE and WebSocket server connections (`sse_connection`, and the `websocket_connection` of `upgrade_websocket()`) have a write timeout by default, and reverse proxy tunnels an idle timeout. 0.8 had neither: an SSE or WebSocket client that stopped reading kept its connection, its newest events or messages dropped, until it left, and a proxied tunnel whose two peers went silent, or one of them stopped reading, stayed open for ever. `write_limits::write_timeout` (60 s, nginx's `send_timeout` and `proxy_send_timeout`, and B9's `write_timeout`): a write that makes no progress for that long, no byte taken by the socket nor by the client from it (checked once the socket took nothing for the timeout, so a stalled write closes within a quarter more, 75 s by default), closes the connection (a WebSocket with `1013`), `on_close()` is called and awaited sends and `wait_writable()` complete with `false`. It bounds a stall, so a slow client that keeps reading is never cut, however large an event or message; 0.8's `write_timeout`, off by default, bounded the whole write of each message and cut those clients. `set_write_limits()` with limits that do not name `write_timeout` keeps the 60 s. The SSE keep-alive does not move it (see [Server-Sent Events](README.md#sse-server)). `proxy_options::tunnel_idle_timeout` (120 s): a tunnel with nothing forwarded in either direction for that long has both its connections closed; any byte forwarded counts, WebSocket pings and pongs included. 120 s, above nginx's and AWS ALB's 60 s and Cloudflare's 100 s, because a thinger-http WebSocket pings a quiet peer only after 60 s; it equals the WebSocket and connection idle timeouts. It is separate from `proxy_options::idle_timeout`, which covers the exchange only: `idle_timeout = 0` (event streams) does not turn it off. To keep 0.8's behaviour, set `write_timeout = 0` in the `write_limits` of each connection and `tunnel_idle_timeout = 0` in the `proxy_options` of the proxy or of each `forward()` call (see [WebSocket Flow Control](README.md#websocket-flow-control) and [Reverse Proxy](README.md#reverse-proxy)). `asio::socket_pipe` keeps no deadline unless `set_idle_timeout()` is called.

Defaults that keep 0.8's behaviour: `max_pending_handshakes` (unbounded), `session_cache` (on, 20,480 sessions), the keepalive and message size of the WebSocket message interface (pings after 60 s, closes after 120 s, 16 MiB messages), the SSE keep-alive (15 s), the exchange timeouts of the reverse proxy (`connect_timeout` 10 s, `idle_timeout` 60 s), and the idle and linger bounds of `connection_options` (120 s idle; linger 8 MiB, 5 s). These do not: the three request and response deadlines of `connection_options` (B9), the write timeout of SSE and WebSocket server connections, and the idle timeout of reverse proxy tunnels (B11).

### Configuration mapping

An application with a configuration file feeds these options from it. The keys below are those of the Thinger.io server, which uses the library; other applications follow the same shape.

| Library option | Configuration key | Notes |
|---|---|---|
| `workers::start(threads)` | `server.threads` | 0 means the hardware threads: call `start()` with no argument, as `start(0)` returns `invalid_argument` |
| `listener_options::endpoint` (HTTP, HTTPS) | `http_server.address`, `http_server.port`, `http_server.ssl_port` | one application, two listeners on one server |
| `listener_options::endpoint` (device protocols) | the address and port keys of `thing_server` and `mqtt_server` | unchanged |
| `listener_options::tls` | the manager of the node's TLS service | one per node, shared by every TLS listener |
| `listener_options::client_verify` | `thing_server.client_certificates`, `mqtt_server.client_certificates`; HTTPS: none | the listener's mode |
| `certificate_manager::set_client_verify(host, mode)` | `ssl.client_verify_hosts` | overrides the listener's mode for those names |
| `listener_options::handshake_timeout` | `thing_server.tls_handshake_timeout`, `mqtt_server.handshake_timeout`; HTTPS: no key (60 s, the default) | unchanged |
| `listener_options::max_listening_attempts` | none: 1 | a busy port fails at once |
| `listener_options::max_pending_handshakes`, `session_cache` | none yet | the defaults (unbounded; on, 20,480 sessions) |
| `connection_options::idle_timeout`, `linger_max_bytes`, `linger_timeout` | none: the library defaults (120 s; 8 MiB, 5 s) | as in 0.8 |
| `connection_options::header_timeout`, `request_timeout`, `write_timeout` | none yet: the library defaults (60 s, 300 s, 60 s) | new in 0.9 (B9); `.request_timeout()` on the routes, groups or hosts receiving large uploads |
| `proxy_options::tunnel_idle_timeout` | none yet: 120 s | new in 0.9 (B11) |
| the deadline `by` of `shutdown(by)` | `now + server.shutdown_timeout − server.cancel_grace` | `server.cancel_grace` (default 1 s) leaves the interrupted handlers time to unwind before the process stops; a configuration with `cancel_grace > shutdown_timeout / 4` is refused |
| `websocket_accept_options::max_message_size` | none: 16 MiB | the 0.8 value |
| `write_limits` of `websocket_connection` and `sse_connection` | derived from the node's memory budget (`memory.node_bytes`) | per-connection queues inside that budget; `write_timeout` 60 s unless set (B11) |

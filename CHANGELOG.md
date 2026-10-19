# Changelog

All notable changes to thinger-http are documented in this file. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html), as described in
[Versioning](README.md#versioning).

## [Unreleased]

## [0.7.1] - 2026-10-19

### Added

- `http::server::set_lingering_close(max_bytes, timeout)` (static, for every server of the process, so that the classes keep the layout of 0.7.0 and its ABI): how long a connection closing with its request still arriving reads and drops it (see Fixed). 8 MiB and 5 s by default; 0 for either closes at once, as before.

### Fixed

- A connection closed after a response while the client was still sending its request (a body over the limit answered `413` before it was read, `417`, a `401` from a middleware before a body too large to discard, a request timeout, a chunked body going past the limit, a header section too large) was closed with the body unread: the system answered the data still arriving with a reset, so a client still sending got `EPIPE` or a connection reset, and never read the response (e.g. 4 MiB sent to a route limited to 64 KiB). The server now closes such a connection gracefully (RFC 9112 section 9.6): once the response is written it stops sending, reads and drops what keeps arriving until the client closes, up to 8 MiB or for 5 s at most, and then closes. Its memory does not grow meanwhile, and a graceful shutdown does not wait for it. A body within the limit left unread is still dropped to keep the connection alive, as before. See [Limits and Timeouts per Route](README.md#limits-and-timeouts-per-route).
- The client reads the response a server sent before the request was written whole, when writing the rest fails because the server closed the connection (`EPIPE` or a reset), as curl does: it returned the write error. A `408` read that way on a reused connection is taken as the server closing it while idle, and the request is sent once more on a new one, whatever its method, as it was not processed.

## [0.7.0] - 2026-10-19

### Added

- `on_close()` on the server WebSocket (`websocket_connection`) and SSE (`sse_connection`) connections: called once the connection closed, whatever closed it (the peer, a lost connection, `stop()`, the server shutdown, the overflow policy, a write timeout...), once, posted to the thread of the connection (never from within the call closing it), what it throws logged; set once the connection closed, it is posted at once. The WebSocket one receives a `http::websocket_close`: the status code of the peer's close frame and its reason, or the code the server closed with, or 1006 if no close frame was exchanged, and whether the peer closed first. An `sse_connection` reads its client, only to notice when it leaves, so the producer learns it at once, not at its next event: a client half-closing the connection (EOF) has left too. See [WebSocket Close](README.md#websocket-close).
- `asio::websocket::get_close_code()`, `get_close_reason()` and `closed_by_peer()`: the closing handshake, once closed; `CLOSE_NO_STATUS` (1005), `CLOSE_ABNORMAL` (1006) and `CLOSE_INTERNAL_ERROR` (1011).
- `request::get_request_scheme()`: `"http"` or `"https"`, the scheme of the connection, or, when the peer is a trusted proxy, the one it forwarded for the client of `get_request_ip()`, in the same walk: the `X-Forwarded-Proto` value aligned with the `X-Forwarded-For` entry of the client (or its last value when they have not as many), or the `proto` of the `Forwarded` element of the client (RFC 7239). `trusted_proxies::resolve()` (address and scheme at once) and `parse_scheme()`. See [Client IP Behind Proxies](README.md#client-ip-behind-proxies).
- `http_application::on_response(hook)`: called once per request with its response (`http::response_info`: status, bytes written, duration, and whether it completed, handed the connection over or was aborted, and was streamed), whoever answered it, the server's own errors included, once the response ended and the application is done with the request. It costs nothing without a hook. See [Response Hook](README.md#response-hook).
- `request::get_http_request() const`, returning a `std::shared_ptr<const http_request>`.
- TLS options of the server contexts: `asio::tls_options` (minimum and maximum versions, TLS 1.2 ciphers, TLS 1.3 cipher suites, server cipher preference, groups and OpenSSL security level), `certificate_manager::set_tls_options()` and `get_tls_options()`, and `asio::apply_tls_options(context, options)` for contexts made elsewhere. They apply to every context the manager creates, the certificates selected by SNI included. See [TLS Settings](README.md#tls-settings).

### Changed

- TLS defaults: the contexts `certificate_manager` creates follow Mozilla's "intermediate" configuration, as Node and nginx: TLS 1.2 and 1.3 only, ECDHE with AES-GCM or ChaCha20-Poly1305 for TLS 1.2, and the client's cipher preference. They allowed from TLS 1.0 (refused by the default security level of OpenSSL 3, not by the server) and OpenSSL's default ciphers, CBC and those without forward secrecy included. Clients without TLS 1.2 (Android before 4.4.2, Java 7, Internet Explorer 10 and older) are refused; `enable_legacy_protocols(true)` still allows TLS 1.0 and 1.1 (with OpenSSL's default ciphers unless others are set). `set_server_ciphers()` and `enable_legacy_protocols()` are deprecated: they change the TLS options, validated (`set_server_ciphers()` returns `bool`), in any order; `enable_legacy_protocols(false)` undoes only what `true` did, and the server cipher preference no longer requires a cipher list.
- A server WebSocket whose `on_message()` throws is closed with 1011 (Internal Error), the exception logged: it stopped reading, and was left open.
- A server WebSocket whose read fails (the connection reset, or ended without a close frame) is closed, instead of being left open, unread, until released.
- `websocket_connection::release_socket()` called from `on_message()` stops reading the socket at once: the WebSocket read the next message from the socket released.
- An in-memory dispatch that times out completes the response with the 504 it returns, so a late answer is dropped; a response that completes in the meantime is returned instead of the 504.
- Fewer allocations answering: `response::header(key, value)` takes its key and value by value, moved into the response (callers do not change; pass an rvalue to save the copy), and the answer sent moves the status and headers set before answering into its message instead of copying them. A header set before answering costs one allocation for a long value passed as an lvalue, and none when moved in, instead of five: a response with a 36-character `X-Request-Id` took 8 allocations (3 without it), and now takes 4, or 3 with the value moved in. Each answer claims the response before building its message, so `json()`, `encode()` and `send_file()` encode or read the body first (an error answered instead still has the headers set). `http_frame`, `headers` and `http_response` are movable: moving one copied it.

- 0.7.0 is not ABI compatible with 0.6.0 (the signature of `response::header()`, the virtual functions of `response_sink`, the layout of several classes): rebuild what links it. The shared library's SONAME carries the minor version while on 0.x (`libthinger_http.so.0.7`), so the next ones do not load in its place. The overload `request::get_http_request() const` makes `&request::get_http_request` ambiguous: cast it to the one wanted.

## [0.6.0] - 2026-10-17

### Added

- `asio::websocket` takes the executor it is used on (e.g. a strand), the io_context of its socket by default, and `get_executor()` returns it. `asio::running_in_this_thread(executor)` (`<thinger/asio/io_context_of.hpp>`) tells whether the caller runs on an io_context executor or a strand held as an `any_io_executor`.

### Changed

- `websocket_client`'s synchronous calls (`send_text()`, `send_binary()`, `receive()`, `close()`) throw `std::logic_error` when called on the strand the WebSocket was opened from, where they waited forever; the coroutine API works there.
- `parse_websocket_url()` compiles its regex once instead of on every call.
- Renames and signatures of internal helpers, following the executor of the WebSocket: `on_io_context(io_context, keeper, operation)` (`<thinger/util/types.hpp>`) is `on_executor(executor, keeper, operation)`, which runs the operation at once when the calling coroutine is on that executor; `http::write_flow` takes an `any_io_executor` instead of an `io_context&`; and the `deliver` of `response::accept_websocket()` receives the `asio::websocket&` of the connection instead of its raw `asio::socket&`.

### Fixed

- A WebSocket runs its keepalive, the control frames it sends (pings and pongs) and its closing handshake on the executor it was created with, and the clients create it with the executor of the coroutine opening it (`client`, `async_client` and `executor_client` `websocket()`). They ran on the io_context of the socket: opened from a strand of an io_context run by several threads, they could use the WebSocket on one thread while the caller used it on another. `websocket_connection` runs on the executor of its WebSocket too. Nothing changes on an io_context run by one thread, nor for the server.

## [0.5.0] - 2026-10-17

### Added

- `executor_client`: an HTTP client running on an io_context the caller already runs, usually the one of the calling coroutine (`executor_client::for_current_coroutine()`), with no threads of its own and no idle timers. Its requests run on the caller's thread and are cancelled with the coroutine awaiting them; it keeps no idle connection by default and closes its connections when destroyed. For occasional calls from server coroutines; a pooled client (`async_client`) suits many calls to the same host. See [Executor Client](README.md#executor-client-executor_client).
- `asio::io_context_of(executor)` (`<thinger/asio/io_context_of.hpp>`): the io_context of an io_context executor or of a strand of one, null for other executors.

### Changed

- Quieter startup and shutdown logs. At `info`, a server logs where it listens (`listening on http://0.0.0.0:8080`, `https://...` or `unix:/path`, with the port bound), when a graceful shutdown starts (`shutting down the server on http://0.0.0.0:8080: 2 connections open, grace period of 30000 ms`) and when it stopped (`server on http://0.0.0.0:8080 stopped`, also after `stop()` without a shutdown), one line each, as Node and Go servers do; the workers log the signal received and what it does in one line (`received signal 15: stopping gracefully, within 30000 ms (a second signal stops at once)`). The rest moved to `debug`: a line per connection accepted (`received connection from: ip: ...`, `received connection on Unix socket: ...`), a line per worker thread stopped, worker clients registered and unregistered, the steps of the workers starting and stopping, acceptors starting and stopping, and the socket servers' own `TCP server is now listening` (`Unix socket server ...`, `Standalone server ...`), which said `TCP` for TLS too. Warnings and errors did not change. See [What a Server Logs When It Starts and Stops](README.md#what-a-server-logs-when-it-starts-and-stops).

### Fixed

- The connect timeout of the TCP and Unix sockets runs on the executor of the coroutine connecting, as the connect does. It ran on the io_context of the socket: from a strand of an io_context run by several threads, it could cancel the socket on one thread while the connect used it on another.
- The library builds on macOS with Apple clang (libc++) and OpenSSL 3.6: the self-signed certificate wrote into the subject name OpenSSL now returns as const, and the key of the client's TLS contexts printed a file time count that is a 128-bit integer with libc++.

## [0.4.0] - 2026-10-16

### Added

- Flow control for the server WebSocket (`websocket_connection`) and SSE (`sse_connection`) connections, for streams between a fast and a slow end. See [WebSocket Flow Control](README.md#websocket-flow-control).
  - `send_text_async()` and `send_binary_async()`: awaitable sends that complete once the message is written to the socket (never dropped).
  - `set_write_limits()` with a `http::write_limits`: limits of the queue in messages and in bytes, high and low watermarks (`on_high_watermark()`, `on_low_watermark()`, told asynchronously, and `wait_writable()`), the overflow policy of the messages sent without waiting: `drop_newest` (the default, as before), `drop_oldest`, or `close` (WebSocket `1013 Try Again Later`), and an optional `write_timeout` closing a client that stops reading. `queued_bytes()` and `queued_messages()` (atomic, readable from any thread) tell what is not written yet; `queue_full()` tells whether the next message overflows. SSE drops whole events.
  - `pause_reading()` and `resume_reading()`: a WebSocket stops reading its socket, so TCP backpressure reaches the client, and goes on in order. The keepalive is suspended while paused.
- `log_bodies(enabled, max_length)` on the server (`http_server_base`) and the client (`http_client_base`): an opt-in for debugging that logs the header values and the start of the bodies of the requests and responses, up to `max_length` bytes (1024 by default), with control characters escaped. See [What the Message Logs Show](README.md#what-the-message-logs-show).
- `logging::should_log(level)` and `LOG_INFO_ENABLED()`, `LOG_DEBUG_ENABLED()`, `LOG_TRACE_ENABLED()`, to skip building a costly log message.
- `asio::websocket::close(code)` is public, with `CLOSE_TRY_AGAIN_LATER` (1013), `CLOSE_INVALID_PAYLOAD` (1007) and `CLOSE_MESSAGE_TOO_BIG` (1009); codes that may not be sent (1005, 1006, 1015...) are sent as 1000. `get_ping_interval()`/`get_idle_timeout()` tell the keepalive settings.
- Benchmarks `benchmark_ws_echo` (echo throughput) and `benchmark_ws_slow_reader` (memory with a slow reader, by producer mode).

### Changed

- `asio::websocket::set_keepalive()` counts the idle time from the call.
- Received close codes 1012 to 1014 (IANA registry) are valid, as in Node ws and Go: they were answered as a protocol error (1002).
- A server WebSocket whose write fails is closed, and the messages queued are discarded, instead of being kept until the connection was released.
- A server WebSocket receiving invalid UTF-8 text closes with `1007`, and one receiving a message larger than its buffer with `1009`, instead of only stopping to read.
- `http_frame::log(scope, level)` is `log(scope, options)`, with a `http::body_logging` (the level was not used). A frame without a log of its own, as the body chunks of a streamed response, logs nothing: it logged a warning for every chunk.

### Removed

- `websocket_connection::congested_connection()`: use `queue_full()`.

### Security

- The HTTP logs no longer show bodies, header values or WebSocket messages, which may hold credentials. The client and the server logged the bodies at `trace` level, and the header values at `debug` level, Authorization and Cookie included; the server WebSocket logged every message received (hex encoded) at `debug` level. They now log the request line or the status, the header names and the body size; bodies and header values only with `log_bodies()`, and the values of `Authorization`, `Proxy-Authorization`, `Cookie` and `Set-Cookie` never. Nothing is formatted unless its level is logged.
- The logged request lines and URLs redact the values of query parameters that carry credentials (`access_token`, `authorization`, `token`, `api_key`, `apikey`, `key`, `password`, `client_secret`) and the password of the user info: `GET /data?authorization=[redacted]&count=10`. `http::util::url::loggable_uri()` does it.

## [0.3.0] - 2026-10-14

### Added

- Request context: typed values a middleware stores in the request for the route handler, such as the authenticated principal (`req.context().set(key, value)`, `req.context().get(key)`), as Express `res.locals`, Go `context.WithValue` or Rust `http::Extensions`. Values live under `http::context_key<T>` keys, told apart by their address, so two values of the same type do not collide; move-only values are supported, and they are destroyed with the request. Every kind of middleware and route callback sees them, and in-memory dispatch too, which seeds them with `dispatch_options::context`. A request storing nothing allocates nothing for it. See [Request Context](README.md#request-context).

### Changed

- `certificate_manager::set_default_certificate(certificate, key)` returns `bool`, and keeps the current default certificate when they are not valid, instead of removing it.
- `certificate_manager::get_default_host()` returns a copy (`std::string`), as the reference returned could change while it was read.
- The manager installs its SNI callback on the contexts it creates, and on those given to `set_default_certificate()` and `set_certificate()`, when they are created or set. The servers no longer install it as they start listening (`tcp_socket_server::set_sni_callback()` is still there for other contexts).
- A TLS handshake with a host name (SNI) without a certificate of its own logs at debug level, instead of a warning per handshake.

### Fixed

- TLS: replacing the default certificate (`certificate_manager::set_default_certificate()`) while a server listens applies to the next handshakes without SNI or with a host name without a certificate, as Node's `setSecureContext()` and Go's `GetCertificate` do. They got the default certificate of when the server started listening until a restart (e.g. after an ACME renewal). Connections keep the certificate they took.
- `certificate_manager`: certificates created while the cipher list or the legacy protocols were being changed read those settings without the lock.
- TLS: a server starting to listen wrote the SNI callback into the default context, shared with the servers already handshaking on it (a data race).

## [0.2.0] - 2026-10-14

### Added

- Precompressed static files: `serve_static()` sends a file as its `<file>.br` or `<file>.gz` sibling to the clients accepting `br` or `gzip` (by `q` value, `br` first at equal quality), if the sibling is a regular file not older than the file, with `Content-Encoding`, the `Content-Type` of the file, `Vary: Accept-Encoding` and an `ETag` of its own (`W/"...-br"`); conditional requests, `HEAD` and byte ranges apply to it. On by default (`static_options::precompressed`).
- Lists of content codings (`Content-Encoding: deflate, gzip`) are decoded in reverse order, by the client and by the server, up to five of them; a list with a coding that is not supported is kept as it is (`util::decode_content`).
- `deflate` bodies without the zlib header (raw deflate) are decoded, as curl and browsers do.
- `parse_accept_encoding()`: the quality `Accept-Encoding` gives gzip, deflate, br and identity.
- `Expect: 100-continue` (RFC 9110 section 10.1.1): the server sends `100 Continue` when the body is first read, by the server or by the handler, in order with the pipelined responses, so curl uploads no longer wait a second for it. A request answered without reading its body gets no `100 Continue`, and its connection is closed after the response, as Go and Node do. The reverse proxy sends it once it starts streaming the body to the target.
- `417 Expectation Failed` (`http_response::status::expectation_failed`) for any expectation other than `100-continue`, as Node and Go answer. `http_request::get_expectation()` tells the expectation of a received request.
- multipart/form-data bodies (file uploads), parsed as they arrive without holding the body in memory: `req.read_multipart()` gives the fields and writes the files to temporary files, deleted with the form unless moved (`move_to()`) or kept (`release()`); `http::multipart_reader` streams the parts and their content, like Go's `multipart.Reader`; `http::multipart_parser` is the incremental parser under both. Limits on parts, files, header sections, filename, field and file sizes (`413`), strict RFC 7578 / RFC 2046 syntax (`400`), `filename*`, and safe filenames against path traversal. See [File Uploads](README.md#file-uploads-multipartform-data).
- `multipart` body decoder, for routes that `accepts({"multipart"})`: the same object as a urlencoded form, files as objects with their content.
- Fuzz target `fuzz_multipart`, with its seed corpus.

### Changed

- gzip bodies are decoded whole: every member of concatenated gzip data, instead of the first one only, with the size limit on all of them. Bytes after the last member (other than zero padding), and after the end of a deflate stream, now fail to decode (`client_error::bad_content_encoding`, or `400` for request bodies) instead of being ignored.
- `util::gzip::decompress()` and `util::deflate::decompress()` take a `std::string_view`.
- Client: the synchronous `client` also closes its expired idle connections as a request ends, not only as the next one starts, so they no longer wait for the next call after a long request, nor take the room of other connections in the pool. Between calls they still stay open until the next call (as in curl and urllib3): `clear_connections()` closes them.
- A request with `Expect: 100-continue` answered without reading its body (e.g. `401` from a middleware) closes the connection after the response (`Connection: close`), instead of discarding the body, which the client may never send. A request with another expectation is answered `417` before the middlewares and the handler run. `Expect` in HTTP/1.0 requests is ignored, as before.
- The built-in body decoders include `multipart`: a route taking the body decoded that accepts `multipart/form-data` (by that media type) decodes it now, where it answered `415` for lack of a decoder. Routes that do not list it are not affected; `body_decoders().remove("multipart")` restores the previous behavior.
- Internal reorganization of the server responses, without behavior changes: the reverse proxy is split by concern (`reverse_proxy/`), file responses and static files move to `file_server/`, and response compression to `response_compression.hpp`, which now declares `parse_accept_encoding()` and `negotiate_content_coding()` (`body_decoders.hpp` still includes it).

### Fixed

- `Content-Encoding` names are case-insensitive and `x-gzip` is gzip when decoding responses (client) and request bodies (server), as RFC 9110 says; only `gzip` and `deflate` in lowercase were decoded.
- Client: multipart forms percent-encode `"`, CR and LF in field names and filenames, as browsers and curl do. Before, a filename with a quote or a line break could end the `Content-Disposition` parameter or header and inject others.

### Removed

- `thinger/util/gzip.hpp`, a legacy header (the global `Gzip` class, on CryptoPP and Boost.Iostreams) that nothing used and did not compile on its own. Compression is in `thinger/util/compression.hpp` (`util::gzip`, `util::deflate`).

## [0.1.0] - 2026-10-14

First tagged release. Until now the library was used from `main`.

### Server

- HTTP/1.1 server on Boost.Asio coroutines, served on a pool of worker threads or standalone, over TCP, TLS (with per-domain certificates through SNI) and Unix sockets.
- Routing on a tree of path segments: path parameters with constraints, query parameters, route groups, and documentation fields and free metadata per route.
- Synchronous and coroutine handlers. Handlers can take the JSON body already parsed and validated against a JSON Schema.
- Request bodies decoded by `Content-Type` (JSON, msgpack, CBOR, UBJSON, forms), with limits on size, nesting and values. Bodies can be read whole, deferred or streamed.
- Middlewares, synchronous or coroutines, Express style.
- Body size, decoding limits and timeout per route, group and virtual host.
- OpenAPI 3.1 documents generated from the routes.
- In-memory request dispatch, without a socket, with a timeout that cancels the handler.
- Virtual hosts, a configurable error format, and the client IP behind trusted proxies.
- Static files with `ETag`, `Last-Modified`, `304 Not Modified`, byte ranges (`206`, `416`), `Cache-Control` and an SPA fallback.
- `HEAD` answered by the `GET` routes, without a body. `405 Method Not Allowed` with `Allow`.
- Response compression (gzip, deflate) negotiated as RFC 9110 says (q-values, `identity`, `*`), with `Vary: Accept-Encoding`, as Express's `compression` does.
- Streaming responses with flow control, chunked responses, Server-Sent Events, WebSocket, and taking a connection over (also per stream).
- WebSocket as a byte stream socket, for tunnels and other protocols over it.
- Reverse proxy to HTTP, HTTPS and Unix socket targets, streaming bodies both ways and tunneling WebSockets.
- Graceful shutdown: stop accepting, drain the requests in progress within a grace period and notify WebSocket and SSE connections. `workers::wait_graceful()` does it on `SIGINT`/`SIGTERM`, and stops at once on a second signal.
- Connection statistics by kind (HTTP, WebSocket, SSE, tunnels, taken over).

### Client

- Synchronous `client` and `async_client` (callbacks or coroutines), with a fluent request builder, redirects, streaming and file downloads, Unix sockets and WebSocket.
- Keep-alive connection pool: idle connections reused per origin, bounded per host and in total, closed after an idle timeout (shortened to the server's `Keep-Alive: timeout`), checked before reuse, and requests retried once on a new connection when that is safe (as Go does).
- HTTP proxies: absolute-form for `http://`, `CONNECT` tunnels for `https://`, `ws://` and `wss://`, Basic proxy authentication, and the `http_proxy`, `https_proxy`, `all_proxy` and `no_proxy` variables (uppercase too, except `HTTP_PROXY`, never read, as curl does, against httpoxy).
- Additional trusted CAs (files or PEM), with or without the system ones.
- Responses parsed as strictly as requests, with an error code for each way a response can be wrong (`client_error`).
- gzip and deflate bodies decoded within the maximum content size (no decompression bombs); a body that cannot be decoded fails with `bad_content_encoding`.
- `TCP_NODELAY` on client connections.

### Robustness

- Strict request parsing and framing (RFC 9112): `Content-Length` and `Transfer-Encoding` rules, no obsolete line folding, ASCII request targets, bounded header sections and bodies.
- Fuzzing with libFuzzer of the request and response parsers, the chunked decoder, the body decoders, URL decoding and gzip/deflate decoding, and differential fuzzing against llhttp and Boost.Beast.
- Test suite run in CI in Release and with AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer, plus CodeQL and coverage.

### Changed (for users tracking `main` before 0.1.0)

These changes of the last weeks can be noticed by code written against an earlier `main`:

- A coroutine lambda passed to a callback that is only called, never awaited (`sync_callback`, e.g. `on_message`, `on_shutdown`, error formatters, hooks), no longer compiles. Before, it compiled and never ran. `use()`, `take_over()`, `upgrade_websocket()`, `start_sse()` and the client callbacks now run coroutines.
- A route with `.schema()` validates the body whatever the handler signature (400 when invalid, 415 when its type cannot be decoded). Before, a handler taking `(request&, response&)` skipped the validation.
- Responses sent whole always carry the `Content-Length` of their content and never `Transfer-Encoding`. 1xx and 204 have no body nor `Content-Length`, and 304 has no body.
- Responses of `take_over()` that are not framed, and event streams, say `Connection: close`.
- `Vary: Accept-Encoding` is added to compressible responses even when they are not compressed. `Accept-Encoding: gzip;q=0` is honoured, and `Cache-Control: no-transform` disables compression.
- The reverse proxy answers 504 instead of 400 when its timeout ends while it streams the request body.
- Client: `auto_decompress(false)` keeps bodies as received; before, a compressed body was decoded anyway. A body that cannot be decoded fails with `client_error::bad_content_encoding` instead of being returned undecoded.
- Client: streamed requests (`send_streaming()`, `download()`) ask for `Accept-Encoding: identity` and are never decoded. `stream_result::headers` is filled (names in lowercase).
- Client: connections are kept alive between requests (see the README to tune or disable the pool).
- The unused `request_handler` interface was removed.

[Unreleased]: https://github.com/thinger-io/thinger-http/compare/v0.7.1...HEAD
[0.7.1]: https://github.com/thinger-io/thinger-http/compare/v0.7.0...v0.7.1
[0.7.0]: https://github.com/thinger-io/thinger-http/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/thinger-io/thinger-http/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/thinger-io/thinger-http/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/thinger-io/thinger-http/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/thinger-io/thinger-http/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/thinger-io/thinger-http/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/thinger-io/thinger-http/releases/tag/v0.1.0

# Changelog

All notable changes to thinger-http are documented in this file. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html), as described in
[Versioning](README.md#versioning).

## [Unreleased]

### Added

- Flow control for the server WebSocket (`websocket_connection`) and SSE (`sse_connection`) connections, for streams between a fast and a slow end. See [WebSocket Flow Control](README.md#websocket-flow-control).
  - `send_text_async()` and `send_binary_async()`: awaitable sends that complete once the message is written to the socket (never dropped).
  - `set_write_limits()` with a `http::write_limits`: limits of the queue in messages and in bytes, high and low watermarks (`on_high_watermark()`, `on_low_watermark()`, told asynchronously, and `wait_writable()`), the overflow policy of the messages sent without waiting: `drop_newest` (the default, as before), `drop_oldest`, or `close` (WebSocket `1013 Try Again Later`), and an optional `write_timeout` closing a client that stops reading. `queued_bytes()` and `queued_messages()` (atomic, readable from any thread) tell what is not written yet; `queue_full()` replaces `congested_connection()`, which stays. SSE drops whole events.
  - `pause_reading()` and `resume_reading()`: a WebSocket stops reading its socket, so TCP backpressure reaches the client, and goes on in order. The keepalive is suspended while paused.
- `asio::websocket::close(code)` is public, with `CLOSE_TRY_AGAIN_LATER` (1013), `CLOSE_INVALID_PAYLOAD` (1007) and `CLOSE_MESSAGE_TOO_BIG` (1009); codes that may not be sent (1005, 1006, 1015...) are sent as 1000. `get_ping_interval()`/`get_idle_timeout()` tell the keepalive settings.
- Benchmarks `benchmark_ws_echo` (echo throughput) and `benchmark_ws_slow_reader` (memory with a slow reader, by producer mode).

### Changed

- `asio::websocket::set_keepalive()` counts the idle time from the call.
- Received close codes 1012 to 1014 (IANA registry) are valid, as in Node ws and Go: they were answered as a protocol error (1002).
- A server WebSocket whose write fails is closed, and the messages queued are discarded, instead of being kept until the connection was released.
- A server WebSocket receiving invalid UTF-8 text closes with `1007`, and one receiving a message larger than its buffer with `1009`, instead of only stopping to read.

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

[Unreleased]: https://github.com/thinger-io/thinger-http/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/thinger-io/thinger-http/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/thinger-io/thinger-http/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/thinger-io/thinger-http/releases/tag/v0.1.0

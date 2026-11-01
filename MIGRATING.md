# Migrating

How to move code from one minor version to the next while the version is 0.x (see [Versioning](README.md#versioning)). Each entry of the [changelog](CHANGELOG.md) under **Changed** and **Removed** says what changed and how to adapt; this guide puts the breaking ones in the order a migration meets them, with code before and after.

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

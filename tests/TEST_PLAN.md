# Test Plan for thinger-http

This document outlines the comprehensive test plan for the thinger-http library, organized by component.

## ASIO Component Tests

The ASIO layer provides the foundation for all asynchronous I/O operations in the library. This section details the test structure and coverage requirements for all ASIO components.

### Test Structure

```
tests/asio/
├── core/                     # Core worker and threading tests
│   ├── test_io_worker.cpp
│   ├── test_worker_thread.cpp
│   ├── unit/asio/workers_test.cpp ✓ (start/stop, executors, threads, wait)
│   └── integration/workers_instances_test.cpp ✓ (two workers in one process)
├── sockets/                  # Socket abstraction tests
│   ├── test_socket_base.cpp
│   ├── test_tcp_socket.cpp
│   ├── test_ssl_socket.cpp
│   ├── test_unix_socket.cpp
│   └── test_websocket.cpp
├── servers/                  # Server implementation tests
│   ├── integration/listener_test.cpp ✓ (TCP, TLS and Unix listener)
│   ├── integration/listener_tls_test.cpp ✓ (handshake bound, client certificates, session cache)
│   ├── integration/server_listeners_test.cpp ✓ (one server on several listeners, connection options)
│   ├── integration/shutdown_deadline_test.cpp ✓ (shutdown(by): its deadline, several listeners, threads of the server)
│   ├── integration/shutdown_test.cpp ✓ (graceful shutdown: drain, WebSocket, SSE, takeover, signals)
│   └── test_server_lifecycle.cpp
├── ssl/                      # SSL/Certificate tests
│   └── test_certificate_manager.cpp ✓ (existing)
└── integration/              # Integration tests
    ├── test_worker_socket_integration.cpp
    ├── test_server_client_integration.cpp
    └── test_ssl_integration.cpp
```

### Core Worker Tests

#### test_io_worker.cpp
- [ ] **Construction/Destruction**
  - [ ] Default construction creates valid io_context
  - [ ] Work guard prevents io_context from stopping
  - [ ] Destruction properly cleans up resources
- [ ] **Lifecycle Management**
  - [ ] start() initializes work guard
  - [ ] stop() removes work guard
  - [ ] stop() resets io_context
  - [ ] Multiple start/stop cycles work correctly
- [ ] **IO Context Access**
  - [ ] get_io_context() returns valid context
  - [ ] IO context can execute posted handlers
  - [ ] IO context stops when work guard removed

#### test_worker_thread.cpp
- [ ] **Thread Management**
  - [ ] Constructor creates named thread
  - [ ] start() returns valid thread ID
  - [ ] Thread name is properly set
  - [ ] stop() properly terminates thread
  - [ ] Thread joins correctly on destruction
- [ ] **Async Operations**
  - [ ] run() executes handlers in thread context
  - [ ] async_worker() can be overridden
  - [ ] Handlers execute on correct thread
- [ ] **Error Handling**
  - [ ] Exceptions in handlers don't crash thread
  - [ ] Thread recovers from errors

#### unit/asio/workers_test.cpp ✓
- [x] **Lifecycle**
  - [x] start()/stop(); start() again after stop(), on new threads
  - [x] A second start() returns already_started and adds no thread; 0 threads is invalid_argument
  - [x] stop() twice, and before start()
  - [x] stop() from one of its threads, then again from another thread, or started again
  - [x] Destroyed while running (it stops), and while one of its threads stops it
- [x] **Executors**
  - [x] next_executor() round-robin, one by one and from 2N threads at once (twice each)
  - [x] next_executor() and isolated_executor() null when never started or stopped
  - [x] isolated_executor(name): out of the pool, on a thread of its own named after it, asked for while the pool hands out executors
  - [x] get_thread_io_context(): the caller's on its threads, one of the pool on any other (another workers' included)
- [x] **Threads**
  - [x] in_worker_thread() true on its threads (pool and isolated), false on any other and once stopped
- [x] **Signals**
  - [x] wait() returns the signal and the workers keep running; twice in a row, a signal each
  - [x] wait() released by stop() with 0 (from another thread and from one of its own), several waits at once
  - [x] An empty set waits for stop(); not running, it returns 0 at once

#### integration/workers_instances_test.cpp ✓
- [x] Two workers in one process, each with a pool_server and an async_client: each request served on its server's workers and answered on its client's
- [x] in_worker_thread() of each tells its own threads only
- [x] Stopping one leaves the other serving and requesting
- [x] Nothing starts the workers by themselves (no thread before start())

### Socket Tests

#### test_socket_base.cpp
- [ ] **Abstract Interface Testing**
  - [ ] Cannot instantiate socket directly
  - [ ] Virtual destructor behavior
  - [ ] Connection counting per io_context
- [ ] **Mock Implementation Tests**
  - [ ] All virtual methods can be overridden
  - [ ] Default implementations where applicable

#### test_tcp_socket.cpp
- [ ] **Connection Management**
  - [ ] Connect to valid endpoint
  - [ ] Connect with timeout
  - [ ] Connect failure handling
  - [ ] Multiple connection attempts
  - [ ] Cancel connection in progress
- [ ] **Socket Options**
  - [ ] TCP_NODELAY setting
  - [ ] Socket reuse options
- [ ] **Read Operations**
  - [ ] Synchronous read
  - [ ] Asynchronous read_some
  - [ ] Asynchronous read exact size
  - [ ] Read until delimiter
  - [ ] Read with timeout
  - [ ] Read cancellation
- [ ] **Write Operations**
  - [ ] Synchronous write
  - [ ] Asynchronous write
  - [ ] Write multiple buffers
  - [ ] Write with timeout
  - [ ] Write cancellation
- [ ] **State Queries**
  - [ ] is_open() accuracy
  - [ ] available() bytes
  - [ ] Remote/local endpoint info
- [ ] **Error Conditions**
  - [ ] Connection refused
  - [ ] Connection reset
  - [ ] Timeout handling
  - [ ] Buffer overflow protection

#### test_ssl_socket.cpp
- [ ] **SSL Handshake**
  - [ ] Client handshake success
  - [ ] Server handshake success
  - [ ] Handshake failure handling
  - [ ] Certificate verification
  - [ ] Self-signed certificates
- [ ] **Encrypted Communication**
  - [ ] Read/write over SSL
  - [ ] Large data transfers
  - [ ] Binary data integrity
- [ ] **SSL-Specific Features**
  - [ ] is_secure() returns true
  - [ ] requires_handshake() behavior
  - [ ] SNI support
- [ ] **Error Handling**
  - [ ] Invalid certificates
  - [ ] Protocol mismatches
  - [x] Handshake timeouts
- [ ] **Resource Management**
  - [ ] Proper SSL cleanup
  - [ ] Memory leak prevention
  - [ ] SSL_clear safety

#### test_unix_socket.cpp
- [ ] **Connection Management**
  - [ ] Connect to Unix socket path
  - [ ] Path validation
  - [ ] Permission handling
  - [ ] Socket file creation
- [ ] **Data Transfer**
  - [ ] Read/write operations
  - [ ] Large data transfers
  - [ ] Binary data support
- [ ] **Unix-Specific Features**
  - [ ] get_remote_ip() returns path
  - [ ] Port methods return "0"
  - [ ] Socket file cleanup
- [ ] **Error Handling**
  - [ ] Invalid paths
  - [ ] Permission denied
  - [ ] Socket already exists
  - [ ] Path too long

#### test_websocket.cpp
- [ ] **WebSocket Handshake**
  - [ ] Client handshake (RFC 6455)
  - [ ] Server handshake response
  - [ ] Protocol negotiation
  - [ ] Extension negotiation
- [ ] **Frame Handling**
  - [ ] Text frames
  - [ ] Binary frames
  - [ ] Continuation frames
  - [ ] Control frames (ping/pong/close)
  - [ ] Frame masking (client)
  - [ ] Frame unmasking (server)
- [ ] **Message Assembly**
  - [ ] Single frame messages
  - [ ] Multi-frame messages
  - [ ] Message size limits
  - [ ] Fragmentation handling
- [ ] **Protocol Compliance**
  - [ ] Ping/pong mechanism
  - [ ] Close handshake
  - [ ] Invalid frame handling
  - [ ] UTF-8 validation for text frames
- [ ] **Performance**
  - [ ] Large message handling
  - [ ] Many small messages
  - [ ] Concurrent connections

### Server Tests

#### integration/listener_test.cpp
`thinger::asio::listener` (it replaced the socket server classes in 0.9), with executor
providers and with the internal acceptor thread
- [x] **Accepting**
  - [x] TCP, TLS (handed over once the handshake is done) and Unix domain sockets
  - [x] `url()` and `local_port()` (0 before `open()`, after `stop()` and for Unix)
  - [x] TCP_NODELAY set by default, left off with `tcp_no_delay = false`
  - [x] A handler throwing drops only its connection
- [x] **Opening**
  - [x] Port already in use with `max_listening_attempts = 1`; `stop()` ends the retries
  - [x] Unresolvable host; executors not on an io_context
  - [x] Used once: `open()` twice, `start()` before `open()`, `stop()` before `open()`
  - [x] Stale Unix socket file replaced; the file removed on `stop()`
- [x] **IP Filtering**
  - [x] Forbidden remotes, allowed remotes, an address not allowed
- [x] **Stopping**
  - [x] From another thread while accepting (TCP and Unix, 50 rounds)
  - [x] From the handler (plain and TLS), from two handlers at once, destroyed by the handler
  - [x] Waits for a handler running on another thread
  - [x] Against a TLS handshake ending (200 rounds per mode)
- [x] **TLS handshake deadline** (`[handshake-timeout]`)
  - [x] A silent client (or one stalled after its ClientHello) closed in time, logged at debug
  - [x] 0 keeps it open; `stop()` closes the handshakes pending

#### integration/listener_tls_test.cpp
The TLS of `thinger::asio::listener`
- [x] **Bound of the handshakes in progress** (`[handshake-bound]`, `max_pending_handshakes`)
  - [x] At a bound of 2 a third ClientHello is not answered until a slot frees (closed, reset);
    a TLS client then completes and reaches the handler (both accept modes)
  - [x] A connection waiting for a slot is closed at its deadline, counted from its accept
  - [x] Accepting goes on while connections wait; a plain listener on the same thread is not bounded
  - [x] A bound of 0 makes nothing wait; the bound is per IO thread (two threads: four at once)
  - [x] `stop()` closes the connections waiting; slots freed by deadlines and resets fill again
- [x] **Client certificates** (`[mtls]`, `[sni]`)
  - [x] Listener mode × host mode (unset, none, optional, required), with and without a certificate
  - [x] Other names (or none) get the listener's mode; two listeners of one manager keep their own
- [x] **Session cache** (`[resumption]`)
  - [x] Resumed by default (TLS 1.2 by ticket and by session id, TLS 1.3); never with the cache
    disabled, and no tickets issued
  - [x] A cache that differs from the one of the manager's other listeners: `invalid_argument`,
    nothing bound; an equal one is accepted

#### integration/server_listeners_test.cpp
`http::server_base` on several listeners, and its `connection_options` (target
`test_integration_server_listeners`; tags `[listeners]`, `[connection_options]`, `[stats]`)
- [x] **One application, several listeners** (`[listeners]`)
  - [x] One server on plain TCP and TLS: a route, a middleware and a response hook added after
    both `listen()` answer on both; `listening_on()` gives `http://` and `https://` with the
    ports the system chose
  - [x] A third listener on a Unix socket serves the same routes and shows as `unix:/path`
  - [x] A `listen()` that fails (port taken: `address_in_use`; TLS with a manager without a
    default certificate: `invalid_argument`; an endpoint the server listens on already) is not
    in `listening_on()`, and the listeners open keep serving
  - [x] A server without listeners (`server` and `pool_server`): `listening_on()` empty,
    `stop()` harmless, also after its only `listen()` failed
  - [x] `stats()` counts the connections of every listener
- [x] **Connection options** (`[connection_options]`)
  - [x] The defaults without `set_connection_options()`: 120 s, 8 MiB, 5 s, no logging
  - [x] `idle_timeout` of 1 s closes an idle connection about 1 s after its last response; the
    default keeps it open and serving (transport matrix)

Other suites on `connection_options`, per server:
- `integration/lingering_close_test.cpp` (`[linger]`): every case sets the linger of its own
  server; two servers of one process, one with linger 0 and one with the default, close as
  each is configured (transport matrix).
- `integration/body_logging_test.cpp` (`[logging]`): two servers of one process, one logging
  and one not, log as each is configured.
- `integration/server_base_test.cpp` (`[server]`, built into `all_integration_tests` only:
  `./all_integration_tests "-#" "[#server_base_test]"`): the application served by `server_base`,
  the idle timeout set through `connection_options`.
- `integration/server_basic_test.cpp`: `listen()` returns an `error_code`; the same port twice
  is `address_in_use`, a second port 0 a new listener; `listening_on()`.

#### integration/shutdown_deadline_test.cpp
`server_base::shutdown(by)`, its single deadline, and `stop_listening()` and `stop()` around it
(target `test_integration_shutdown_deadline`; tag `[deadline]`, most cases on the transport
matrix). The application awaits it with `http::test::awaited_shutdown`
(`tests/fixtures/awaited_shutdown.hpp`): `co_await shutdown(by)` on an io_context of its own
- [x] **Deadline** (`[deadline]`)
  - [x] An idle server: `true` in under 100 ms, however far `by` (10 h too), and the port
    refuses connections; also blocking a thread of the application
  - [x] `by` passed: `true` at once without connections, `false` at once with a request in flight
  - [x] A server not running: listening but `wait()` never called, and never listening
  - [x] An idle keep-alive connection closed at once, a request of 200 ms answered with
    `Connection: Close`, and `true` soon after it, long before `by`
  - [x] A handler running past `by` = 500 ms: `false` at `by` ± 50 ms, the client socket closed
    by then (20 rounds per transport)
  - [x] Blocking `shutdown(ms)` from a thread of the application: the same result and timing as
    the awaited one, clean and not clean
- [x] **Several listeners** (TCP + TLS, TCP + Unix): handlers of 300 ms on each end clean in
  ~300 ms, not 600; handlers that do not finish end at 500 ± 50 ms, not 1000, both listeners
  refusing connections
- [x] **Threads of the server**
  - [x] Awaited on the thread of a `server` and on the only worker of a `pool_server`: the other
    connections of that thread go on being served while it drains
  - [x] Blocking `shutdown(ms)` from a handler: `false` in under 50 ms, the misuse logged
    (`[logging]`), and the server closed at `by` ± 50 ms with `wait()` returning
- [x] **Quiesce, drain, release**
  - [x] `stop_listening()` refuses new connections, the one open goes on serving and `stats()`
    counts it; a later `shutdown(by)` drains the same
  - [x] `stop()` during a shutdown ends it with `false`, and `wait()` returns
  - [x] Two calls share the end of the first (a later call keeps its `by`), concurrent ones too
  - [x] `on_shutdown()` called once over every call, as the drain starts; what it throws is
    logged and the shutdown goes on
  - [x] A connection taken over that never closes is closed at `by`, with `false`

#### integration/shutdown_test.cpp
The graceful shutdown as in 0.8.1, on `co_await shutdown(by)` and the blocking `shutdown(ms)`
(target `test_integration_shutdown`; tag `[shutdown]`). Its raw clients and slow handlers are in
`integration/shutdown_test_helpers.hpp` (namespace `shutdown_test`), shared with the suite above
- [x] A request in flight finishes with `Connection: close`; idle keep-alive connections closed
  at once; a request arriving on a draining connection answered with `Connection: close`
- [x] What is left closed at the deadline; a connection taken over that ignores the shutdown too
- [x] WebSockets closed with 1001, event streams closed, a byte stream told through its scope
  (`scope.on_shutdown()`)
- [x] Called twice, `stop()` during it, from a handler without waiting, a server not running
- [x] A `pool_server`; a Unix socket; an application shutting down on a signal (and a second
  signal, or no drain); `stats()` counting connections

#### integration/server_scope_test.cpp
The `server_scope` of the connections taken over or upgraded (target
`test_integration_server_scope`; tag `[scope]`, on the transport matrix but the last case)
- [x] `take_over()`, a WebSocket byte stream, a WebSocket with the message interface and an
  event stream: counted as what they serve while open, told once as the drain starts (well
  within the grace period), and the shutdown ends clean once they close
- [x] Once `scope.closed()`: the count goes back to what it was, and a later shutdown does not
  call the callback
- [x] An `on_shutdown()` set once the drain started is still called, once (posted)
- [x] A takeover while the server closes its connections: the connection is closed and the
  handler is not called, only released once the server is destroyed
- [x] The socket handed out released without `closed()`: the connection no longer counts
- [x] A socket taken over still open at the end of the grace period is closed by the server
- [x] A scope with no server (default-constructed) or whose server was destroyed does nothing

#### integration/websocket_accept_options_test.cpp
`websocket_accept_options` on both WebSocket upgrades (target
`test_integration_websocket_accept_options`; tag `[options]`, partly on the transport matrix)
- [x] Defaults of the message interface: ping after 60 s, close after 120 s (read from the
  connection, no real wait); 0 turns each off
- [x] A 16 MiB message is delivered by default; one byte more closes with 1009
- [x] `max_message_size`, `ping_interval` and `idle_timeout` with small values, and both off
- [x] The byte stream: no keepalive unset or 0, and each option when set
- [x] `upgrade_websocket()` subprotocols with `protocol_required` true and false: none offered,
  one, several (the first the client offers that is listed), only others

#### unit/http/server/response_header_test.cpp
- [x] `response.hpp` included first and alone brings in none of the connection headers
  (`server_connection`, `http_stream`, `connection_tracker`, `response_sink`,
  `websocket_connection`, `sse_connection`, `asio/sockets/websocket.hpp`: `#error` on their
  guards), and the 8 upgrade overloads compile from it (target `test_response_header`)

#### test_server_lifecycle.cpp
- [ ] **Start/Stop Cycles**
  - [ ] Single start/stop
  - [ ] Multiple start/stop cycles
  - [ ] Rapid start/stop
  - [ ] Stop during accept
- [ ] **Connection Handling**
  - [ ] Connections during shutdown
  - [ ] Active connections on stop
  - [ ] Handler exceptions
- [ ] **Resource Management**
  - [ ] Port release on stop
  - [ ] File descriptor limits
  - [ ] Memory usage stability

### SSL/Certificate Tests

#### test_certificate_manager.cpp ✓ (existing, comprehensive; `tests/unit/asio/certificate_manager_test.cpp`)
Each test creates its own managers with `std::make_shared` (there is no process-wide
instance since 0.9), so no test saves or restores a global default.
- [x] **Certificate Storage**
- [x] **Wildcard Matching**
- [x] **SNI Callback** (driven through a ClientHello in memory: the callback needs the
  `weak_ptr` kept with the context)
- [x] **Default Certificate** (`remove_default_certificate()` included)
- [x] **Setters return `error_code`** (empty on success; `invalid_argument` keeps the
  previous state)
- [x] **Manager destroyed** while a context of its own is in use: the next handshake stays
  on that context, with no crash; the `weak_ptr` is freed with its context
- [x] **Thread Safety**

Integration (`tests/integration/tls_certificates_test.cpp`, `tls_client_verify_test.cpp`):
two managers in one process serve different certificates for the same SNI name on two
servers, and only the one with `set_client_verify()` for a host asks for a client
certificate; a server keeps serving after its manager is destroyed.

### Integration Tests

#### test_worker_socket_integration.cpp
- [ ] **Socket + Worker Integration**
  - [ ] Sockets use correct io_context
  - [ ] Connection distribution across workers
  - [ ] Load balancing verification
  - [ ] Worker shutdown with active sockets

#### test_server_client_integration.cpp
- [ ] **End-to-End Communication**
  - [ ] TCP client to TCP server
  - [ ] SSL client to SSL server
  - [ ] Unix client to Unix server
  - [ ] WebSocket client to server
- [ ] **Concurrent Connections**
  - [ ] Multiple clients to single server
  - [ ] Connection limits
  - [ ] Fair scheduling
- [ ] **Error Scenarios**
  - [ ] Server shutdown during transfer
  - [ ] Client disconnect handling
  - [ ] Network interruptions

#### test_ssl_integration.cpp
- [ ] **SSL End-to-End**
  - [ ] Certificate verification
  - [ ] SNI functionality
  - [ ] Multiple certificates
  - [ ] Certificate renewal
- [ ] **Performance Under SSL**
  - [ ] Throughput comparison
  - [ ] Handshake overhead
  - [ ] Connection pooling benefits

### Performance and Stress Tests

#### benchmarks/asio/ (separate directory)
- [ ] **Throughput Tests**
  - [ ] TCP socket throughput
  - [ ] SSL overhead measurement
  - [ ] WebSocket frame overhead
- [ ] **Latency Tests**
  - [ ] Round-trip times
  - [ ] Connection establishment time
  - [ ] SSL handshake time
- [ ] **Scalability Tests**
  - [ ] Maximum concurrent connections
  - [ ] Worker thread scaling
  - [ ] Memory usage per connection
- [ ] **Stress Tests**
  - [ ] Rapid connect/disconnect
  - [ ] Maximum throughput sustained
  - [ ] Error recovery under load

## Implementation Priority

1. **Phase 1 - Core Components** (High Priority)
   - [ ] test_io_worker.cpp
   - [ ] test_worker_thread.cpp
   - [x] unit/asio/workers_test.cpp
   - [x] integration/workers_instances_test.cpp

2. **Phase 2 - Socket Layer** (High Priority)
   - [ ] test_tcp_socket.cpp
   - [ ] test_ssl_socket.cpp
   - [ ] test_socket_base.cpp

3. **Phase 3 - Server Components** (Medium Priority)
   - [x] integration/listener_test.cpp
   - [x] integration/listener_tls_test.cpp
   - [ ] test_server_lifecycle.cpp

4. **Phase 4 - Advanced Sockets** (Medium Priority)
   - [ ] test_unix_socket.cpp
   - [ ] test_websocket.cpp

5. **Phase 5 - Integration** (Medium Priority)
   - [ ] test_worker_socket_integration.cpp
   - [ ] test_server_client_integration.cpp
   - [ ] test_ssl_integration.cpp

6. **Phase 6 - Performance** (Low Priority)
   - [ ] Benchmark suite
   - [ ] Stress tests
   - [ ] Scalability tests

## Testing Guidelines

1. **Test Naming Convention**
   - Test files: `test_<component>.cpp`
   - Test cases: `TEST_CASE("<Component> - <Feature>")`
   - Sections: `SECTION("<Specific behavior>")`

2. **Test Organization**
   - Group related tests in sections
   - Use fixtures for common setup
   - Clean up resources in destructors

3. **Assertions**
   - Use descriptive assertion messages
   - Test both positive and negative cases
   - Verify error codes and exceptions

4. **Threading Tests**
   - Use proper synchronization
   - Test race conditions
   - Verify thread safety claims

5. **Resource Tests**
   - Check for leaks (valgrind)
   - Verify cleanup on destruction
   - Test resource limits

6. **Error Injection**
   - Test timeouts
   - Test connection failures
   - Test invalid inputs
   - Test system resource exhaustion

## Coverage Goals

- **Line Coverage**: Minimum 80% for all components
- **Branch Coverage**: Minimum 75% for all components
- **Critical Paths**: 100% coverage for:
  - Connection establishment
  - Data transfer
  - Error handling
  - Resource cleanup

## Test Execution

```bash
# Run all ASIO tests
ctest -R "^test_.*asio.*"

# Run specific component tests
ctest -R "^test_tcp_socket$"

# Run with valgrind for memory checks
ctest -T memcheck -R "^test_.*asio.*"

# Run with thread sanitizer
ctest -T tsan -R "^test_.*asio.*"
```

## Notes

- Tests marked with ✓ already exist but may need expansion
- Each test file should be self-contained
- Integration tests may require longer timeouts
- Performance tests should be run separately from unit tests
- Some tests may require root privileges (Unix sockets with special permissions)
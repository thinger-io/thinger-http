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
│   ├── test_workers.cpp      ✓ (existing)
│   └── test_worker_client.cpp
├── sockets/                  # Socket abstraction tests
│   ├── test_socket_base.cpp
│   ├── test_tcp_socket.cpp
│   ├── test_ssl_socket.cpp
│   ├── test_unix_socket.cpp
│   └── test_websocket.cpp
├── servers/                  # Server implementation tests
│   ├── test_socket_server_base.cpp
│   ├── test_tcp_socket_server.cpp
│   ├── test_unix_socket_server.cpp
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

#### test_workers.cpp ✓ (existing, needs expansion)
- [x] **Singleton Pattern**
  - [x] get_workers() returns same instance
- [x] **Lifecycle**
  - [x] start()/stop() basic functionality
- [ ] **IO Context Distribution**
  - [x] get_next_io_context() round-robin
  - [ ] get_isolated_io_context() returns unique contexts
  - [ ] get_thread_io_context() thread-local storage
  - [ ] Distribution is thread-safe
- [ ] **Client Management**
  - [ ] register_client()/unregister_client()
  - [ ] Auto-start when first client registers
  - [ ] Auto-stop when last client unregisters
  - [ ] Client lifecycle notifications
- [ ] **Signal Handling**
  - [ ] wait() handles SIGINT
  - [ ] wait() handles SIGTERM
  - [ ] Signal handling doesn't interfere with clients

#### test_worker_client.cpp
- [ ] **Base Class Functionality**
  - [ ] start()/stop() lifecycle
  - [ ] is_running() state tracking
  - [ ] wait() blocks until stopped
  - [ ] notify_stopped() mechanism
- [ ] **Registration with Workers**
  - [ ] Automatic registration on start
  - [ ] Automatic unregistration on stop
  - [ ] Multiple clients can coexist
- [ ] **Virtual Method Testing**
  - [ ] get_service_name() pure virtual implementation
  - [ ] Derived class behavior

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
  - [ ] Handshake timeouts
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

#### test_socket_server_base.cpp
- [ ] **Abstract Interface**
  - [ ] Cannot instantiate directly
  - [ ] Virtual method requirements
  - [ ] Handler registration
- [ ] **IP Filtering**
  - [ ] Allowed remotes list
  - [ ] Forbidden remotes list
  - [ ] Empty lists behavior
  - [ ] IP matching logic
- [ ] **Configuration**
  - [ ] Max listening attempts
  - [ ] Handler setting
  - [ ] Thread-safe configuration updates

#### test_tcp_socket_server.cpp
- [ ] **TCP Server Basics**
  - [ ] Bind to port
  - [ ] Listen for connections
  - [ ] Accept connections
  - [ ] Multiple simultaneous connections
- [ ] **SSL Configuration**
  - [ ] Enable/disable SSL
  - [ ] SSL context setting
  - [ ] SNI callback integration
  - [ ] Client certificate requirement
- [ ] **Socket Options**
  - [ ] TCP_NODELAY propagation
  - [ ] Socket reuse
  - [ ] Backlog configuration
- [ ] **Error Handling**
  - [ ] Port already in use
  - [ ] Invalid addresses
  - [ ] Accept failures
  - [ ] SSL handshake failures
- [ ] **IO Context Providers**
  - [ ] Custom provider functions
  - [ ] Legacy constructor compatibility
  - [ ] Round-robin connection distribution

#### test_unix_socket_server.cpp
- [ ] **Unix Socket Server**
  - [ ] Bind to socket path
  - [ ] Socket file creation
  - [ ] Permission setting
  - [ ] Accept connections
- [ ] **Path Management**
  - [ ] Existing socket file removal
  - [ ] Socket file cleanup on stop
  - [ ] Path validation
- [ ] **Error Handling**
  - [ ] Permission denied
  - [ ] Path too long
  - [ ] Directory doesn't exist
  - [ ] Socket file locked

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

#### test_certificate_manager.cpp ✓ (existing, comprehensive)
- [x] **Certificate Storage**
- [x] **Wildcard Matching**
- [x] **SNI Callback**
- [x] **Default Certificate**
- [x] **Thread Safety**

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
   - [ ] Expand test_workers.cpp
   - [ ] test_worker_client.cpp

2. **Phase 2 - Socket Layer** (High Priority)
   - [ ] test_tcp_socket.cpp
   - [ ] test_ssl_socket.cpp
   - [ ] test_socket_base.cpp

3. **Phase 3 - Server Components** (Medium Priority)
   - [ ] test_tcp_socket_server.cpp
   - [ ] test_unix_socket_server.cpp
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
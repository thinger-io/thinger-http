# HTTP Async Client Examples

# Simple Async Client
add_executable(simple_async_client simple_async_client.cpp)
target_link_libraries(simple_async_client PRIVATE thinger::http)

# Concurrent Requests
add_executable(concurrent_requests concurrent_requests.cpp)
target_link_libraries(concurrent_requests PRIVATE thinger::http)

# Timeout Example
add_executable(async_timeout_example timeout_example.cpp)
target_link_libraries(async_timeout_example PRIVATE thinger::http)

# Simple Async Client (Coroutines)
add_executable(simple_async_client_coro simple_async_client_coro.cpp)
target_link_libraries(simple_async_client_coro PRIVATE thinger::http)

# Streaming Download Example
add_executable(async_streaming_download_example streaming_download_example.cpp)
target_link_libraries(async_streaming_download_example PRIVATE thinger::http)

# Streaming Download Example (Coroutines)
add_executable(async_streaming_download_example_coro streaming_download_example_coro.cpp)
target_link_libraries(async_streaming_download_example_coro PRIVATE thinger::http)

# WebSocket Client Example
add_executable(async_websocket_client_example websocket_client_example.cpp)
target_link_libraries(async_websocket_client_example PRIVATE thinger::http)

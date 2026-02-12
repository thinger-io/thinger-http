# HTTP Client Examples

# Simple HTTP Client
add_executable(simple_http_client simple_http_client.cpp)
target_link_libraries(simple_http_client PRIVATE thinger::http)

# Multiple Requests Example
add_executable(multiple_requests multiple_requests.cpp)
target_link_libraries(multiple_requests PRIVATE thinger::http)

# Timeout Example
add_executable(timeout_example timeout_example.cpp)
target_link_libraries(timeout_example PRIVATE thinger::http)

# WebSocket Client Example
add_executable(websocket_client_example websocket_client_example.cpp)
target_link_libraries(websocket_client_example PRIVATE thinger::http)

# Streaming Download Example
add_executable(streaming_download_example streaming_download_example.cpp)
target_link_libraries(streaming_download_example PRIVATE thinger::http)
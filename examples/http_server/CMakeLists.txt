cmake_minimum_required(VERSION 3.14)

# Simple HTTP Server Example
add_executable(simple_http_server simple_http_server.cpp)
target_link_libraries(simple_http_server PRIVATE thinger::http)

# Simple HTTPS Server Example
add_executable(simple_https_server simple_https_server.cpp)
target_link_libraries(simple_https_server PRIVATE thinger::http)

# Advanced HTTP Server Example with routing
add_executable(advanced_http_server advanced_http_server.cpp)
target_link_libraries(advanced_http_server PRIVATE thinger::http)

# Routing Example using new routing system
add_executable(routing_example routing_example.cpp)
target_link_libraries(routing_example PRIVATE thinger::http)

# Server class example
add_executable(server_example server_example.cpp)
target_link_libraries(server_example PRIVATE thinger::http)

# Server auth example
add_executable(server_auth_example server_auth_example.cpp)
target_link_libraries(server_auth_example PRIVATE thinger::http)

# Server CORS example
add_executable(server_cors_example server_cors_example.cpp)
target_link_libraries(server_cors_example PRIVATE thinger::http)

# Server shutdown example
add_executable(server_shutdown_example server_shutdown_example.cpp)
target_link_libraries(server_shutdown_example PRIVATE thinger::http)

# Server WebSocket example
add_executable(server_websocket_example server_websocket_example.cpp)
target_link_libraries(server_websocket_example PRIVATE thinger::http)

# Server SSE example
add_executable(server_sse_example server_sse_example.cpp)
target_link_libraries(server_sse_example PRIVATE thinger::http)

# Server Upload example (on-demand body reading with TCP backpressure)
add_executable(server_upload_example server_upload_example.cpp)
target_link_libraries(server_upload_example PRIVATE thinger::http)

# Server Streaming Upload example (deferred body with chunk-by-chunk reading)
add_executable(server_streaming_upload_example server_streaming_upload_example.cpp)
target_link_libraries(server_streaming_upload_example PRIVATE thinger::http)

# Install examples
install(TARGETS simple_http_server simple_https_server advanced_http_server
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/examples
)
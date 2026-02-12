cmake_minimum_required(VERSION 3.14)

# Logging example
add_executable(logging_example logging_example.cpp)
target_link_libraries(logging_example PRIVATE thinger::http)
# Link spdlog if logging is enabled
if(THINGER_HTTP_ENABLE_LOGGING)
    target_link_libraries(logging_example PRIVATE spdlog::spdlog)
endif()

# HTTP Client examples (synchronous)
add_subdirectory(http_client)

# HTTP Async Client examples (async/concurrent)
add_subdirectory(http_async_client)

# HTTP Server examples
add_subdirectory(http_server)

# Testing configuration
include(FetchContent)

# Fetch Catch2
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.4.0
)
FetchContent_MakeAvailable(Catch2)

# Enable CTest
include(CTest)
include(Catch)

# Create a test main library with logging initialization
add_library(test_main STATIC test_main.cpp)
target_link_libraries(test_main PUBLIC Catch2::Catch2 thinger::http spdlog::spdlog)

# Function to add a test easily
function(add_thinger_test test_name test_file)
    add_executable(${test_name} ${test_file})
    target_link_libraries(${test_name} 
        PRIVATE 
            thinger::http
            test_main
            nlohmann_json::nlohmann_json
    )
    catch_discover_tests(${test_name})
endfunction()

# ==================== UNIT TESTS ====================

# Unit tests - HTTP Client
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/client/client_test.cpp)
    add_thinger_test(test_client unit/http/client/client_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/client/client_debug_test.cpp)
    add_thinger_test(test_client_debug unit/http/client/client_debug_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/client/connection_pool_thread_safety_test.cpp)
    add_thinger_test(test_connection_pool_thread_safety unit/http/client/connection_pool_thread_safety_test.cpp)
endif()

# Unit tests - HTTP Server
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/server/server_test.cpp)
    add_thinger_test(test_server unit/http/server/server_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/server/route_test.cpp)
    add_thinger_test(test_route unit/http/server/route_test.cpp)
endif()

# Unit tests - HTTP Common
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/common/headers_test.cpp)
    add_thinger_test(test_headers unit/http/common/headers_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/common/http_request_test.cpp)
    add_thinger_test(test_http_request unit/http/common/http_request_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/common/http_response_test.cpp)
    add_thinger_test(test_http_response unit/http/common/http_response_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/common/http_data_test.cpp)
    add_thinger_test(test_http_data unit/http/common/http_data_test.cpp)
endif()

# Unit tests - ASIO
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/asio/workers_test.cpp)
    add_thinger_test(test_workers unit/asio/workers_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/asio/certificate_manager_test.cpp)
    add_thinger_test(test_certificate_manager unit/asio/certificate_manager_test.cpp)
endif()

# ==================== INTEGRATION TESTS ====================

# Integration tests - Client
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/client_wait_test.cpp)
    add_thinger_test(test_integration_client_wait integration/client_wait_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/client_timeout_test.cpp)
    add_thinger_test(test_integration_client_timeout integration/client_timeout_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/client_redirect_test.cpp)
    add_thinger_test(test_integration_client_redirect integration/client_redirect_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/client_sync_test.cpp)
    add_thinger_test(test_integration_client_sync integration/client_sync_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/client_async_test.cpp)
    add_thinger_test(test_integration_client_async integration/client_async_test.cpp)
endif()

# Integration tests - Async Client
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/async_client_wait_test.cpp)
    add_thinger_test(test_integration_async_client_wait integration/async_client_wait_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/connection_pool_test.cpp)
    add_thinger_test(test_integration_connection_pool integration/connection_pool_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/async_client_thread_affinity_test.cpp)
    add_thinger_test(test_integration_async_client_thread_affinity integration/async_client_thread_affinity_test.cpp)
endif()

# Integration tests - Server
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/server_basic_test.cpp)
    add_thinger_test(test_integration_server_basic integration/server_basic_test.cpp)
endif()

# Integration tests - Socket Pipe
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/socket_pipe_test.cpp)
    add_thinger_test(test_integration_socket_pipe integration/socket_pipe_test.cpp)
endif()

# Integration tests - Compression
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/compression_test.cpp)
    add_thinger_test(test_integration_compression integration/compression_test.cpp)
endif()

# Integration tests - Unix Socket Server
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/unix_socket_server_test.cpp)
    add_thinger_test(test_integration_unix_socket_server integration/unix_socket_server_test.cpp)
endif()

# Integration tests - Socket I/O
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/socket_io_test.cpp)
    add_thinger_test(test_integration_socket_io integration/socket_io_test.cpp)
endif()

# Integration tests - Routing
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/routing_test.cpp)
    add_thinger_test(test_integration_routing integration/routing_test.cpp)
endif()

# ==================== ALL TESTS RUNNER ====================

# Collect all test files
file(GLOB_RECURSE UNIT_TEST_FILES ${CMAKE_CURRENT_SOURCE_DIR}/unit/*_test.cpp)
file(GLOB_RECURSE INTEGRATION_TEST_FILES ${CMAKE_CURRENT_SOURCE_DIR}/integration/*_test.cpp)

# Debug: Print detected files
message(STATUS "Found ${CMAKE_CURRENT_SOURCE_DIR} unit test files: ${UNIT_TEST_FILES}")
message(STATUS "Found integration test files: ${INTEGRATION_TEST_FILES}")

# Create a test runner for all unit tests
if(UNIT_TEST_FILES)
    add_executable(all_unit_tests ${UNIT_TEST_FILES})
    target_link_libraries(all_unit_tests 
        PRIVATE 
            thinger::http
            test_main
            nlohmann_json::nlohmann_json
    )
    catch_discover_tests(all_unit_tests)
endif()

# Create a test runner for all integration tests
if(INTEGRATION_TEST_FILES)
    add_executable(all_integration_tests ${INTEGRATION_TEST_FILES})
    target_link_libraries(all_integration_tests 
        PRIVATE 
            thinger::http
            test_main
            nlohmann_json::nlohmann_json
            spdlog::spdlog
    )
    catch_discover_tests(all_integration_tests)
endif()

# Create a test runner for ALL tests
set(ALL_TEST_FILES ${UNIT_TEST_FILES} ${INTEGRATION_TEST_FILES})
if(ALL_TEST_FILES)
    add_executable(all_tests ${ALL_TEST_FILES})
    target_link_libraries(all_tests 
        PRIVATE 
            thinger::http
            test_main
            nlohmann_json::nlohmann_json
            spdlog::spdlog
    )
    catch_discover_tests(all_tests)
endif()
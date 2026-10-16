# Testing configuration
include(FetchContent)

# Fetch Catch2
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
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

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/client/response_factory_test.cpp)
    add_thinger_test(test_response_factory unit/http/client/response_factory_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/client/proxy_test.cpp)
    add_thinger_test(test_proxy unit/http/client/proxy_test.cpp)
endif()

# Unit tests - HTTP Server
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/server/server_test.cpp)
    add_thinger_test(test_server unit/http/server/server_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/server/route_test.cpp)
    add_thinger_test(test_route unit/http/server/route_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/server/route_tree_test.cpp)
    add_thinger_test(test_route_tree unit/http/server/route_tree_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/server/response_test.cpp)
    add_thinger_test(test_response unit/http/server/response_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/server/openapi_test.cpp)
    add_thinger_test(test_openapi unit/http/server/openapi_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/server/virtual_host_test.cpp)
    add_thinger_test(test_virtual_host unit/http/server/virtual_host_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/server/trusted_proxies_test.cpp)
    add_thinger_test(test_trusted_proxies unit/http/server/trusted_proxies_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/server/file_info_test.cpp)
    add_thinger_test(test_file_info unit/http/server/file_info_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/server/body_reader_test.cpp)
    add_thinger_test(test_body_reader unit/http/server/body_reader_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/server/body_decoders_test.cpp)
    add_thinger_test(test_body_decoders unit/http/server/body_decoders_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/server/request_context_test.cpp)
    add_thinger_test(test_request_context unit/http/server/request_context_test.cpp)
endif()

# Unit tests - HTTP Common
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/common/headers_test.cpp)
    add_thinger_test(test_headers unit/http/common/headers_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/common/multipart_parser_test.cpp)
    add_thinger_test(test_multipart_parser unit/http/common/multipart_parser_test.cpp)
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

# Unit tests - URL utilities
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/util/url_test.cpp)
    add_thinger_test(test_url unit/http/util/url_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/util/http_date_test.cpp)
    add_thinger_test(test_http_date unit/http/util/http_date_test.cpp)
endif()

# Unit tests - Utilities
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/util/compression_test.cpp)
    add_thinger_test(test_compression unit/util/compression_test.cpp)
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

# Integration tests - Client response framing (malformed responses, connection reuse)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/client_response_framing_test.cpp)
    add_thinger_test(test_integration_client_response_framing integration/client_response_framing_test.cpp)
endif()

# Integration tests - Client through an HTTP proxy, and CAs added to the client
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/client_proxy_test.cpp)
    add_thinger_test(test_integration_client_proxy integration/client_proxy_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/client_ca_test.cpp)
    add_thinger_test(test_integration_client_ca integration/client_ca_test.cpp)
endif()

# Integration tests - Server certificates replaced while listening
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/tls_certificates_test.cpp)
    add_thinger_test(test_integration_tls_certificates integration/tls_certificates_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/client_keepalive_test.cpp)
    add_thinger_test(test_integration_client_keepalive integration/client_keepalive_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/client_decompression_test.cpp)
    add_thinger_test(test_integration_client_decompression integration/client_decompression_test.cpp)
endif()

# Integration tests - Server
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/server_basic_test.cpp)
    add_thinger_test(test_integration_server_basic integration/server_basic_test.cpp)
endif()

# Integration tests - Socket Pipe
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/socket_pipe_test.cpp)
    add_thinger_test(test_integration_socket_pipe integration/socket_pipe_test.cpp)
endif()

# Integration tests - WebSocket as a byte stream socket
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/websocket_stream_test.cpp)
    add_thinger_test(test_integration_websocket_stream integration/websocket_stream_test.cpp)
endif()

# Integration tests - WebSocket messages through the message interface
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/websocket_message_test.cpp)
    add_thinger_test(test_integration_websocket_message integration/websocket_message_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/websocket_flow_control_test.cpp)
    add_thinger_test(test_integration_websocket_flow_control integration/websocket_flow_control_test.cpp)
endif()

# Integration tests - Sockets defined by the application
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/custom_socket_test.cpp)
    add_thinger_test(test_integration_custom_socket integration/custom_socket_test.cpp)
endif()

# Integration tests - Compression
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/compression_test.cpp)
    add_thinger_test(test_integration_compression integration/compression_test.cpp)
endif()

# Integration tests - Unix Socket Server
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/memory_dispatch_test.cpp)
    add_thinger_test(test_integration_memory_dispatch integration/memory_dispatch_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/request_context_test.cpp)
    add_thinger_test(test_integration_request_context integration/request_context_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/unix_socket_server_test.cpp)
    add_thinger_test(test_integration_unix_socket_server integration/unix_socket_server_test.cpp)
endif()

# Integration tests - Socket I/O
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/socket_io_test.cpp)
    add_thinger_test(test_integration_socket_io integration/socket_io_test.cpp)
endif()

# Integration tests - Socket servers stopped while accepting
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/socket_server_stop_test.cpp)
    add_thinger_test(test_integration_socket_server_stop integration/socket_server_stop_test.cpp)
endif()

# Integration tests - Socket servers failing to set a connection up
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/socket_server_setup_test.cpp)
    add_thinger_test(test_integration_socket_server_setup integration/socket_server_setup_test.cpp)
endif()

# Integration tests - Routing
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/routing_test.cpp)
    add_thinger_test(test_integration_routing integration/routing_test.cpp)
endif()

# Integration tests - SSE
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/sse_test.cpp)
    add_thinger_test(test_integration_sse integration/sse_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/sse_flow_control_test.cpp)
    add_thinger_test(test_integration_sse_flow_control integration/sse_flow_control_test.cpp)
endif()

# Integration tests - Schema validation
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/schema_validation_test.cpp)
    add_thinger_test(test_integration_schema_validation integration/schema_validation_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/body_decoders_test.cpp)
    add_thinger_test(test_integration_body_decoders integration/body_decoders_test.cpp)
endif()

# Integration tests - Logging of the messages
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/body_logging_test.cpp)
    add_thinger_test(test_integration_body_logging integration/body_logging_test.cpp)
endif()

# Integration tests - Virtual hosts
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/virtual_host_test.cpp)
    add_thinger_test(test_integration_virtual_host integration/virtual_host_test.cpp)
endif()

# Integration tests - Error format
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/error_format_test.cpp)
    add_thinger_test(test_integration_error_format integration/error_format_test.cpp)
endif()

# Integration tests - Trusted proxies
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/trusted_proxies_test.cpp)
    add_thinger_test(test_integration_trusted_proxies integration/trusted_proxies_test.cpp)
endif()

# Integration tests - Request framing (smuggling, chunked decoding)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/request_framing_test.cpp)
    add_thinger_test(test_integration_request_framing integration/request_framing_test.cpp)
endif()

# Integration tests - Expect: 100-continue
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/expect_continue_test.cpp)
    add_thinger_test(test_integration_expect_continue integration/expect_continue_test.cpp)
endif()

# Integration tests - multipart/form-data bodies
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/multipart_test.cpp)
    add_thinger_test(test_integration_multipart integration/multipart_test.cpp)
endif()

# Integration tests - Request body limits
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/request_body_limits_test.cpp)
    add_thinger_test(test_integration_request_body_limits integration/request_body_limits_test.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/route_limits_test.cpp)
    add_thinger_test(test_integration_route_limits integration/route_limits_test.cpp)
endif()

# Integration tests - Handler exceptions
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/handler_exceptions_test.cpp)
    add_thinger_test(test_integration_handler_exceptions integration/handler_exceptions_test.cpp)
endif()

# Integration tests - Route hardening (schemas, coroutine handlers, route storage, responses)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/route_hardening_test.cpp)
    add_thinger_test(test_integration_route_hardening integration/route_hardening_test.cpp)
endif()

# Integration tests - Static files (validators, conditional requests, ranges)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/static_files_test.cpp)
    add_thinger_test(test_integration_static_files integration/static_files_test.cpp)
endif()

# Integration tests - Reverse proxy
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/reverse_proxy_test.cpp)
    add_thinger_test(test_integration_reverse_proxy integration/reverse_proxy_test.cpp)
endif()

# Integration tests - HEAD requests
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/head_requests_test.cpp)
    add_thinger_test(test_integration_head_requests integration/head_requests_test.cpp)
endif()

# Integration tests - Graceful shutdown and statistics
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/shutdown_test.cpp)
    add_thinger_test(test_integration_shutdown integration/shutdown_test.cpp)
endif()

# Integration tests - Framing of the responses sent
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/response_framing_test.cpp)
    add_thinger_test(test_integration_response_framing integration/response_framing_test.cpp)
endif()

# Integration tests - Callbacks given as coroutines
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/coroutine_callbacks_test.cpp)
    add_thinger_test(test_integration_coroutine_callbacks integration/coroutine_callbacks_test.cpp)
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
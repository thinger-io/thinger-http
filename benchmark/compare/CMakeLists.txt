# Comparison servers: thinger-http and the same routes on Crow and cpp-httplib (see README.md).
# Standalone project: cmake -S benchmark/compare -B build -DCMAKE_BUILD_TYPE=Release
cmake_minimum_required(VERSION 3.20)
project(thinger_http_compare CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(THINGER_HTTP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(THINGER_HTTP_ENABLE_LOGGING OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. thinger_http)

add_executable(thinger_server servers/thinger_server.cpp)
target_link_libraries(thinger_server PRIVATE thinger::http)

# Header-only competitors: fetched as sources only (SOURCE_SUBDIR points nowhere)
include(FetchContent)
FetchContent_Declare(compare_asio GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-30-0 GIT_SHALLOW TRUE SOURCE_SUBDIR _none)
FetchContent_Declare(compare_crow GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG v1.2.1 GIT_SHALLOW TRUE SOURCE_SUBDIR _none)
FetchContent_Declare(compare_httplib GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.14.3 GIT_SHALLOW TRUE SOURCE_SUBDIR _none)
FetchContent_MakeAvailable(compare_asio compare_crow compare_httplib)
find_package(Threads REQUIRED)

add_executable(competitors servers/competitors.cpp)
target_include_directories(competitors PRIVATE ${compare_asio_SOURCE_DIR}/asio/include
    ${compare_crow_SOURCE_DIR}/include ${compare_httplib_SOURCE_DIR})
# TCP_NODELAY on cpp-httplib, as the others have it: without it, Nagle plus delayed ACKs
# stall its responses ~40 ms
target_compile_definitions(competitors PRIVATE ASIO_STANDALONE CPPHTTPLIB_TCP_NODELAY=true)
target_link_libraries(competitors PRIVATE nlohmann_json::nlohmann_json Threads::Threads)

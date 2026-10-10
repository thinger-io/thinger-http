cmake_minimum_required(VERSION 3.14)

# Hello World server, compared with other frameworks (see README.md)
add_executable(benchmark_thinger_http thinger-http/main.cpp)
target_link_libraries(benchmark_thinger_http PRIVATE thinger::http)

# The same Hello World server on Crow and cpp-httplib (header-only: sources are fetched, not
# added as subprojects)
include(FetchContent)
FetchContent_Declare(bench_asio GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-30-0 GIT_SHALLOW TRUE SOURCE_SUBDIR _none)
FetchContent_Declare(bench_crow GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG v1.2.1 GIT_SHALLOW TRUE SOURCE_SUBDIR _none)
FetchContent_Declare(bench_httplib GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.14.3 GIT_SHALLOW TRUE SOURCE_SUBDIR _none)
FetchContent_MakeAvailable(bench_asio bench_crow bench_httplib)
find_package(Threads REQUIRED)

add_executable(benchmark_crow crow/main.cpp)
target_include_directories(benchmark_crow PRIVATE ${bench_asio_SOURCE_DIR}/asio/include ${bench_crow_SOURCE_DIR}/include)
target_compile_definitions(benchmark_crow PRIVATE ASIO_STANDALONE)
target_link_libraries(benchmark_crow PRIVATE Threads::Threads)

add_executable(benchmark_cpp_httplib cpp-httplib/main.cpp)
target_include_directories(benchmark_cpp_httplib PRIVATE ${bench_httplib_SOURCE_DIR})
target_link_libraries(benchmark_cpp_httplib PRIVATE Threads::Threads)

# Routing micro-benchmark: route_handler::find_route() on ~200 routes
add_executable(benchmark_routing routing/routing_benchmark.cpp)
target_link_libraries(benchmark_routing PRIVATE thinger::http)

# Server with the same ~200 routes, for end-to-end load tests (wrk, bombardier)
add_executable(benchmark_routes_server routing/routes_server.cpp)
target_link_libraries(benchmark_routes_server PRIVATE thinger::http)

# Server with a few routes, for end-to-end load tests of the per-request cost
add_executable(benchmark_few_routes_server routing/few_routes_server.cpp)
target_link_libraries(benchmark_few_routes_server PRIVATE thinger::http)

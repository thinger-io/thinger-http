cmake_minimum_required(VERSION 3.14)

# Hello World server, compared with other frameworks (see README.md)
add_executable(benchmark_thinger_http thinger-http/main.cpp)
target_link_libraries(benchmark_thinger_http PRIVATE thinger::http)

# Routing micro-benchmark: route_handler::find_route() on ~200 routes
add_executable(benchmark_routing routing/routing_benchmark.cpp)
target_link_libraries(benchmark_routing PRIVATE thinger::http)

# Server with the same ~200 routes, for end-to-end load tests (wrk, bombardier)
add_executable(benchmark_routes_server routing/routes_server.cpp)
target_link_libraries(benchmark_routes_server PRIVATE thinger::http)

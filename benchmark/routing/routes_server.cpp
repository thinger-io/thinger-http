// Server with about 200 routes in the style of the Thinger API, plus GET /hello registered
// after them, for end-to-end load tests of routing (wrk, bombardier).
//
// Usage: benchmark_routes_server [port] [threads]

#include "api_routes.hpp"
#include <thinger/http.hpp>
#include <cstdlib>
#include <iostream>

using namespace thinger;

int main(int argc, char* argv[]) {
    uint16_t port = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : 9090;
    size_t threads = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : std::thread::hardware_concurrency();
    asio::get_workers().start(threads);

    http::pool_server srv;
    for (const auto& [method, pattern] : benchmark::api_routes()) {
        srv.router()[method][pattern] = [](http::response& res) {
            res.send("Hello World!");
        };
    }
    srv.get("/hello", [](http::response& res) {
        res.send("Hello World!");
    });

    std::cout << "Server with routes running at http://localhost:" << port << " (" << threads << " threads)" << std::endl;
    srv.start("0.0.0.0", port);
    return 0;
}

// Server with a few routes (static, parameters, JSON body and coroutine), for end-to-end
// load tests of the per-request cost when routing is trivial (wrk, bombardier).
//
// Usage: benchmark_few_routes_server [port] [threads]

#include <thinger/http.hpp>
#include <cstdlib>
#include <iostream>

using namespace thinger;

int main(int argc, char* argv[]) {
    uint16_t port = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : 9090;
    size_t threads = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : std::thread::hardware_concurrency();
    asio::get_workers().start(threads);

    http::pool_server srv;
    srv.get("/hello", [](http::response& res) {
        res.send("Hello World!");
    });
    srv.get("/v1/users/:user/devices/:device", [](http::request& req, http::response& res) {
        res.json({{"user", req["user"]}, {"device", req["device"]}});
    });
    srv.post("/v1/echo", [](nlohmann::json& body, http::response& res) {
        res.json(body);
    });
    srv.get("/co", [](http::response& res) -> thinger::awaitable<void> {
        res.send("co");
        co_return;
    });

    std::cout << "Server with a few routes running at http://localhost:" << port << " (" << threads << " threads)" << std::endl;
    srv.start("0.0.0.0", port);
    return 0;
}

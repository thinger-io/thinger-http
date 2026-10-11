// thinger-http server with the comparison routes. Usage: thinger_server <threads> <port>
#include <thinger/http.hpp>
#include <thinger/asio/workers.hpp>
#include <nlohmann/json.hpp>
#include <cstdlib>

using namespace thinger;

int main(int argc, char** argv) {
    int threads = argc > 1 ? std::atoi(argv[1]) : 8;
    int port = argc > 2 ? std::atoi(argv[2]) : 9080;
    asio::get_workers().start(threads);

    http::pool_server server;

    server.get("/hello", [](http::request&, http::response& res) {
        res.send("Hello World!");
    });

    // path parameters and a small JSON answer
    server.get("/v1/users/:user/devices/:device", [](http::request& req, http::response& res) {
        res.json({{"user", req["user"]}, {"device", req["device"]}, {"connected", true}});
    });

    // JSON body parsed (within the default limits) and echoed back
    server.post("/v1/echo", [](http::request&, nlohmann::json& body, http::response& res) {
        res.json(body);
    });

    server.start("0.0.0.0", port);
    return 0;
}

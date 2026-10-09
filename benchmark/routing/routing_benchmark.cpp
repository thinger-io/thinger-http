// Routing micro-benchmark: time route_handler::find_route() (route matching and parameter
// extraction) on a table of about 200 routes in the style of the Thinger API, for routes
// registered at the start and at the end of the table, static routes, wildcards and
// unmatched paths, and on a router with a few routes.
//
// Usage: benchmark_routing [iterations per case] [rounds]

#include "api_routes.hpp"
#include <thinger/http/server/routing/route_handler.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace thinger;

namespace {

struct bench_case {
    std::string name;
    http::route_handler* router;
    http::method method;
    std::string path;
    bool expect_match;
};

std::shared_ptr<http::request> make_request(http::method method, const std::string& path) {
    auto http_request = http::http_request::create_http_request(method, "http://localhost" + path);
    return std::make_shared<http::request>(nullptr, nullptr, http_request);
}

// Nanoseconds per find_route() call: best of several rounds
double run_case(const bench_case& c, size_t iterations, size_t rounds) {
    auto req = make_request(c.method, c.path);
    if ((c.router->find_route(req) != nullptr) != c.expect_match) {
        std::fprintf(stderr, "unexpected result for %s %s\n", c.name.c_str(), c.path.c_str());
        std::exit(1);
    }
    double best = 1e18;
    for (size_t round = 0; round < rounds; ++round) {
        size_t matched = 0;
        auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < iterations; ++i) {
            matched += c.router->find_route(req) != nullptr;
        }
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (matched != (c.expect_match ? iterations : 0)) std::exit(1);
        best = std::min(best, std::chrono::duration<double, std::nano>(elapsed).count() / iterations);
    }
    return best;
}

void noop(http::response&) {}

} // namespace

int main(int argc, char* argv[]) {
    size_t iterations = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 200000;
    size_t rounds = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 5;

    // ~200 routes, as the Thinger API
    http::route_handler api;
    auto routes = benchmark::api_routes();
    for (const auto& [method, pattern] : routes) {
        api[method][pattern] = noop;
    }

    // A few routes, as a small service
    http::route_handler small;
    small[http::method::GET]["/hello"] = noop;
    small[http::method::GET]["/users/:user"] = noop;
    small[http::method::POST]["/users/:user"] = noop;
    small[http::method::GET]["/users/:user/devices/:device([a-zA-Z0-9_-]{1,32})"] = noop;
    small[http::method::GET]["/static/:path(.+)"] = noop;

    const std::vector<bench_case> cases = {
        {"api: first route (constrained params)", &api, http::method::GET, "/v1/users/alice/devices/sensor-1", true},
        {"api: plain params", &api, http::method::GET, "/v1/users/alice/devices/sensor-1/resources/temperature", true},
        {"api: bucket data", &api, http::method::GET, "/v1/users/alice/buckets/weather/data", true},
        {"api: wildcard file path", &api, http::method::GET, "/v1/users/alice/storages/files-1/files/dir/sub/file.txt", true},
        {"api: late static route", &api, http::method::GET, "/v1/server/version", true},
        {"api: last route", &api, http::method::GET, "/v1/users/alice/devices/sensor-1/projects", true},
        {"api: unmatched path (404)", &api, http::method::GET, "/v1/users/alice/unknown/thing", false},
        {"few routes: static", &small, http::method::GET, "/hello", true},
        {"few routes: last route", &small, http::method::GET, "/static/css/site.css", true},
    };

    std::printf("%zu routes registered (%zu GET); %zu iterations x %zu rounds per case (best round)\n\n",
                routes.size(), api.get_routes().at(http::method::GET).size(), iterations, rounds);
    std::printf("%-42s %12s\n", "case", "ns/lookup");
    for (const auto& c : cases) {
        std::printf("%-42s %12.1f\n", c.name.c_str(), run_case(c, iterations, rounds));
    }
    return 0;
}

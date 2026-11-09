// Time to register 500 routes in an http_application: building each route and adding it to
// the routing tree. Two sets of routes: a mix (literal paths, parameters, parameters with a
// regex and wildcards) and one with no parameter regex.
//
// Usage: benchmark_route_registration [rounds]

#include <thinger/http/server/http_application.hpp>
#include <thinger/http/server/response.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

using namespace thinger;

namespace {

constexpr size_t routes_per_set = 500;

std::vector<std::string> plain_routes() {
    std::vector<std::string> routes;
    for (size_t i = 0; i < routes_per_set; ++i) {
        auto id = std::to_string(i);
        routes.push_back(i % 2 ? "/v1/static" + id + "/info"
                               : "/v1/users/:user/things" + id + "/:thing");
    }
    return routes;
}

std::vector<std::string> mixed_routes() {
    std::vector<std::string> routes;
    for (size_t i = 0; i < routes_per_set; ++i) {
        auto id = std::to_string(i);
        switch (i % 10) {
            case 0: case 1: case 2: case 3:
                routes.push_back("/v1/static" + id + "/info");
                break;
            case 4: case 5: case 6:
                routes.push_back("/v1/users/:user/things" + id + "/:thing");
                break;
            case 7: case 8:
                routes.push_back("/v1/items" + id + "/:id([0-9]+)/:slug([a-z0-9-]{1,32})");
                break;
            default:
                routes.push_back("/v1/files" + id + "/:path(.+)");
                break;
        }
    }
    return routes;
}

// Microseconds to register the routes, one value per round
std::vector<double> run_set(const std::vector<std::string>& routes, size_t rounds) {
    std::vector<double> samples;
    for (size_t round = 0; round < rounds; ++round) {
        http::http_application app;
        auto start = std::chrono::steady_clock::now();
        for (const auto& pattern : routes) {
            app.get(pattern, [](http::response& res) { res.send("ok"); });
        }
        auto elapsed = std::chrono::steady_clock::now() - start;
        samples.push_back(std::chrono::duration<double, std::micro>(elapsed).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples;
}

void report(const char* name, const std::vector<std::string>& routes, size_t rounds) {
    auto samples = run_set(routes, rounds);
    double best = samples.front();
    double median = samples[samples.size() / 2];
    std::printf("%-28s %12.2f %12.2f %12.2f %12.2f\n", name, best / 1000, median / 1000,
                best / routes.size(), median / routes.size());
}

} // namespace

int main(int argc, char* argv[]) {
    size_t rounds = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 20;
    std::printf("%zu routes per set, %zu rounds\n\n", routes_per_set, rounds);
    std::printf("%-28s %12s %12s %12s %12s\n", "set", "best ms", "median ms", "best us/rt", "median us/rt");
    report("mixed", mixed_routes(), rounds);
    report("without parameter regex", plain_routes(), rounds);
    return 0;
}

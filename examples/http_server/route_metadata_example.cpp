// Route metadata example
//
// Routes carry free metadata (here, the permission each one requires) that the library
// does not interpret. An authorization middleware reads it from the matched route and
// answers 403 when the caller lacks the permission. Routes also carry documentation
// fields, listed at /routes from the router.
//
// Try:
//   curl -i localhost:8090/devices/sensor1                          -> 403
//   curl -i -H "Authorization: reader" localhost:8090/devices/sensor1  -> 200
//   curl -i -X DELETE -H "Authorization: reader" localhost:8090/devices/sensor1 -> 403
//   curl -i -X DELETE -H "Authorization: admin" localhost:8090/devices/sensor1  -> 200
//   curl localhost:8090/routes

#include <thinger/http_server.hpp>
#include <iostream>
#include <map>
#include <set>

using namespace thinger;

int main() {
    http::server server;

    // Permissions granted to each token (a real server would look them up asynchronously)
    std::map<std::string, std::set<std::string>> grants = {
        {"reader", {"Device:Read"}},
        {"admin", {"Device:Read", "Device:Delete"}},
    };

    // Authorization middleware: the library knows nothing about permissions, it only
    // exposes the metadata of the route that matched the request
    server.use([&grants](http::request& req, http::response& res) -> thinger::awaitable<bool> {
        auto* route = req.get_matched_route();
        if (!route || !route->has_meta("permission")) co_return true;

        auto permission = route->get_meta("permission").get<std::string>();
        auto it = grants.find(req.header("Authorization"));
        if (it == grants.end() || !it->second.contains(permission)) {
            res.error(http::http_response::status::forbidden, "Missing permission " + permission);
            co_return false;
        }
        co_return true;
    });

    // Routes sharing a prefix and a documentation tag
    auto devices = server.group("/devices").tag("Devices");

    devices.get("/:device", [](http::request& req, http::response& res) {
        res.json({{"device", req["device"]}});
    })
    .summary("Get a device")
    .path_param("device", "Device identifier")
    .returns(200, "The device")
    .meta("permission", "Device:Read");

    devices.del("/:device", [](http::request& req, http::response& res) {
        res.json({{"deleted", req["device"]}});
    })
    .summary("Delete a device")
    .path_param("device", "Device identifier")
    .returns(200, "Device deleted")
    .meta("permission", "Device:Delete");

    // List registered routes with their documentation and metadata
    server.get("/routes", [&server](http::response& res) {
        nlohmann::json routes = nlohmann::json::array();
        for (const auto& [method, method_routes] : server.router().get_routes()) {
            for (const auto& route : method_routes) {
                routes.push_back({
                    {"method", http::get_method(method)},
                    {"path", route.get_pattern()},
                    {"summary", route.get_summary()},
                    {"tags", route.get_tags()},
                    {"meta", route.get_metadata()}
                });
            }
        }
        res.json(routes);
    }).summary("List routes");

    std::cout << "Listening on http://localhost:8090" << std::endl;
    server.start(8090);
    return 0;
}

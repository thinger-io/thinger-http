#ifndef THINGER_HTTP_BENCHMARK_API_ROUTES_HPP
#define THINGER_HTTP_BENCHMARK_API_ROUTES_HPP

#include <thinger/http/common/http_request.hpp>
#include <string>
#include <utility>
#include <vector>

// About 200 routes in the style of the Thinger API: resources owned by users, identified
// by constrained parameters, with nested resources, files under wildcard paths and a few
// static server routes. Shared by the routing micro-benchmark and the routes server.
namespace thinger::benchmark {

#define BENCH_ID(name) ":" name "([a-zA-Z0-9_-]{1,32})"
#define BENCH_USER     BENCH_ID("user")
#define BENCH_DEVICE   BENCH_ID("device")
#define BENCH_BUCKET   BENCH_ID("bucket")
#define BENCH_PRODUCT  BENCH_ID("product")
#define BENCH_FILE     ":file(.+)"

inline std::vector<std::pair<http::method, std::string>> api_routes() {
    using http::method;
    std::vector<std::pair<method, std::string>> routes;

    // Specific routes first, as registered by the Thinger server modules
    const std::vector<std::pair<method, std::string>> specific = {
        {method::GET, "/v1/users/" BENCH_USER "/devices/" BENCH_DEVICE},
        {method::GET, "/v1/users/" BENCH_USER "/devices/" BENCH_DEVICE "/stats"},
        {method::GET, "/v1/users/" BENCH_USER "/devices/" BENCH_DEVICE "/tokens"},
        {method::POST, "/v1/users/" BENCH_USER "/devices/" BENCH_DEVICE "/tokens"},
        {method::DELETE, "/v1/users/" BENCH_USER "/devices/" BENCH_DEVICE "/tokens/" BENCH_ID("token")},
        {method::GET, "/v1/users/:user/devices/:device/resources/:resource"},
        {method::POST, "/v1/users/:user/devices/:device/resources/:resource"},
        {method::GET, "/v1/users/" BENCH_USER "/buckets/" BENCH_BUCKET "/data"},
        {method::POST, "/v1/users/" BENCH_USER "/buckets/" BENCH_BUCKET "/data"},
        {method::DELETE, "/v1/users/" BENCH_USER "/buckets/" BENCH_BUCKET "/data"},
        {method::GET, "/v1/users/" BENCH_USER "/buckets/" BENCH_BUCKET "/exports"},
        {method::POST, "/v1/users/" BENCH_USER "/buckets/" BENCH_BUCKET "/exports"},
        {method::GET, "/v1/users/" BENCH_USER "/buckets/" BENCH_BUCKET "/exports/" BENCH_FILE},
        {method::DELETE, "/v1/users/" BENCH_USER "/buckets/" BENCH_BUCKET "/exports/" BENCH_FILE},
        {method::POST, "/v1/users/" BENCH_USER "/buckets/" BENCH_BUCKET "/imports"},
        {method::GET, "/v1/users/" BENCH_USER "/buckets/" BENCH_BUCKET "/tags"},
        {method::GET, "/v1/users/" BENCH_USER "/buckets/" BENCH_BUCKET "/tags/" BENCH_ID("tag")},
        {method::GET, "/v1/users/" BENCH_USER "/account"},
        {method::GET, "/v1/users/" BENCH_USER "/alarms/instances/stats"},
        {method::POST, "/v1/users/" BENCH_USER "/alarms/rules/" BENCH_ID("rule") "/test-notification"},
        {method::POST, "/v1/users/" BENCH_USER "/alarms/rules/" BENCH_ID("rule") "/test-rule"},
        {method::GET, "/v1/users/" BENCH_USER "/assets/locations"},
        {method::GET, "/v1/users/" BENCH_USER "/email"},
        {method::GET, "/v1/users/" BENCH_USER "/events"},
        {method::GET, "/v1/users/" BENCH_USER "/groups/" BENCH_ID("group") "/locations"},
        {method::GET, "/v1/users/" BENCH_USER "/info/devices/locations"},
        {method::GET, "/v1/users/" BENCH_USER "/info/devices/stats"},
        {method::GET, "/v1/users/" BENCH_USER "/limits"},
        {method::GET, "/v1/users/" BENCH_USER "/limits/" BENCH_ID("resource")},
        {method::GET, "/v1/users/" BENCH_USER "/mfa"},
        {method::DELETE, "/v1/users/" BENCH_USER "/mfa/totp"},
        {method::POST, "/v1/users/" BENCH_USER "/mfa/totp/setup"},
        {method::POST, "/v1/users/" BENCH_USER "/mfa/totp/verify"},
        {method::GET, "/v1/users/" BENCH_USER "/passkeys"},
        {method::PUT, "/v1/users/" BENCH_USER "/passkeys/:credential([a-zA-Z0-9_=-]{1,512})"},
        {method::DELETE, "/v1/users/" BENCH_USER "/passkeys/:credential([a-zA-Z0-9_=-]{1,512})"},
        {method::PUT, "/v1/users/" BENCH_USER "/password"},
        {method::GET, "/v1/users/" BENCH_USER "/profile"},
        {method::PUT, "/v1/users/" BENCH_USER "/profile"},
        {method::GET, "/v1/users/" BENCH_USER "/stats"},
        {method::GET, "/v1/users/" BENCH_USER "/timezone"},
        {method::PUT, "/v1/users/" BENCH_USER "/timezone"},
        {method::GET, "/v1/users/" BENCH_USER "/plugins/" BENCH_ID("plugin") "/files/" BENCH_FILE},
        {method::GET, "/v1/users/" BENCH_USER "/plugins/" BENCH_ID("plugin") "/terminal"},
        {method::GET, "/v1/users/" BENCH_USER "/plugins/" BENCH_ID("plugin") "/terminal/logs"},
        {method::GET, "/v1/users/" BENCH_USER "/products/" BENCH_PRODUCT "/export"},
        {method::GET, "/v1/users/" BENCH_USER "/products/" BENCH_PRODUCT "/firmwares"},
        {method::POST, "/v1/users/" BENCH_USER "/products/" BENCH_PRODUCT "/firmwares"},
        {method::GET, "/v1/users/" BENCH_USER "/products/" BENCH_PRODUCT "/profile/code"},
        {method::PUT, "/v1/users/" BENCH_USER "/products/" BENCH_PRODUCT "/profile/code"},
        {method::GET, "/v1/users/" BENCH_USER "/products/" BENCH_PRODUCT "/profile/:category(properties|buckets|flows|endpoints|api)"},
        {method::GET, "/v1/users/" BENCH_USER "/products/" BENCH_PRODUCT "/profile/:category(properties|buckets|flows|endpoints|api)/" BENCH_ID("resource")},
        {method::PUT, "/v1/users/" BENCH_USER "/products/" BENCH_PRODUCT "/profile/:category(properties|buckets|flows|endpoints|api)/" BENCH_ID("resource")},
        {method::GET, "/v1/users/" BENCH_USER "/products/" BENCH_PRODUCT "/services"},
        {method::GET, "/v1/users/" BENCH_USER "/products/" BENCH_PRODUCT "/services/" BENCH_ID("service")},
        {method::GET, "/v1/users/" BENCH_USER "/storages/" BENCH_ID("storage") "/files"},
        {method::GET, "/v1/users/" BENCH_USER "/storages/" BENCH_ID("storage") "/files/" BENCH_FILE},
        {method::PUT, "/v1/users/" BENCH_USER "/storages/" BENCH_ID("storage") "/files/" BENCH_FILE},
        {method::DELETE, "/v1/users/" BENCH_USER "/storages/" BENCH_ID("storage") "/files/" BENCH_FILE},
        {method::POST, "/v1/users/" BENCH_USER "/endpoints/" BENCH_ID("endpoint") "/call"},
        {method::GET, "/v1/users/" BENCH_USER "/types/" BENCH_ID("type") "/locations"},
        {method::GET, "/v2/users/" BENCH_USER "/devices/" BENCH_DEVICE "/:resource(.+)"},
        {method::POST, "/v2/users/" BENCH_USER "/devices/" BENCH_DEVICE "/:resource(.+)"},
        {method::GET, "/v2/users/" BENCH_USER "/events"},
        {method::GET, "/v3/users/" BENCH_USER "/devices/" BENCH_DEVICE "/buckets/" BENCH_BUCKET "/tags"},
        {method::GET, "/v3/users/" BENCH_USER "/devices/" BENCH_DEVICE "/callback"},
        {method::PUT, "/v3/users/" BENCH_USER "/devices/" BENCH_DEVICE "/callback"},
        {method::GET, "/v3/users/" BENCH_USER "/devices/" BENCH_DEVICE "/callback/data"},
        {method::POST, "/v3/users/" BENCH_USER "/devices/" BENCH_DEVICE "/callback/data"},
        {method::GET, "/v3/users/" BENCH_USER "/devices/" BENCH_DEVICE "/resources"},
        {method::GET, "/v3/users/" BENCH_USER "/devices/" BENCH_DEVICE "/resources/" BENCH_ID("resource")},
        {method::POST, "/v3/users/" BENCH_USER "/devices/" BENCH_DEVICE "/resources/" BENCH_ID("resource")},
        {method::GET, "/v3/users/" BENCH_USER "/devices/" BENCH_DEVICE "/services"},
        {method::GET, "/v3/users/" BENCH_USER "/devices/" BENCH_DEVICE "/services/" BENCH_ID("service")},
        {method::GET, "/v3/users/" BENCH_USER "/devices/" BENCH_DEVICE "/stats"},
        {method::GET, "/v3/users/" BENCH_USER "/devices/" BENCH_DEVICE "/files/" BENCH_FILE},
        {method::PUT, "/v3/users/" BENCH_USER "/devices/" BENCH_DEVICE "/files/" BENCH_FILE},
        {method::GET, "/oauth/callback"},
        {method::POST, "/oauth/token"},
        {method::POST, "/oauth/device/authorize"},
        {method::GET, "/v1/oauth/providers"},
        {method::GET, "/v1/oauth/providers/" BENCH_ID("provider")},
        {method::GET, "/v1/claims/" BENCH_ID("claim")},
        {method::POST, "/v1/claims"},
        {method::GET, "/v1/server/actions"},
        {method::GET, "/v1/server/events"},
        {method::GET, "/v1/server/healthcheck"},
        {method::GET, "/v1/server/settings"},
        {method::GET, "/v1/server/statistics"},
        {method::GET, "/v1/server/version"},
        {method::GET, "/brand/" BENCH_FILE},
        {method::GET, "/robots.txt"},
    };
    routes.insert(routes.end(), specific.begin(), specific.end());

    // Generic CRUD routes of the user resources, registered last
    const std::vector<std::pair<std::string, std::string>> resources = {
        {"assets", "asset"}, {"asset_groups", "asset_group"}, {"asset_types", "asset_type"},
        {"buckets", "bucket"}, {"dashboards", "dashboard"}, {"endpoints", "endpoint"},
        {"tokens", "token"}, {"products", "product"}, {"plugins", "plugin"},
        {"proxies", "proxy"}, {"projects", "project"}, {"storages", "storage"},
        {"domains", "domain"}, {"devices", "device"},
    };
    for (const auto& [collection, item] : resources) {
        const std::string list = "/v1/users/" BENCH_USER "/" + collection;
        const std::string full = list + "/:" + item + "([a-zA-Z0-9_-]{1,32})";
        routes.emplace_back(method::GET, list);
        routes.emplace_back(method::POST, list);
        routes.emplace_back(method::GET, full);
        routes.emplace_back(method::PUT, full);
        routes.emplace_back(method::PATCH, full);
        routes.emplace_back(method::DELETE, full);
        routes.emplace_back(method::GET, full + "/projects");
        routes.emplace_back(method::POST, full + "/clone");
    }
    return routes;
}

} // namespace thinger::benchmark

#endif // THINGER_HTTP_BENCHMARK_API_ROUTES_HPP

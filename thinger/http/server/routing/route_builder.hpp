#ifndef THINGER_HTTP_ROUTE_BUILDER_HPP
#define THINGER_HTTP_ROUTE_BUILDER_HPP

#include <deque>
#include <string>
#include "route.hpp"
#include "../../common/http_request.hpp"

namespace thinger::http {

class route_builder {
public:
    route_builder(method http_method, std::deque<route>& routes, const nlohmann::json* schema_components = nullptr)
        : method_(http_method), routes_(routes), schema_components_(schema_components) {}

    // Create a new route with the given pattern
    route& operator[](const std::string& pattern) {
        routes_.emplace_back(pattern);
        routes_.back().set_schema_components(schema_components_);
        return routes_.back();
    }

private:
    method method_;
    std::deque<route>& routes_;
    const nlohmann::json* schema_components_;
};

} // namespace thinger::http

#endif // THINGER_HTTP_ROUTE_BUILDER_HPP
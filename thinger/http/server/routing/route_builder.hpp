#ifndef THINGER_HTTP_ROUTE_BUILDER_HPP
#define THINGER_HTTP_ROUTE_BUILDER_HPP

#include <deque>
#include <string>
#include "route.hpp"
#include "route_tree.hpp"
#include "../../common/http_request.hpp"

namespace thinger::http {

class route_builder {
public:
    route_builder(method http_method, std::deque<route>& routes, route_tree& tree,
                  const nlohmann::json* schema_components = nullptr)
        : method_(http_method), routes_(routes), tree_(tree), schema_components_(schema_components) {}

    // Create a new route with the given pattern (throws std::regex_error or
    // std::invalid_argument if the pattern is not valid)
    route& operator[](const std::string& pattern) {
        auto& added = routes_.emplace_back(pattern);
        added.set_schema_components(schema_components_);
        try {
            tree_.insert(added, get_method(method_));
        } catch (...) {
            routes_.pop_back();
            throw;
        }
        return added;
    }

private:
    method method_;
    std::deque<route>& routes_;
    route_tree& tree_;
    const nlohmann::json* schema_components_;
};

} // namespace thinger::http

#endif // THINGER_HTTP_ROUTE_BUILDER_HPP
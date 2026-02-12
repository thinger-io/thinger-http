#ifndef THINGER_HTTP_ROUTE_BUILDER_HPP
#define THINGER_HTTP_ROUTE_BUILDER_HPP

#include <vector>
#include <string>
#include "route.hpp"
#include "../../common/http_request.hpp"

namespace thinger::http {

class route_builder {
public:
    route_builder(method http_method, std::vector<route>& routes)
        : method_(http_method), routes_(routes) {}
    
    // Create a new route with the given pattern
    route& operator[](const std::string& pattern) {
        routes_.emplace_back(pattern);
        return routes_.back();
    }
    
private:
    method method_;
    std::vector<route>& routes_;
};

} // namespace thinger::http

#endif // THINGER_HTTP_ROUTE_BUILDER_HPP
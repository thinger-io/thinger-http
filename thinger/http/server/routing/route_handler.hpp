#ifndef THINGER_HTTP_ROUTE_HANDLER_HPP
#define THINGER_HTTP_ROUTE_HANDLER_HPP

#include <map>
#include <vector>
#include <memory>
#include "route.hpp"
#include "route_builder.hpp"

namespace thinger::http {

class route_handler {
public:
    route_handler();
    virtual ~route_handler() = default;
    
    // Access route builders for different HTTP methods
    route_builder operator[](method http_method);

    // Find the matching route for a request (without executing the handler)
    const route* find_route(std::shared_ptr<request> req);

    // Handle an unmatched request (fallback handler, or 404/405) through the response
    void handle_unmatched(std::shared_ptr<request> req, response& res);

    // Enable CORS support: answer preflight OPTIONS requests on any path
    void enable_cors(bool enabled = true);
    
    // Add a catch-all handler for unmatched routes
    void set_fallback_handler(std::function<void(request&, response&)> handler);
    
    // Get all registered routes (useful for API documentation)
    const std::map<method, std::vector<route>>& get_routes() const { return routes_; }

    // Shared JSON schemas (OpenAPI components), referenced from route schemas with
    // {"$ref": "#/components/schemas/<name>"}. Register them before the routes using them.
    void add_schema_component(const std::string& name, nlohmann::json schema) {
        schema_components_[name] = std::move(schema);
    }
    const nlohmann::json& get_schema_components() const { return schema_components_; }
    
private:
    std::map<method, std::vector<route>> routes_;
    nlohmann::json schema_components_ = nlohmann::json::object();
    std::function<void(request&, response&)> fallback_handler_;
    
    // Friend class to allow route_builder to access routes_
    friend class route_builder;
};

} // namespace thinger::http

#endif // THINGER_HTTP_ROUTE_HANDLER_HPP

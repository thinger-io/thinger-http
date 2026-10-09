#ifndef THINGER_HTTP_ROUTE_HANDLER_HPP
#define THINGER_HTTP_ROUTE_HANDLER_HPP

#include <deque>
#include <map>
#include <memory>
#include "route.hpp"
#include "route_builder.hpp"

namespace thinger::http {

class route_handler {
public:
    // Fallback for unmatched requests
    using fallback_handler = route_callback_awaitable;

    route_handler();
    virtual ~route_handler() = default;
    
    // Access route builders for different HTTP methods
    route_builder operator[](method http_method);

    // Find the matching route for a request (without executing the handler)
    const route* find_route(std::shared_ptr<request> req);

    // Handle an unmatched request (fallback handler, or 404/405) through the response
    thinger::awaitable<void> handle_unmatched(std::shared_ptr<request> req, response& res);

    // Enable CORS support: answer preflight OPTIONS requests on any path
    void enable_cors(bool enabled = true);
    
    // Add a catch-all handler for unmatched routes, taking (request&, response&) or
    // (response&), synchronous or coroutine
    template<typename F> requires request_response_callback<F> || response_callback<F>
    void set_fallback_handler(F&& handler) {
        fallback_handler_ = make_route_callback(std::forward<F>(handler));
    }
    
    // Get all registered routes (useful for API documentation). Routes are never moved
    // once registered: references to them stay valid while more routes are added.
    const std::map<method, std::deque<route>>& get_routes() const { return routes_; }

    // Shared JSON schemas (OpenAPI components), referenced from route schemas with
    // {"$ref": "#/components/schemas/<name>"}. Register them before the routes using them.
    void add_schema_component(const std::string& name, nlohmann::json schema) {
        schema_components_[name] = std::move(schema);
    }
    const nlohmann::json& get_schema_components() const { return schema_components_; }
    
private:
    std::map<method, std::deque<route>> routes_;
    nlohmann::json schema_components_ = nlohmann::json::object();
    fallback_handler fallback_handler_;
    
    // Friend class to allow route_builder to access routes_
    friend class route_builder;
};

} // namespace thinger::http

#endif // THINGER_HTTP_ROUTE_HANDLER_HPP

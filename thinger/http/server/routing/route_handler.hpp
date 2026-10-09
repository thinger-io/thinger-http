#ifndef THINGER_HTTP_ROUTE_HANDLER_HPP
#define THINGER_HTTP_ROUTE_HANDLER_HPP

#include <deque>
#include <map>
#include <memory>
#include "route.hpp"
#include "route_builder.hpp"
#include "route_tree.hpp"

namespace thinger::http {

class route_handler {
public:
    // Fallback for unmatched requests
    using fallback_handler = route_callback_awaitable;

    route_handler();
    virtual ~route_handler() = default;
    
    // Access route builders for different HTTP methods
    route_builder operator[](method http_method);

    // Find the matching route for a request (without executing the handler), setting it
    // and its path parameters on the request. Routes are matched by path segments, by
    // priority: static segments, then segments with a regex constraint, then parameters,
    // then parameters that may match '/' (see route_tree), whatever their registration order.
    const route* find_route(std::shared_ptr<request> req);

    // Handle an unmatched request (fallback handler, or 404/405) through the response
    thinger::awaitable<void> handle_unmatched(std::shared_ptr<request> req, response& res);

    // Enable CORS support: answer preflight OPTIONS requests on any path
    void enable_cors(bool enabled = true);
    
    // Add a catch-all handler for unmatched routes, taking (request&, response&) or
    // (response&), synchronous or coroutine. An empty one (nullptr, or an empty
    // std::function) removes it: unmatched requests are answered 404/405 again.
    template<request_handler_callable F>
    void set_fallback_handler(F&& handler) {
        fallback_handler_ = make_route_callback(std::forward<F>(handler));
    }
    void set_fallback_handler(std::nullptr_t) { fallback_handler_ = nullptr; }
    
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
    // The same routes, by method, as trees of path segments for matching
    std::map<method, route_tree> trees_;
    nlohmann::json schema_components_ = nlohmann::json::object();
    fallback_handler fallback_handler_;
    
    // Friend class to allow route_builder to access routes_
    friend class route_builder;
};

} // namespace thinger::http

#endif // THINGER_HTTP_ROUTE_HANDLER_HPP

#ifndef THINGER_HTTP_ROUTE_HANDLER_HPP
#define THINGER_HTTP_ROUTE_HANDLER_HPP

#include <map>
#include <vector>
#include <memory>
#include "../request_handler.hpp"
#include "route.hpp"
#include "route_builder.hpp"

namespace thinger::http {

class route_handler : public request_handler {
public:
    route_handler();
    virtual ~route_handler() = default;
    
    // Access route builders for different HTTP methods
    route_builder operator[](method http_method);
    
    // Handle incoming requests
    bool handle_request(std::shared_ptr<request> request) override;

    // Find the matching route for a request (without executing the handler)
    const route* find_route(std::shared_ptr<request> req);

    // Handle an unmatched request (404/fallback)
    void handle_unmatched(std::shared_ptr<request> req);

    // Enable CORS support
    void enable_cors(bool enabled = true);
    
    // Add a catch-all handler for unmatched routes
    void set_fallback_handler(std::function<void(request&, response&)> handler);
    
    // Get all registered routes (useful for API documentation)
    const std::map<method, std::vector<route>>& get_routes() const { return routes_; }
    
private:
    std::map<method, std::vector<route>> routes_;
    bool cors_enabled_ = false;
    std::function<void(request&, response&)> fallback_handler_;
    
    // Helper function to send error responses
    void send_error_response(std::shared_ptr<request> req, http_response::status status);
    
    // Friend class to allow route_builder to access routes_
    friend class route_builder;
};

} // namespace thinger::http

#endif // THINGER_HTTP_ROUTE_HANDLER_HPP
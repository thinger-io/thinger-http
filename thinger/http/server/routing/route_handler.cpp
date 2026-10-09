#include "route_handler.hpp"
#include "../response.hpp"
#include "../../../util/logger.hpp"
#include <regex>

namespace thinger::http {

route_handler::route_handler() = default;

route_builder route_handler::operator[](method http_method) {
    return route_builder(http_method, routes_[http_method], &schema_components_);
}

void route_handler::enable_cors(bool enabled) {
    if (enabled) {
        // Add OPTIONS handler for all routes
        (*this)[method::OPTIONS][":path(.*)"] = [](request& req, response& res) {
            auto response = std::make_shared<http_response>();
            response->set_status(http_response::status::no_content);
            
            // Add CORS headers
            response->add_header("Access-Control-Allow-Origin", "*");
            response->add_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS, HEAD, PATCH");
            response->add_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
            response->add_header("Access-Control-Max-Age", "86400");
            
            res.send_response(response);
        };
        routes_[method::OPTIONS].back().hidden();
    }
}

void route_handler::set_fallback_handler(std::function<void(request&, response&)> handler) {
    if (!handler) {
        fallback_handler_ = nullptr;
        return;
    }
    fallback_handler_ = [handler = std::move(handler)](request& req, response& res) -> thinger::awaitable<void> {
        handler(req, res);
        co_return;
    };
}

const route* route_handler::find_route(std::shared_ptr<request> req) {
    auto http_request = req->get_http_request();
    const auto& request_method = http_request->get_method();
    const auto path = http_request->get_path();

    LOG_DEBUG("Finding route for {} {}", get_method(request_method), path);

    // Find routes for this method
    auto method_routes = routes_.find(request_method);
    if (method_routes == routes_.end()) {
        LOG_DEBUG("No routes registered for method {}", get_method(request_method));
        return nullptr;
    }

    // Try to match against registered routes
    for (auto& route : method_routes->second) {
        std::smatch matches;
        if (route.matches(path, matches)) {
            LOG_DEBUG("Matched route: {}", route.get_pattern());

            // Extract parameters from the match
            for (size_t i = 0; i < route.get_parameters().size(); ++i) {
                const auto& param = route.get_parameters()[i];
                if (i + 1 < matches.size() && matches[i + 1].matched) {
                    req->set_uri_parameter(param, matches[i + 1].str());
                }
            }

            // Set the matched route in request
            req->set_matched_route(&route);

            return &route;
        }
    }

    LOG_DEBUG("No matching route found for {}", path);
    return nullptr;
}

thinger::awaitable<void> route_handler::handle_unmatched(std::shared_ptr<request> req, response& res) {
    if (fallback_handler_) {
        co_await fallback_handler_(*req, res);
        co_return;
    }

    // Check if the method has no routes at all → 405, otherwise 404
    const auto& request_method = req->get_http_request()->get_method();
    auto status = routes_.contains(request_method) ? http_response::status::not_found
                                                   : http_response::status::not_allowed;
    res.error(status);
}

} // namespace thinger::http
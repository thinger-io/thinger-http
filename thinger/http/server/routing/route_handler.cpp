#include "route_handler.hpp"
#include "../response.hpp"
#include "../../../util/logger.hpp"
#include <regex>

namespace thinger::http {

route_handler::route_handler() = default;

route_builder route_handler::operator[](method http_method) {
    return route_builder(http_method, routes_[http_method]);
}

void route_handler::enable_cors(bool enabled) {
    cors_enabled_ = enabled;
    
    if (enabled) {
        // Add OPTIONS handler for all routes
        (*this)[method::OPTIONS][".*"] = [](request& req, response& res) {
            auto response = std::make_shared<http_response>();
            response->set_status(http_response::status::no_content);
            
            // Add CORS headers
            response->add_header("Access-Control-Allow-Origin", "*");
            response->add_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS, HEAD, PATCH");
            response->add_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
            response->add_header("Access-Control-Max-Age", "86400");
            
            res.send_response(response);
        };
    }
}

void route_handler::set_fallback_handler(std::function<void(request&, response&)> handler) {
    fallback_handler_ = std::move(handler);
}

void route_handler::send_error_response(std::shared_ptr<request> req, http_response::status status) {
    auto connection = req->get_http_connection();
    auto stream = req->get_http_stream();
    auto http_request = req->get_http_request();
    
    if (connection && stream && http_request) {
        response res(connection, stream, http_request, cors_enabled_);
        res.status(status);
        res.send("");
    }
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

            // Check authorization if required
            if (route.get_auth_level() != auth_level::PUBLIC) {
                LOG_DEBUG("Route requires authentication level: {}",
                         static_cast<int>(route.get_auth_level()));
            }

            return &route;
        }
    }

    LOG_DEBUG("No matching route found for {}", path);
    return nullptr;
}

void route_handler::handle_unmatched(std::shared_ptr<request> req) {
    auto http_request = req->get_http_request();

    if (fallback_handler_) {
        auto connection = req->get_http_connection();
        auto stream = req->get_http_stream();
        if (connection && stream) {
            response res(connection, stream, http_request, cors_enabled_);
            fallback_handler_(*req, res);
            return;
        }
    }

    // Check if the method has no routes at all → 405, otherwise 404
    const auto& request_method = http_request->get_method();
    auto method_routes = routes_.find(request_method);
    if (method_routes == routes_.end()) {
        send_error_response(req, http_response::status::not_allowed);
    } else {
        send_error_response(req, http_response::status::not_found);
    }
}

bool route_handler::handle_request(std::shared_ptr<request> request) {
    auto* matched = find_route(request);

    if (!matched) {
        handle_unmatched(request);
        return true;
    }

    // Handle the request
    try {
        auto connection = request->get_http_connection();
        auto stream = request->get_http_stream();
        auto http_request = request->get_http_request();
        if (!connection || !stream) {
            LOG_ERROR("No connection or stream available");
            return false;
        }

        response res(connection, stream, http_request, cors_enabled_);
        matched->handle_request(*request, res);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Exception handling route: {}", e.what());
        send_error_response(request, http_response::status::internal_server_error);
        return true;
    }
}

} // namespace thinger::http
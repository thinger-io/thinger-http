#include "route_handler.hpp"
#include "../response.hpp"
#include "../../../util/logger.hpp"

namespace thinger::http {

route_handler::route_handler() = default;

route_builder route_handler::operator[](method http_method) {
    return route_builder(http_method, routes_[http_method], trees_[http_method], &schema_components_);
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

const route* route_handler::find_route(std::shared_ptr<request> req) {
    auto http_request = req->get_http_request();
    const auto& request_method = http_request->get_method();
    std::string_view path = http_request->get_uri();
    path = path.substr(0, path.find('?'));

    LOG_DEBUG("Finding route for {} {}", get_method(request_method), path);

    detail::route_captures captures;
    const auto* found = match(request_method, path, captures);
    // HEAD is answered by the GET route of the path when no HEAD route matches it (the
    // response drops the body): only searched once HEAD missed, so GET pays nothing for it
    if (!found && request_method == method::HEAD) {
        captures.clear();
        found = match(method::GET, path, captures);
    }
    if (!found) {
        LOG_DEBUG("No matching route found for {} {}", get_method(request_method), path);
        return nullptr;
    }
    LOG_DEBUG("Matched route: {}", found->target->get_pattern());

    // Path parameters, as captured (raw, not percent-decoded)
    for (size_t i = 0; i < captures.size() && i < found->names.size(); ++i) {
        if (!found->names[i].empty() && captures[i].data()) {
            req->set_uri_parameter(found->names[i], std::string(captures[i]));
        }
    }

    req->set_matched_route(found->target);
    return found->target;
}

thinger::awaitable<void> route_handler::handle_unmatched(std::shared_ptr<request> req, response& res) {
    if (fallback_handler_) {
        co_await fallback_handler_(*req, res);
        co_return;
    }

    // A method with routes (HEAD has those of GET too) → 404
    auto http_request = req->get_http_request();
    const auto& request_method = http_request->get_method();
    if (routes_.contains(request_method) || (request_method == method::HEAD && routes_.contains(method::GET))) {
        res.error(http_response::status::not_found);
        co_return;
    }

    // A method without routes at all → 405, with the methods of the routes matching the path
    // (RFC 9110, 15.5.6), HEAD wherever GET: empty if none does
    std::string_view path = http_request->get_uri();
    path = path.substr(0, path.find('?'));
    std::string allowed;
    for (auto candidate : {method::GET, method::HEAD, method::POST, method::PUT, method::DELETE,
                           method::TRACE, method::OPTIONS, method::CONNECT, method::PATCH}) {
        detail::route_captures captures;
        if (!match(candidate, path, captures) && !(candidate == method::HEAD && match(method::GET, path, captures))) {
            continue;
        }
        if (!allowed.empty()) allowed += ", ";
        allowed += get_method(candidate);
    }
    res.header("Allow", allowed);
    res.error(http_response::status::not_allowed);
}

} // namespace thinger::http
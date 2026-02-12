#ifndef THINGER_HTTP_ROUTE_DESCRIPTOR_HPP
#define THINGER_HTTP_ROUTE_DESCRIPTOR_HPP

#include <functional>
#include <regex>
#include <string>
#include <vector>
#include <memory>
#include "../request.hpp"
#include "../../../util/types.hpp"

namespace thinger::http {

// Route parameters syntax:
// 1. Simple parameters: :param_name
//    Example: "/api/v1/users/:user/devices/:device"
//    Matches any non-slash characters
//
// 2. Parameters with custom regex: :param_name(regex)
//    Example: "/api/v1/users/:id([0-9]+)"         - numeric ID only
//    Example: "/api/v1/users/:user([a-zA-Z0-9_-]{1,32})" - alphanumeric with length limit
//    Example: "/files/:path(.+)"                   - match everything including slashes
//
// Common patterns:
#define ID_PATTERN      "[0-9]+"                      // Numeric ID
#define ALPHANUM_ID     "[a-zA-Z0-9_-]{1,32}"        // Alphanumeric ID (1-32 chars)
#define UUID_PATTERN    "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"
#define EMAIL_PATTERN   "[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}"
#define SLUG_PATTERN    "[a-z0-9]+(?:-[a-z0-9]+)*"   // URL-friendly slug

// Authorization levels
enum class auth_level {
    PUBLIC,     // No authentication required
    USER,       // Valid user required
    ADMIN       // Admin user required
};

// Forward declarations
class request;
class response;

// Callback types for route handlers - supporting multiple signatures
using route_callback_response_only = std::function<void(response&)>;
using route_callback_json_response = std::function<void(nlohmann::json&, response&)>;
using route_callback_request_response = std::function<void(request&, response&)>;
using route_callback_request_json_response = std::function<void(request&, nlohmann::json&, response&)>;
using route_callback_awaitable = std::function<thinger::awaitable<void>(request&, response&)>;

// Legacy callback types (for backward compatibility if needed)
using route_callback = route_callback_request_response;
using route_callback_json = route_callback_request_json_response;

class route {
public:
    route(const std::string& pattern);
    
    // Set the callback handler - multiple signatures supported
    route& operator=(route_callback_response_only callback);
    route& operator=(route_callback_json_response callback);
    route& operator=(route_callback_request_response callback);
    route& operator=(route_callback_request_json_response callback);
    route& operator=(route_callback_awaitable callback);
    
    // Deferred body mode - handler reads body at its discretion
    route& deferred_body(bool enabled = true);
    bool is_deferred_body() const { return deferred_body_; }

    // Set authorization level
    route& auth(auth_level level);
    
    // Set description for API documentation
    route& description(const std::string& desc);
    
    // Check if route matches the given path
    bool matches(const std::string& path, std::smatch& matches) const;
    
    // Get route parameters from regex
    const std::vector<std::string>& get_parameters() const { return parameters_; }
    
    // Get authorization level
    auth_level get_auth_level() const { return auth_level_; }
    
    // Handle the request (synchronous)
    void handle_request(request& req, response& res) const;

    // Handle the request (coroutine — works for all callback types)
    thinger::awaitable<void> handle_request_coro(request& req, response& res) const;
    
    // Get the original pattern
    const std::string& get_pattern() const { return pattern_; }
    
private:
    std::string pattern_;
    std::regex regex_;
    std::vector<std::string> parameters_;
    auth_level auth_level_ = auth_level::PUBLIC;
    std::string description_;
    bool deferred_body_ = false;
    std::variant<
        route_callback_response_only,
        route_callback_json_response,
        route_callback_request_response,
        route_callback_request_json_response,
        route_callback_awaitable
    > callback_;
    
    void parse_parameters();
};

} // namespace thinger::http

#endif // THINGER_HTTP_ROUTE_DESCRIPTOR_HPP
#include "route.hpp"
#include "../response.hpp"
#include <regex>

namespace thinger::http {

route::route(const std::string& pattern)
    : pattern_(pattern)
{
    // Convert route pattern to regex
    // Support two syntaxes:
    // 1. :param_name - matches any non-slash characters
    // 2. :param_name(regex) - matches the specified regex pattern
    
    std::string regex_pattern = pattern;
    
    // First, handle parameters with custom regex: :param(regex)
    std::regex custom_param_regex(":([a-zA-Z_][a-zA-Z0-9_]*)\\(([^)]+)\\)");
    std::smatch match;
    std::string temp = pattern;
    
    // Find all :param(regex) patterns
    while (std::regex_search(temp, match, custom_param_regex)) {
        parameters_.push_back(match[1]);
        temp = match.suffix();
    }
    
    // Then, handle simple parameters: :param
    std::regex simple_param_regex(":([a-zA-Z_][a-zA-Z0-9_]*)(?![\\(])");
    temp = pattern;
    while (std::regex_search(temp, match, simple_param_regex)) {
        // Only add if not already added (avoid duplicates with custom regex params)
        std::string param_name = match[1];
        if (std::find(parameters_.begin(), parameters_.end(), param_name) == parameters_.end()) {
            parameters_.push_back(param_name);
        }
        temp = match.suffix();
    }
    
    // Escape special regex characters in the pattern (but not in our parameter patterns)
    std::string escaped = pattern;
    
    // First, temporarily replace our parameter patterns to protect them
    escaped = std::regex_replace(escaped, custom_param_regex, "__CUSTOM_PARAM_$1__");
    escaped = std::regex_replace(escaped, simple_param_regex, "__SIMPLE_PARAM_$1__");
    
    // Escape special characters
    escaped = std::regex_replace(escaped, std::regex("([.^$*+?{}\\[\\]\\\\|])"), "\\\\$1");
    
    // Now replace parameters with their regex groups
    // Custom parameters: restore the custom regex
    temp = pattern;
    std::string result = escaped;
    while (std::regex_search(temp, match, custom_param_regex)) {
        std::string param_name = match[1];
        std::string param_regex = match[2];
        std::string placeholder = "__CUSTOM_PARAM_" + param_name + "__";
        result = std::regex_replace(result, std::regex(placeholder), "(" + param_regex + ")");
        temp = match.suffix();
    }
    
    // Simple parameters: use default regex
    result = std::regex_replace(result, std::regex("__SIMPLE_PARAM_([a-zA-Z_][a-zA-Z0-9_]*)__"), "([^/]+)");
    
    // Add anchors
    regex_pattern = "^" + result + "$";
    
    regex_ = std::regex(regex_pattern);
}

route& route::operator=(route_callback_response_only callback) {
    callback_ = std::move(callback);
    return *this;
}

route& route::operator=(route_callback_json_response callback) {
    callback_ = std::move(callback);
    return *this;
}

route& route::operator=(route_callback_request_response callback) {
    callback_ = std::move(callback);
    return *this;
}

route& route::operator=(route_callback_request_json_response callback) {
    callback_ = std::move(callback);
    return *this;
}

route& route::operator=(route_callback_awaitable callback) {
    callback_ = std::move(callback);
    deferred_body_ = true;  // auto-enable deferred body for awaitable callbacks
    return *this;
}

route& route::deferred_body(bool enabled) {
    deferred_body_ = enabled;
    return *this;
}

route& route::auth(auth_level level) {
    auth_level_ = level;
    return *this;
}

route& route::description(const std::string& desc) {
    description_ = desc;
    return *this;
}

bool route::matches(const std::string& path, std::smatch& matches) const {
    return std::regex_match(path, matches, regex_);
}

void route::handle_request(request& req, response& res) const {
    // Handle response-only callback
    if (std::holds_alternative<route_callback_response_only>(callback_)) {
        std::get<route_callback_response_only>(callback_)(res);
    }
    // Handle JSON + response callback (json is parsed from request body)
    else if (std::holds_alternative<route_callback_json_response>(callback_)) {
        auto http_req = req.get_http_request();
        if (!http_req->get_body().empty()) {
            auto json = nlohmann::json::parse(http_req->get_body(), nullptr, false);
            if (json.is_discarded()) {
                res.error(http_response::status::bad_request, "Invalid JSON");
            } else {
                std::get<route_callback_json_response>(callback_)(json, res);
            }
        } else {
            nlohmann::json empty_json;
            std::get<route_callback_json_response>(callback_)(empty_json, res);
        }
    }
    // Handle request + response callback
    else if (std::holds_alternative<route_callback_request_response>(callback_)) {
        std::get<route_callback_request_response>(callback_)(req, res);
    }
    // Handle request + JSON + response callback (json is parsed from request body)
    else if (std::holds_alternative<route_callback_request_json_response>(callback_)) {
        auto http_req = req.get_http_request();
        if (!http_req->get_body().empty()) {
            auto json = nlohmann::json::parse(http_req->get_body(), nullptr, false);
            if (json.is_discarded()) {
                res.error(http_response::status::bad_request, "Invalid JSON");
            } else {
                std::get<route_callback_request_json_response>(callback_)(req, json, res);
            }
        } else {
            nlohmann::json empty_json;
            std::get<route_callback_request_json_response>(callback_)(req, empty_json, res);
        }
    }
    // Awaitable callback — cannot be called synchronously
    else if (std::holds_alternative<route_callback_awaitable>(callback_)) {
        res.error(http_response::status::internal_server_error,
                  "Awaitable route handler invoked synchronously; use handle_request_coro() instead");
    }
}

thinger::awaitable<void> route::handle_request_coro(request& req, response& res) const {
    if (std::holds_alternative<route_callback_awaitable>(callback_)) {
        co_await std::get<route_callback_awaitable>(callback_)(req, res);
    } else {
        handle_request(req, res);
    }
}

void route::parse_parameters() {
    // Parameters are now parsed in the constructor
}

} // namespace thinger::http
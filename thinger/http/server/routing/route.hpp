#ifndef THINGER_HTTP_ROUTE_DESCRIPTOR_HPP
#define THINGER_HTTP_ROUTE_DESCRIPTOR_HPP

#include <concepts>
#include <variant>
#include <map>
#include <functional>
#include <regex>
#include <string>
#include <vector>
#include <memory>
#include "../request.hpp"
#include "../../../util/types.hpp"

#ifdef THINGER_HTTP_VALIJSON_ENABLED
#include <valijson/schema.hpp>
#include <valijson/schema_parser.hpp>
#include <valijson/validator.hpp>
#include <valijson/validation_results.hpp>
#include <valijson/adapters/nlohmann_json_adapter.hpp>
#endif

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

// Forward declarations
class request;
class response;

// Callback types for route handlers - supporting multiple signatures
using route_callback_response_only = std::function<void(response&)>;
using route_callback_json_response = std::function<void(nlohmann::json&, response&)>;
using route_callback_request_response = std::function<void(request&, response&)>;
using route_callback_request_json_response = std::function<void(request&, nlohmann::json&, response&)>;
using route_callback_awaitable = std::function<thinger::awaitable<void>(request&, response&)>;
using route_callback_awaitable_json = std::function<thinger::awaitable<void>(nlohmann::json&, response&)>;
using route_callback_awaitable_request_json = std::function<thinger::awaitable<void>(request&, nlohmann::json&, response&)>;

// Coroutine handler signatures. Those taking a JSON body get it read, parsed and validated
// against the route schema before they run; (request&, response&) reads the body itself
// (deferred body) unless deferred_body(false) is set.
template<typename F>
concept awaitable_handler = requires(F f, request& req, response& res) {
    { f(req, res) } -> std::same_as<thinger::awaitable<void>>;
};

template<typename F>
concept awaitable_request_json_handler = requires(F f, request& req, nlohmann::json& json, response& res) {
    { f(req, json, res) } -> std::same_as<thinger::awaitable<void>>;
};

// Generic lambdas taking two arguments are treated as (request&, response&)
template<typename F>
concept awaitable_json_handler = !awaitable_handler<F> && requires(F f, nlohmann::json& json, response& res) {
    { f(json, res) } -> std::same_as<thinger::awaitable<void>>;
};

template<typename F>
concept coroutine_handler = awaitable_handler<F> || awaitable_json_handler<F> || awaitable_request_json_handler<F>;

// Legacy callback types (for backward compatibility if needed)
using route_callback = route_callback_request_response;
using route_callback_json = route_callback_request_json_response;

// Documented request parameter (path or query), for API documentation
struct route_parameter {
    std::string name;
    std::string in;             // "path" or "query"
    std::string description;
    nlohmann::json schema;      // JSON Schema of the value
    bool required = false;
};

// Documented response for a status code, for API documentation
struct route_response {
    std::string description;
    nlohmann::json schema;      // JSON Schema of the body (null if none)
    nlohmann::json example;     // example body (null if none)
};

class route {
public:
    route(const std::string& pattern);
    
    // Set the callback handler - multiple signatures supported
    route& operator=(route_callback_response_only callback);
    route& operator=(route_callback_json_response callback);
    route& operator=(route_callback_request_response callback);
    route& operator=(route_callback_request_json_response callback);
    route& operator=(route_callback_awaitable callback);
    route& operator=(route_callback_awaitable_json callback);
    route& operator=(route_callback_awaitable_request_json callback);

    // Coroutine lambdas: picked before the std::function<void(...)> overloads above,
    // which would otherwise also accept them (discarding the awaitable)
    template<coroutine_handler F>
    route& operator=(F&& callback) {
        if constexpr (awaitable_handler<F>) {
            return *this = route_callback_awaitable(std::forward<F>(callback));
        } else if constexpr (awaitable_request_json_handler<F>) {
            return *this = route_callback_awaitable_request_json(std::forward<F>(callback));
        } else {
            return *this = route_callback_awaitable_json(std::forward<F>(callback));
        }
    }

    // Deferred body mode - handler reads body at its discretion. Ignored by handlers
    // that take the JSON body, which always need it read first.
    route& deferred_body(bool enabled = true);
    bool is_deferred_body() const { return deferred_body_ && !takes_json_body(); }

    // Set JSON Schema for request body validation
    route& schema(const nlohmann::json& json_schema);

    // --- API documentation (read by tools such as OpenAPI generators) ---
    route& summary(const std::string& text);
    route& description(const std::string& desc);
    route& tag(const std::string& name);
    route& tags(const std::vector<std::string>& names);
    route& operation_id(const std::string& id);
    route& deprecated(bool value = true);
    route& path_param(const std::string& name, const std::string& description,
                      nlohmann::json schema = {{"type", "string"}});
    route& query_param(const std::string& name, const std::string& description,
                       nlohmann::json schema = {{"type", "string"}}, bool required = false);
    route& returns(int status, const std::string& description,
                   nlohmann::json schema = nullptr, nlohmann::json example = nullptr);
    route& example(nlohmann::json body);
    route& hidden(bool value = true);   // leave the route out of the API documentation

    const std::string& get_summary() const { return summary_; }
    const std::string& get_description() const { return description_; }
    const std::vector<std::string>& get_tags() const { return tags_; }
    const std::string& get_operation_id() const { return operation_id_; }
    bool is_deprecated() const { return deprecated_; }
    const std::vector<route_parameter>& get_param_docs() const { return param_docs_; }
    const std::map<int, route_response>& get_responses() const { return responses_; }
    const std::vector<nlohmann::json>& get_examples() const { return examples_; }
    const nlohmann::json& get_schema() const { return json_schema_; }
    bool is_hidden() const { return hidden_; }

    // Whether the callback receives the request body parsed as JSON
    bool takes_json_body() const;

    // Shared schemas (OpenAPI components) that route schemas can reference with
    // {"$ref": "#/components/schemas/<name>"}; set by the router that owns the route
    void set_schema_components(const nlohmann::json* components) { schema_components_ = components; }

    // Custom regex of a path parameter declared as :name(regex), empty if it has none
    std::string get_parameter_pattern(const std::string& name) const;

    // --- Free metadata for the application (e.g. the permission a route requires). ---
    // The library does not interpret it; middlewares can read it via request::get_matched_route().
    route& meta(const std::string& key, nlohmann::json value);
    bool has_meta(const std::string& key) const;
    const nlohmann::json& get_meta(const std::string& key) const;  // null if missing
    const nlohmann::json& get_metadata() const { return metadata_; }
    
    // Check if route matches the given path
    bool matches(const std::string& path, std::smatch& matches) const;
    
    // Get route parameters from regex
    const std::vector<std::string>& get_parameters() const { return parameters_; }
    
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
    std::map<std::string, std::string> parameter_patterns_;
    nlohmann::json json_schema_;

    // Documentation and metadata
    std::string summary_;
    std::string description_;
    std::vector<std::string> tags_;
    std::string operation_id_;
    bool deprecated_ = false;
    std::vector<route_parameter> param_docs_;
    std::map<int, route_response> responses_;
    std::vector<nlohmann::json> examples_;
    bool hidden_ = false;
    nlohmann::json metadata_ = nlohmann::json::object();
    const nlohmann::json* schema_components_ = nullptr;
    bool deferred_body_ = false;
    std::variant<
        route_callback_response_only,
        route_callback_json_response,
        route_callback_request_response,
        route_callback_request_json_response,
        route_callback_awaitable,
        route_callback_awaitable_json,
        route_callback_awaitable_request_json
    > callback_;

    // Parse the request body as JSON and validate it against the schema; responds with
    // an error and returns false if it is not valid
    bool parse_json_body(request& req, response& res, nlohmann::json& json) const;

#ifdef THINGER_HTTP_VALIJSON_ENABLED
    std::shared_ptr<valijson::Schema> schema_;

    bool validate_json(const nlohmann::json& json, response& res) const;
#endif

    void parse_parameters();
};

} // namespace thinger::http

#endif // THINGER_HTTP_ROUTE_DESCRIPTOR_HPP
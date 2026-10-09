#ifndef THINGER_HTTP_ROUTE_DESCRIPTOR_HPP
#define THINGER_HTTP_ROUTE_DESCRIPTOR_HPP

#include <concepts>
#include <type_traits>
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
//    Matches any non-empty text without slashes
//
// 2. Parameters with custom regex: :param_name(regex)
//    Example: "/api/v1/users/:id([0-9]+)"         - numeric ID only
//    Example: "/api/v1/users/:user([a-zA-Z0-9_-]{1,32})" - alphanumeric with length limit
//    Example: "/files/:path(.+)"                   - match everything including slashes
//    The regex extends to the matching closing parenthesis, so it may contain groups.
//
// Matching (see route_tree): the path is matched segment by segment (the text between
// slashes), with this priority whatever the registration order: static segments, then
// segments with a regex constraint (or mixing text and parameters, as ":name.:ext"), then
// simple parameters, then parameters that may match slashes (as ":path(.+)"), which take
// the rest of the path. If the rest of the path does not match below the chosen option,
// the next one is tried. Different constraints at the same position are tried in the
// order they were first registered there, and wildcards in registration order; registering
// a route with the same structure as another one, or one that may match the same paths as
// another one where only that order decides, logs a warning. A parameter regex that
// depends on what surrounds it (anchors ^ $, word boundaries, lookaheads, back-references)
// makes its route match the whole path with its regex, after all the other options.
// - The path is matched as received, before percent-decoding: "%2F" is not a slash, and
//   parameters get the encoded text ("/files/:name" on "/files/a%20b" gives "a%20b").
// - Trailing slashes and empty segments are significant: "/users" does not match
//   "/users/", and a parameter does not match an empty segment ("/users//devices").
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
// What every route callback is turned into: a coroutine taking (request&, response&)
using route_callback_awaitable = std::function<thinger::awaitable<void>(request&, response&)>;

// Route callback signatures, synchronous or coroutines returning awaitable<void>. Those
// taking a JSON body get it read, parsed and validated against the route schema before
// they run; coroutines taking (request&, response&) or (response&) read the body
// themselves (deferred body) unless deferred_body(false) is set on the route.
template<typename F>
concept request_response_callback = std::invocable<F&, request&, response&>;

template<typename F>
concept response_callback = !request_response_callback<F> && std::invocable<F&, response&>;

template<typename F>
concept request_json_callback = std::invocable<F&, request&, nlohmann::json&, response&>;

// Generic lambdas taking two arguments are treated as (request&, response&)
template<typename F>
concept json_callback = !request_response_callback<F> && std::invocable<F&, nlohmann::json&, response&>;

// Callbacks that do not take the JSON body (they may read the body themselves), as the
// not found handlers
template<typename F>
concept request_handler_callable = request_response_callback<F> || response_callback<F>;

template<typename F>
concept route_callable = request_handler_callable<F> || request_json_callback<F> || json_callback<F>;

// The same signatures, as coroutines
template<typename F, typename... Args>
concept returns_awaitable = std::same_as<std::invoke_result_t<F&, Args...>, thinger::awaitable<void>>;

template<typename F>
concept awaitable_handler = request_response_callback<F> && returns_awaitable<F, request&, response&>;

template<typename F>
concept awaitable_response_handler = response_callback<F> && returns_awaitable<F, response&>;

template<typename F>
concept awaitable_request_json_handler = request_json_callback<F>
                                      && returns_awaitable<F, request&, nlohmann::json&, response&>;

template<typename F>
concept awaitable_json_handler = json_callback<F> && returns_awaitable<F, nlohmann::json&, response&>;

namespace detail {

    template<typename T>
    inline constexpr bool is_awaitable_v = false;

    template<typename T, typename Executor>
    inline constexpr bool is_awaitable_v<boost::asio::awaitable<T, Executor>> = true;

    template<typename T>
    inline constexpr bool is_std_function_v = false;

    template<typename R, typename... Args>
    inline constexpr bool is_std_function_v<std::function<R(Args...)>> = true;

    // Whether a callback is empty: a null std::function or function pointer
    template<typename F>
    bool is_empty_callback(const F& callback) {
        using type = std::remove_cvref_t<F>;
        if constexpr (is_std_function_v<type> || std::is_pointer_v<type>) {
            return !callback;
        } else {
            return false;
        }
    }

    template<typename F, typename... Args>
    thinger::awaitable<void> call_synchronously(F& callback, Args&... args) {
        callback(args...);
        co_return;
    }

    // Call a route callback as a coroutine: coroutines as they are, synchronous callbacks
    // from one (their result, if any, is ignored)
    template<typename F, typename... Args>
    thinger::awaitable<void> call_callback(F& callback, Args&... args) {
        using result = std::invoke_result_t<F&, Args&...>;
        if constexpr (std::same_as<result, thinger::awaitable<void>>) {
            return callback(args...);
        } else {
            static_assert(!is_awaitable_v<result>, "coroutine route callbacks must return thinger::awaitable<void>");
            return call_synchronously(callback, args...);
        }
    }

}

// A callback taking (request&, response&) or (response&), as the coroutine every route
// callback is turned into; empty if the callback is (a null std::function or pointer)
template<request_handler_callable F>
route_callback_awaitable make_route_callback(F&& callback) {
    if (detail::is_empty_callback(callback)) return nullptr;
    if constexpr (request_response_callback<F>) {
        return [callback = std::forward<F>(callback)](request& req, response& res) mutable {
            return detail::call_callback(callback, req, res);
        };
    } else {
        return [callback = std::forward<F>(callback)](request&, response& res) mutable {
            return detail::call_callback(callback, res);
        };
    }
}

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
    
    // Set the callback: any route_callable signature, synchronous or coroutine. It is
    // stored as a callable taking (request&, response&), synchronous (called directly,
    // without a coroutine frame per request) or coroutine, and whether it takes the JSON body.
    template<route_callable F>
    route& operator=(F&& callback) {
        takes_json_body_ = !request_handler_callable<F>;
        auto adapted = [callback = std::forward<F>(callback)](request& req, response& res) mutable -> decltype(auto) {
            return invoke_callback(callback, req, res);
        };
        using result = std::invoke_result_t<decltype(adapted)&, request&, response&>;
        if constexpr (std::same_as<result, thinger::awaitable<void>>) {
            callback_ = std::move(adapted);
            sync_callback_ = nullptr;
            // coroutines not taking the JSON body read it themselves; the others get it read first
            if (!takes_json_body_) deferred_body_ = true;
        } else {
            static_assert(!detail::is_awaitable_v<result>, "coroutine route callbacks must return thinger::awaitable<void>");
            sync_callback_ = std::move(adapted);
            callback_ = nullptr;
        }
        return *this;
    }

    // Deferred body mode - handler reads body at its discretion. Ignored by handlers
    // that take the JSON body, which always need it read first.
    route& deferred_body(bool enabled = true);
    bool is_deferred_body() const { return deferred_body_ && !takes_json_body(); }

    // Set JSON Schema for request body validation. If it cannot be parsed (e.g. a $ref to
    // a schema component not registered yet), requests to the route are answered with 500.
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
    bool takes_json_body() const { return takes_json_body_; }

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
    
    // Handle the request: parse and validate the JSON body for callbacks taking it, then
    // run the callback
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
    bool takes_json_body_ = false;
    // The callback, either synchronous or coroutine (the other one is empty)
    route_callback_request_response sync_callback_;
    route_callback_awaitable callback_;

    // Call a callback with the arguments it takes; those taking the JSON body get the body
    // parsed by handle_request_coro()
    template<typename F>
    static decltype(auto) invoke_callback(F& callback, request& req, response& res) {
        if constexpr (request_response_callback<F>) {
            return callback(req, res);
        } else if constexpr (response_callback<F>) {
            return callback(res);
        } else if constexpr (request_json_callback<F>) {
            return callback(req, req.json_body_, res);
        } else {
            return callback(req.json_body_, res);
        }
    }

    // Parse the request body as JSON and validate it against the schema; responds with
    // an error and returns false if it is not valid
    bool parse_json_body(request& req, response& res, nlohmann::json& json) const;

#ifdef THINGER_HTTP_VALIJSON_ENABLED
    std::shared_ptr<valijson::Schema> schema_;
    std::string schema_error_;  // why the schema could not be parsed (requests are rejected)

    bool validate_json(const nlohmann::json& json, response& res) const;
#endif
};

} // namespace thinger::http

#endif // THINGER_HTTP_ROUTE_DESCRIPTOR_HPP
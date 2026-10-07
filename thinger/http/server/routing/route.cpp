#include "route.hpp"
#include "../response.hpp"
#include <regex>
#include <algorithm>
#include "../../../util/logger.hpp"

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
        parameter_patterns_[match[1]] = match[2];
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

route& route::operator=(route_callback_awaitable_json callback) {
    callback_ = std::move(callback);
    return *this;
}

route& route::operator=(route_callback_awaitable_request_json callback) {
    callback_ = std::move(callback);
    return *this;
}

route& route::deferred_body(bool enabled) {
    deferred_body_ = enabled;
    return *this;
}

route& route::summary(const std::string& text) {
    summary_ = text;
    return *this;
}

route& route::description(const std::string& desc) {
    description_ = desc;
    return *this;
}

route& route::tag(const std::string& name) {
    if (std::find(tags_.begin(), tags_.end(), name) == tags_.end()) {
        tags_.push_back(name);
    }
    return *this;
}

route& route::tags(const std::vector<std::string>& names) {
    for (const auto& name : names) tag(name);
    return *this;
}

route& route::operation_id(const std::string& id) {
    operation_id_ = id;
    return *this;
}

route& route::deprecated(bool value) {
    deprecated_ = value;
    return *this;
}

route& route::path_param(const std::string& name, const std::string& description, nlohmann::json schema) {
    if (std::find(parameters_.begin(), parameters_.end(), name) == parameters_.end()) {
        LOG_WARNING("Route {} documents unknown path parameter '{}'", pattern_, name);
    }
    param_docs_.push_back({name, "path", description, std::move(schema), true});
    return *this;
}

route& route::query_param(const std::string& name, const std::string& description,
                          nlohmann::json schema, bool required) {
    param_docs_.push_back({name, "query", description, std::move(schema), required});
    return *this;
}

route& route::returns(int status, const std::string& description, nlohmann::json schema, nlohmann::json example) {
    responses_[status] = {description, std::move(schema), std::move(example)};
    return *this;
}

route& route::example(nlohmann::json body) {
    examples_.push_back(std::move(body));
    return *this;
}

std::string route::get_parameter_pattern(const std::string& name) const {
    auto it = parameter_patterns_.find(name);
    return it != parameter_patterns_.end() ? it->second : std::string{};
}

route& route::meta(const std::string& key, nlohmann::json value) {
    metadata_[key] = std::move(value);
    return *this;
}

bool route::has_meta(const std::string& key) const {
    return metadata_.contains(key);
}

const nlohmann::json& route::get_meta(const std::string& key) const {
    static const nlohmann::json null_value;
    auto it = metadata_.find(key);
    return it != metadata_.end() ? *it : null_value;
}

bool route::matches(const std::string& path, std::smatch& matches) const {
    return std::regex_match(path, matches, regex_);
}

bool route::takes_json_body() const {
    return std::holds_alternative<route_callback_json_response>(callback_)
        || std::holds_alternative<route_callback_request_json_response>(callback_)
        || std::holds_alternative<route_callback_awaitable_json>(callback_)
        || std::holds_alternative<route_callback_awaitable_request_json>(callback_);
}

bool route::parse_json_body(request& req, response& res, nlohmann::json& json) const {
    const auto& body = req.get_http_request()->get_body();
    if (!body.empty()) {
        json = nlohmann::json::parse(body, nullptr, false);
        if (json.is_discarded()) {
            res.error(http_response::status::bad_request, "Invalid JSON");
            return false;
        }
    }
#ifdef THINGER_HTTP_VALIJSON_ENABLED
    if (!validate_json(json, res)) return false;
#endif
    return true;
}

void route::handle_request(request& req, response& res) const {
    std::visit([&](const auto& callback) {
        using callback_type = std::decay_t<decltype(callback)>;
        if constexpr (std::is_same_v<callback_type, route_callback_response_only>) {
            callback(res);
        } else if constexpr (std::is_same_v<callback_type, route_callback_request_response>) {
            callback(req, res);
        } else if constexpr (std::is_same_v<callback_type, route_callback_json_response>) {
            nlohmann::json json;
            if (parse_json_body(req, res, json)) callback(json, res);
        } else if constexpr (std::is_same_v<callback_type, route_callback_request_json_response>) {
            nlohmann::json json;
            if (parse_json_body(req, res, json)) callback(req, json, res);
        } else {
            // Coroutine callbacks cannot be called synchronously
            res.error(http_response::status::internal_server_error,
                      "Awaitable route handler invoked synchronously; use handle_request_coro() instead");
        }
    }, callback_);
}

thinger::awaitable<void> route::handle_request_coro(request& req, response& res) const {
    if (std::holds_alternative<route_callback_awaitable>(callback_)) {
        co_await std::get<route_callback_awaitable>(callback_)(req, res);
    } else if (std::holds_alternative<route_callback_awaitable_json>(callback_)) {
        nlohmann::json json;
        if (parse_json_body(req, res, json)) {
            co_await std::get<route_callback_awaitable_json>(callback_)(json, res);
        }
    } else if (std::holds_alternative<route_callback_awaitable_request_json>(callback_)) {
        nlohmann::json json;
        if (parse_json_body(req, res, json)) {
            co_await std::get<route_callback_awaitable_request_json>(callback_)(req, json, res);
        }
    } else {
        handle_request(req, res);
    }
}

void route::parse_parameters() {
    // Parameters are now parsed in the constructor
}

route& route::schema(const nlohmann::json& json_schema) {
    json_schema_ = json_schema;
#ifdef THINGER_HTTP_VALIJSON_ENABLED
    schema_ = std::make_shared<valijson::Schema>();
    valijson::SchemaParser parser;
    valijson::adapters::NlohmannJsonAdapter adapter(json_schema_);
    try {
        parser.populateSchema(adapter, *schema_);
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse JSON Schema: {}", e.what());
        schema_.reset();
    }
#else
    LOG_WARNING("JSON Schema validation requested but Valijson is not enabled");
#endif
    return *this;
}

#ifdef THINGER_HTTP_VALIJSON_ENABLED
bool route::validate_json(const nlohmann::json& json, response& res) const {
    if (!schema_) return true;

    valijson::Validator validator;
    valijson::adapters::NlohmannJsonAdapter adapter(json);
    valijson::ValidationResults results;

    if (validator.validate(*schema_, adapter, &results)) {
        return true;
    }

    // Build error response with first validation error
    valijson::ValidationResults::Error error;
    nlohmann::json error_response = {{"error", {{"message", "Schema validation failed"}}}};
    if (results.popError(error)) {
        error_response["error"]["message"] = error.description;
        nlohmann::json context = nlohmann::json::array();
        for (const auto& c : error.context) {
            context.push_back(c);
        }
        if (!context.empty()) {
            error_response["error"]["context"] = std::move(context);
        }
    }

    res.json(error_response, http_response::status::bad_request);
    return false;
}
#endif

} // namespace thinger::http
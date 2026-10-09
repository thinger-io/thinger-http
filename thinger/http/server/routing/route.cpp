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
    
    // Collect parameters in order of appearance, as they map to the regex capture groups:
    // :name or :name(regex)
    std::regex custom_param_regex(":([a-zA-Z_][a-zA-Z0-9_]*)\\(([^)]+)\\)");
    std::regex simple_param_regex(":([a-zA-Z_][a-zA-Z0-9_]*)(?![\\(])");
    std::regex any_param_regex(":([a-zA-Z_][a-zA-Z0-9_]*)(?:\\(([^)]+)\\))?");
    std::smatch match;
    std::string temp;

    for (std::sregex_iterator it(pattern.begin(), pattern.end(), any_param_regex), end; it != end; ++it) {
        std::string param_name = (*it)[1];
        if ((*it)[2].matched) {
            parameter_patterns_[param_name] = (*it)[2];
        }
        if (std::find(parameters_.begin(), parameters_.end(), param_name) == parameters_.end()) {
            parameters_.push_back(param_name);
        }
    }

    // Escape special regex characters in the pattern (but not in our parameter patterns)
    std::string escaped = pattern;
    
    // First, temporarily replace our parameter patterns to protect them
    escaped = std::regex_replace(escaped, custom_param_regex, "__CUSTOM_PARAM_$1__");
    escaped = std::regex_replace(escaped, simple_param_regex, "__SIMPLE_PARAM_$1__");
    
    // Escape special characters
    // (the replacement format only treats $ specially: a single backslash is literal)
    escaped = std::regex_replace(escaped, std::regex("([.^$*+?{}\\[\\]\\\\|])"), "\\$1");
    
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

route& route::hidden(bool value) {
    hidden_ = value;
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

thinger::awaitable<void> route::handle_request_coro(request& req, response& res) const {
    if (takes_json_body_) {
        req.json_body_ = nullptr;
        if (!parse_json_body(req, res, req.json_body_)) co_return;
    }
    if (sync_callback_) {
        sync_callback_(req, res);
    } else {
        co_await callback_(req, res);
    }
}

route& route::schema(const nlohmann::json& json_schema) {
    json_schema_ = json_schema;
#ifdef THINGER_HTTP_VALIJSON_ENABLED
    // Make shared components resolvable as #/components/schemas/<name> within the schema
    nlohmann::json root = json_schema_;
    if (schema_components_ && !schema_components_->empty() && root.is_object()) {
        root["components"]["schemas"] = *schema_components_;
    }
    schema_ = std::make_shared<valijson::Schema>();
    schema_error_.clear();
    valijson::SchemaParser parser;
    valijson::adapters::NlohmannJsonAdapter adapter(root);
    try {
        parser.populateSchema(adapter, *schema_);
    } catch (const std::exception& e) {
        // Fail closed: requests to the route are rejected rather than accepted unvalidated
        schema_error_ = e.what();
        schema_.reset();
        LOG_ERROR("Invalid JSON Schema for route {}: {}. Its requests will be rejected with 500 "
                  "(are the schema components it references registered before the route?)",
                  pattern_, schema_error_);
    }
#else
    LOG_WARNING("JSON Schema validation requested but Valijson is not enabled");
#endif
    return *this;
}

#ifdef THINGER_HTTP_VALIJSON_ENABLED
bool route::validate_json(const nlohmann::json& json, response& res) const {
    if (!schema_error_.empty()) {
        LOG_ERROR("Rejecting request to route {}: its JSON Schema is invalid ({})", pattern_, schema_error_);
        res.error(http_response::status::internal_server_error, "Invalid route schema");
        return false;
    }
    if (!schema_) return true;

    valijson::Validator validator;
    valijson::adapters::NlohmannJsonAdapter adapter(json);
    valijson::ValidationResults results;

    if (validator.validate(*schema_, adapter, &results)) {
        return true;
    }

    // Answer the first validation error, with the location of the invalid value as details
    // (by default: {"error": {"message": ..., "context": [...]}})
    valijson::ValidationResults::Error error;
    std::string message = "Schema validation failed";
    nlohmann::json details = nlohmann::json::object();
    if (results.popError(error)) {
        message = error.description;
        nlohmann::json context = nlohmann::json::array();
        for (const auto& c : error.context) {
            context.push_back(c);
        }
        if (!context.empty()) {
            details["context"] = std::move(context);
        }
    }

    res.error(http_response::status::bad_request, message, details);
    return false;
}
#endif

} // namespace thinger::http
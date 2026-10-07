#include "openapi.hpp"
#include "../../util/logger.hpp"
#include <algorithm>
#include <cctype>
#include <regex>
#include <set>

namespace thinger::http {

openapi_generator& openapi_generator::server(const std::string& url, const std::string& description) {
    nlohmann::json entry = {{"url", url}};
    if (!description.empty()) entry["description"] = description;
    servers_.push_back(std::move(entry));
    return *this;
}

std::string openapi_generator::to_openapi_path(const std::string& pattern) {
    // Same parameter syntax as route: :name(regex) and :name
    static const std::regex custom_param(":([a-zA-Z_][a-zA-Z0-9_]*)\\(([^)]+)\\)");
    static const std::regex simple_param(":([a-zA-Z_][a-zA-Z0-9_]*)");
    auto path = std::regex_replace(pattern, custom_param, "{$1}");
    return std::regex_replace(path, simple_param, "{$1}");
}

nlohmann::json openapi_generator::build_operation(const route& r) const {
    nlohmann::json operation = nlohmann::json::object();

    if (!r.get_summary().empty()) operation["summary"] = r.get_summary();
    if (!r.get_description().empty()) operation["description"] = r.get_description();
    if (!r.get_tags().empty()) operation["tags"] = r.get_tags();
    if (!r.get_operation_id().empty()) operation["operationId"] = r.get_operation_id();
    if (r.is_deprecated()) operation["deprecated"] = true;

    // Path parameters: every parameter in the pattern, completed with its documentation
    nlohmann::json parameters = nlohmann::json::array();
    for (const auto& name : r.get_parameters()) {
        nlohmann::json parameter = {{"name", name}, {"in", "path"}, {"required", true}};
        nlohmann::json schema = {{"type", "string"}};

        auto doc = std::find_if(r.get_param_docs().begin(), r.get_param_docs().end(),
            [&](const route_parameter& p) { return p.in == "path" && p.name == name; });
        if (doc != r.get_param_docs().end()) {
            if (!doc->description.empty()) parameter["description"] = doc->description;
            if (!doc->schema.is_null()) schema = doc->schema;
        }

        auto pattern = r.get_parameter_pattern(name);
        if (!pattern.empty() && schema.is_object() && !schema.contains("pattern")) {
            schema["pattern"] = "^(?:" + pattern + ")$";
        }
        parameter["schema"] = std::move(schema);
        parameters.push_back(std::move(parameter));
    }

    // Query parameters, only as documented
    for (const auto& doc : r.get_param_docs()) {
        if (doc.in != "query") continue;
        nlohmann::json parameter = {{"name", doc.name}, {"in", "query"}, {"required", doc.required}};
        if (!doc.description.empty()) parameter["description"] = doc.description;
        parameter["schema"] = doc.schema.is_null() ? nlohmann::json{{"type", "string"}} : doc.schema;
        parameters.push_back(std::move(parameter));
    }
    if (!parameters.empty()) operation["parameters"] = std::move(parameters);

    // Request body: the route schema and examples
    const auto& schema = r.get_schema();
    const auto& examples = r.get_examples();
    if (!schema.is_null() || !examples.empty() || r.takes_json_body()) {
        nlohmann::json media = {{"schema", schema.is_null() ? nlohmann::json::object() : schema}};
        if (examples.size() == 1) {
            media["example"] = examples.front();
        } else if (examples.size() > 1) {
            for (size_t i = 0; i < examples.size(); ++i) {
                media["examples"]["example" + std::to_string(i + 1)] = {{"value", examples[i]}};
            }
        }
        operation["requestBody"] = {{"required", true}, {"content", {{"application/json", std::move(media)}}}};
    }

    // Responses (OpenAPI requires at least one)
    nlohmann::json responses = nlohmann::json::object();
    for (const auto& [status, response] : r.get_responses()) {
        nlohmann::json entry = {{"description", response.description.empty() ? "Response" : response.description}};
        if (!response.schema.is_null() || !response.example.is_null()) {
            nlohmann::json media = nlohmann::json::object();
            if (!response.schema.is_null()) media["schema"] = response.schema;
            if (!response.example.is_null()) media["example"] = response.example;
            entry["content"]["application/json"] = std::move(media);
        }
        responses[std::to_string(status)] = std::move(entry);
    }
    if (responses.empty()) responses["200"] = {{"description", "Successful response"}};
    operation["responses"] = std::move(responses);

    return operation;
}

nlohmann::json openapi_generator::generate() const {
    nlohmann::json document = {
        {"openapi", "3.1.0"},
        {"info", {{"title", title_}, {"version", version_}}}
    };
    if (!description_.empty()) document["info"]["description"] = description_;
    if (!servers_.empty()) document["servers"] = servers_;

    nlohmann::json paths = nlohmann::json::object();
    std::vector<std::string> tag_names;
    std::set<std::string> seen_tags;

    for (const auto& [http_method, routes] : router_.get_routes()) {
        std::string method_name = get_method(http_method);
        std::transform(method_name.begin(), method_name.end(), method_name.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        for (const auto& r : routes) {
            if (r.is_hidden()) continue;

            auto path = to_openapi_path(r.get_pattern());
            if (paths.contains(path) && paths[path].contains(method_name)) {
                // The router serves the first route registered for a pattern
                LOG_WARNING("OpenAPI: duplicate route {} {}, documenting the first one", method_name, path);
                continue;
            }

            auto operation = build_operation(r);
            for (const auto& hook : operation_hooks_) hook(r, method_name, operation);
            paths[path][method_name] = std::move(operation);

            for (const auto& tag : r.get_tags()) {
                if (seen_tags.insert(tag).second) tag_names.push_back(tag);
            }
        }
    }
    document["paths"] = std::move(paths);

    if (!tag_names.empty()) {
        document["tags"] = nlohmann::json::array();
        for (const auto& tag : tag_names) document["tags"].push_back({{"name", tag}});
    }

    const auto& components = router_.get_schema_components();
    if (!components.empty()) document["components"]["schemas"] = components;

    for (const auto& hook : document_hooks_) hook(document);
    return document;
}

} // namespace thinger::http

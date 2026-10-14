#include "openapi.hpp"
#include "routing/route_pattern.hpp"
#include "../../util/logger.hpp"
#include <algorithm>
#include <cctype>
#include <set>

namespace thinger::http {

namespace {

    // Every parameter in the pattern, completed with its documentation
    void add_path_parameters(const route& r, nlohmann::json& parameters) {
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
    }

    // Query parameters, only as documented
    void add_query_parameters(const route& r, nlohmann::json& parameters) {
        for (const auto& doc : r.get_param_docs()) {
            if (doc.in != "query") continue;
            nlohmann::json parameter = {{"name", doc.name}, {"in", "query"}, {"required", doc.required}};
            if (!doc.description.empty()) parameter["description"] = doc.description;
            parameter["schema"] = doc.schema.is_null() ? nlohmann::json{{"type", "string"}} : doc.schema;
            parameters.push_back(std::move(parameter));
        }
    }

    // The media types a route takes its body as. Routes reading the body themselves,
    // documented with a schema or examples: JSON
    std::vector<std::string> body_media_types(const route& r, const body_settings& bodies) {
        std::vector<std::string> media_types;
        if (!r.get_accepted_types().empty()) {
            media_types = accepted_media_types(bodies.decoders, r.get_accepted_types());
        } else if (r.takes_json_body()) {
            media_types = accepted_media_types(bodies.decoders, bodies.accepted);
        }
        if (media_types.empty()) media_types.emplace_back(default_body_media_type);
        return media_types;
    }

    // The request body: the route schema and examples, for each media type it accepts (the
    // schema describes the decoded body, whatever its type). Null if the route has none
    nlohmann::json request_body(const route& r, const body_settings& bodies) {
        const auto& schema = r.get_schema();
        const auto& examples = r.get_examples();
        if (schema.is_null() && examples.empty() && !r.takes_json_body() && r.get_accepted_types().empty()) {
            return nullptr;
        }

        nlohmann::json media = {{"schema", schema.is_null() ? nlohmann::json::object() : schema}};
        if (examples.size() == 1) {
            media["example"] = examples.front();
        } else if (examples.size() > 1) {
            for (size_t i = 0; i < examples.size(); ++i) {
                media["examples"]["example" + std::to_string(i + 1)] = {{"value", examples[i]}};
            }
        }
        nlohmann::json content = nlohmann::json::object();
        for (const auto& type : body_media_types(r, bodies)) content[type] = media;
        return nlohmann::json{{"required", true}, {"content", std::move(content)}};
    }

    // OpenAPI requires at least one response
    nlohmann::json responses(const route& r) {
        nlohmann::json result = nlohmann::json::object();
        for (const auto& [status, response] : r.get_responses()) {
            nlohmann::json entry = {{"description", response.description.empty() ? "Response" : response.description}};
            if (!response.schema.is_null() || !response.example.is_null()) {
                nlohmann::json media = nlohmann::json::object();
                if (!response.schema.is_null()) media["schema"] = response.schema;
                if (!response.example.is_null()) media["example"] = response.example;
                entry["content"]["application/json"] = std::move(media);
            }
            result[std::to_string(status)] = std::move(entry);
        }
        if (result.empty()) result["200"] = {{"description", "Successful response"}};
        return result;
    }

}

openapi_generator& openapi_generator::server(const std::string& url, const std::string& description) {
    nlohmann::json entry = {{"url", url}};
    if (!description.empty()) entry["description"] = description;
    servers_.push_back(std::move(entry));
    return *this;
}

std::string openapi_generator::to_openapi_path(const std::string& pattern) {
    // Same parameter syntax as route: :name(regex) and :name
    std::string path;
    for (const auto& token : detail::parse_route_pattern(pattern)) {
        path += token.parameter ? "{" + token.text + "}" : token.text;
    }
    return path;
}

nlohmann::json openapi_generator::build_operation(const route& r) const {
    nlohmann::json operation = nlohmann::json::object();

    if (!r.get_summary().empty()) operation["summary"] = r.get_summary();
    if (!r.get_description().empty()) operation["description"] = r.get_description();
    if (!r.get_tags().empty()) operation["tags"] = r.get_tags();
    if (!r.get_operation_id().empty()) operation["operationId"] = r.get_operation_id();
    if (r.is_deprecated()) operation["deprecated"] = true;

    nlohmann::json parameters = nlohmann::json::array();
    add_path_parameters(r, parameters);
    add_query_parameters(r, parameters);
    if (!parameters.empty()) operation["parameters"] = std::move(parameters);

    auto body = request_body(r, bodies_ ? *bodies_ : body_settings::defaults());
    if (!body.is_null()) operation["requestBody"] = std::move(body);

    operation["responses"] = responses(r);

    // Limits set on the route or its group, as extensions
    if (const auto& limit = r.get_max_body_size()) operation["x-max-body-size"] = *limit;
    if (const auto& timeout = r.get_timeout(); timeout && timeout->count() > 0) {
        operation["x-timeout-ms"] = timeout->count();
    }

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

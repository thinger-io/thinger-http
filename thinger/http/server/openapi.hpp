#ifndef THINGER_HTTP_OPENAPI_HPP
#define THINGER_HTTP_OPENAPI_HPP

#include "routing/route_handler.hpp"
#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <vector>

namespace thinger::http {

// Builds an OpenAPI 3.1 document from the routes of a router: paths and parameters from
// the route patterns, and the rest from the route documentation (summary, tags, params,
// responses, examples) and its body schema. Hidden routes are left out.
//
// The library does not interpret route metadata: use the hooks to turn it into whatever
// the application needs (x- extensions, security requirements...).
class openapi_generator {
public:
    // Called for each operation with its route, HTTP method (lowercase) and the operation
    // object already filled, so it can be completed or changed
    using operation_hook = std::function<void(const route&, const std::string& method, nlohmann::json& operation)>;

    // Called with the whole document once built (e.g. to add securitySchemes)
    using document_hook = std::function<void(nlohmann::json& document)>;

    explicit openapi_generator(const route_handler& router) : router_(router) {}

    openapi_generator& title(const std::string& value) { title_ = value; return *this; }
    openapi_generator& version(const std::string& value) { version_ = value; return *this; }
    openapi_generator& description(const std::string& value) { description_ = value; return *this; }
    openapi_generator& server(const std::string& url, const std::string& description = "");

    openapi_generator& on_operation(operation_hook hook) { operation_hooks_.push_back(std::move(hook)); return *this; }
    openapi_generator& on_document(document_hook hook) { document_hooks_.push_back(std::move(hook)); return *this; }

    nlohmann::json generate() const;

    // Convert a route pattern to an OpenAPI path: /users/:id([0-9]+) -> /users/{id}
    static std::string to_openapi_path(const std::string& pattern);

private:
    nlohmann::json build_operation(const route& r) const;

    const route_handler& router_;
    std::string title_ = "API";
    std::string version_ = "1.0.0";
    std::string description_;
    nlohmann::json servers_ = nlohmann::json::array();
    std::vector<operation_hook> operation_hooks_;
    std::vector<document_hook> document_hooks_;
};

} // namespace thinger::http

#endif // THINGER_HTTP_OPENAPI_HPP

#ifndef THINGER_HTTP_VIRTUAL_HOST_HPP
#define THINGER_HTTP_VIRTUAL_HOST_HPP

#include "routing/route_handler.hpp"
#include "routing/route.hpp"
#include "routing/route_registrar.hpp"
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace thinger::http {

class route_group;

// Set of routes served for a host name (see http_application::host). The application
// itself is the default virtual host: its routes answer the requests for hosts not registered.
class virtual_host : public route_registrar<virtual_host> {
public:
    // Default host ("*")
    virtual_host();

    // Host registered by name: an exact name ("api.example.com"), or a pattern where "*"
    // matches one label ("*.example.com") and ":name" or ":name(regex)" (a regex without
    // capture groups) a label available as a request parameter (":tenant.example.com" ->
    // req["tenant"]). Case-insensitive.
    explicit virtual_host(const std::string& name);

    // Host matched by a regular expression on the whole host name (case-insensitive);
    // its capture groups are available through request::get_host_matches()
    virtual_host(const std::string& name, const std::regex& pattern);

    virtual ~virtual_host() = default;
    virtual_host(const virtual_host&) = delete;
    virtual_host& operator=(const virtual_host&) = delete;

    // Route registration: get, post, put, del, patch, head and options (see route_registrar)

    // Group of routes sharing a path prefix, tags and metadata (see route_group)
    route_group group(const std::string& prefix);

    // Shared JSON schema (OpenAPI component) that route schemas can reference with
    // {"$ref": "#/components/schemas/<name>"}. Register it before the routes using it.
    void schema_component(const std::string& name, nlohmann::json schema);

    // Static file serving
    void serve_static(const std::string& url_prefix,
                      const std::string& directory,
                      const std::string& fallback = "index.html");

    // Fallback handler, called instead of answering 404/405 when no route matches: takes
    // (request&, response&) or (response&), synchronous or coroutine. nullptr (or an empty
    // std::function) removes it.
    template<request_handler_callable F>
    void set_not_found_handler(F&& handler) {
        router_.set_fallback_handler(std::forward<F>(handler));
    }
    void set_not_found_handler(std::nullptr_t) { router_.set_fallback_handler(nullptr); }

    // Name the host was registered with ("*" for the default host)
    const std::string& name() const { return name_; }

    // Whether the host is matched by a pattern instead of its exact name
    bool is_pattern() const { return pattern_.has_value(); }

    // Match a host name (lowercase, without port) against the pattern; `matches` gets the
    // whole host and the captures. Always false for exact and default hosts.
    bool matches(const std::string& host, std::smatch& matches) const;

    // Names of the pattern captures, in order (empty for unnamed ones, such as "*")
    const std::vector<std::string>& get_parameters() const { return parameters_; }

    // Access to router for advanced use cases (e.g. an openapi_generator for this host)
    route_handler& router() { return router_; }
    const route_handler& router() const { return router_; }

    // Whether a host name is a pattern for virtual_host(name): it has "*" or ":name"
    static bool is_host_pattern(const std::string& name);

    // Host name of a Host header value: lowercase, without port nor trailing dot
    // ("API.Example.com:8080" -> "api.example.com", "[::1]:80" -> "[::1]")
    static std::string normalize_host(std::string_view host);

protected:
    route_handler router_;

private:
    friend class route_registrar<virtual_host>;
    route& make_route(method http_method, const std::string& path) {
        return router_[http_method][path];
    }

    std::string name_;
    std::optional<std::regex> pattern_;
    std::vector<std::string> parameters_;
};

// Routes registered through a group get its path prefix, and inherit its tags and
// metadata (a route can still override a metadata key with its own meta()).
class route_group : public route_registrar<route_group> {
public:
    route_group(virtual_host& host, std::string prefix)
        : host_(host), prefix_(std::move(prefix)) {}

    route_group& tag(const std::string& name) {
        tags_.push_back(name);
        return *this;
    }

    route_group& meta(const std::string& key, nlohmann::json value) {
        metadata_[key] = std::move(value);
        return *this;
    }

    // Nested group: prefix appended, tags and metadata inherited
    route_group group(const std::string& prefix) const {
        route_group nested(*this);
        nested.prefix_ += prefix;
        return nested;
    }

    const std::string& prefix() const { return prefix_; }

private:
    friend class route_registrar<route_group>;
    route& make_route(method http_method, const std::string& path) {
        auto& r = host_.router()[http_method][prefix_ + path];
        r.tags(tags_);
        for (const auto& [key, value] : metadata_.items()) {
            r.meta(key, value);
        }
        return r;
    }

    virtual_host& host_;
    std::string prefix_;
    std::vector<std::string> tags_;
    nlohmann::json metadata_ = nlohmann::json::object();
};

inline route_group virtual_host::group(const std::string& prefix) {
    return route_group(*this, prefix);
}

} // namespace thinger::http

#endif // THINGER_HTTP_VIRTUAL_HOST_HPP

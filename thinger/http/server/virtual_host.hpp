#ifndef THINGER_HTTP_VIRTUAL_HOST_HPP
#define THINGER_HTTP_VIRTUAL_HOST_HPP

#include "routing/route_handler.hpp"
#include "routing/route.hpp"
#include "routing/route_registrar.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace thinger::http {

class route_group;

// How serve_static() serves the files of a directory
struct static_options {
    // File served for a directory (its own one), and for the paths without a file (the one
    // of the root directory, for single page applications); empty: 404 for both
    std::string fallback = "index.html";
    // Cache-Control of the files. By default, caches keep them but revalidate them on every
    // use: answered with 304 Not Modified (no body) while they do not change. Assets with a
    // hash in their names can be reused without asking: "public, max-age=31536000, immutable".
    std::string cache_control = "no-cache";
    // Precompressed files (as nginx gzip_static and brotli_static): a file is sent as its
    // "<file>.br" or "<file>.gz" sibling, if there is one not older than it and the client
    // accepts br or gzip (br first at equal quality), with that Content-Encoding and the
    // media type of the file. A build can then serve Brotli without compressing at runtime.
    bool precompressed = true;
};

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

    // Static files of `directory` served under `url_prefix` (GET, and HEAD through it), as
    // response::send_file does: with ETag and Last-Modified, answering conditional requests
    // with 304 and single byte ranges with 206, or their precompressed siblings (see
    // static_options). Paths resolving outside the directory (through symlinks) are
    // answered with 403.
    void serve_static(const std::string& url_prefix,
                      const std::string& directory,
                      const std::string& fallback = "index.html");
    void serve_static(const std::string& url_prefix,
                      const std::string& directory,
                      static_options options);

    // Fallback handler, called instead of answering 404/405 when no route matches: takes
    // (request&, response&) or (response&), synchronous or coroutine. nullptr (or an empty
    // std::function) removes it.
    template<request_handler_callable F>
    void set_not_found_handler(F&& handler) {
        router_.set_fallback_handler(std::forward<F>(handler));
    }
    void set_not_found_handler(std::nullptr_t) { router_.set_fallback_handler(nullptr); }

    // Limits of the requests served by this host: the default of its routes (a group or a
    // route can set its own, see route::max_body_size, route::timeout, route::decode_limits),
    // and the limits of its not-found handler (e.g. a reverse proxy for the whole host).
    // Unset, the application ones apply (http_application::set_max_body_size,
    // set_body_limits; no timeout). Precedence: route > group > host > application. On the
    // application itself, they are those of the default host only. Set before serving.
    virtual_host& max_body_size(size_t size) {
        max_body_size_ = size;
        return *this;
    }

    virtual_host& timeout(std::chrono::milliseconds timeout,
                          http_response::status status = http_response::status::gateway_timeout) {
        timeout_ = std::max(timeout, std::chrono::milliseconds{0});
        timeout_status_ = status;
        return *this;
    }

    virtual_host& decode_limits(body_limits limits) {
        decode_limits_ = limits;
        return *this;
    }

    // Limits set on the host (nullopt if unset)
    const std::optional<size_t>& get_max_body_size() const { return max_body_size_; }
    const std::optional<std::chrono::milliseconds>& get_timeout() const { return timeout_; }
    http_response::status get_timeout_status() const { return timeout_status_; }
    const std::optional<body_limits>& get_decode_limits() const { return decode_limits_; }

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

    // Limits of the host (unset: the application ones)
    std::optional<size_t> max_body_size_;
    std::optional<std::chrono::milliseconds> timeout_;
    http_response::status timeout_status_ = http_response::status::gateway_timeout;
    std::optional<body_limits> decode_limits_;
};

// Routes registered through a group get its path prefix, and inherit its tags, metadata
// and limits (a route can still override a metadata key with its own meta(), or a limit).
// They are applied as each route is registered: set them on the group first.
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

    // Limits of the routes registered afterwards (see route::max_body_size, route::timeout
    // and route::decode_limits)
    route_group& max_body_size(size_t size) {
        max_body_size_ = size;
        return *this;
    }

    route_group& timeout(std::chrono::milliseconds timeout,
                         http_response::status status = http_response::status::gateway_timeout) {
        timeout_ = {timeout, status};
        return *this;
    }

    route_group& decode_limits(body_limits limits) {
        decode_limits_ = limits;
        return *this;
    }

    // Nested group: prefix appended, tags, metadata and limits inherited
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
        if (max_body_size_) r.max_body_size(*max_body_size_);
        if (timeout_) r.timeout(timeout_->first, timeout_->second);
        if (decode_limits_) r.decode_limits(*decode_limits_);
        return r;
    }

    virtual_host& host_;
    std::string prefix_;
    std::vector<std::string> tags_;
    nlohmann::json metadata_ = nlohmann::json::object();
    std::optional<size_t> max_body_size_;
    std::optional<std::pair<std::chrono::milliseconds, http_response::status>> timeout_;
    std::optional<body_limits> decode_limits_;
};

inline route_group virtual_host::group(const std::string& prefix) {
    return route_group(*this, prefix);
}

} // namespace thinger::http

#endif // THINGER_HTTP_VIRTUAL_HOST_HPP

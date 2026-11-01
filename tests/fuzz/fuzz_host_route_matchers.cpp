// Fuzz target of the virtual host and route matchers: an application with a host of each kind
// (exact, "*" wildcard, ":name", ":name(regex)", host_regex()) and, on every host, routes of each
// kind the route tree matches (static, parameters, character classes, word alternatives, text
// mixed with parameters, regexes that fall back to std::regex, and the "/:path(.*)" of
// serve_static()). Each input is a Host value and a request target, up to the 16 KiB a header
// section may hold, dispatched in memory: host resolution and route matching run as for a
// request from a connection. No crash, stack overflow or timeout, and the host and route found
// agree with an independent reading of the name and the path.
//
// Input: the Host value, '\n', the request target ('/' is prepended if it does not start with
// one). Without '\n', the whole input is the Host and the target is "/".

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include "fuzz_common.hpp"
#include "thinger/http/server/http_application.hpp"
#include "thinger/http/server/request.hpp"
#include "thinger/http/server/response.hpp"

namespace {

    using namespace thinger::http;

    // Longest host name matched against the pattern hosts (as http_application resolves hosts)
    constexpr size_t max_host_name_size = 253;

    // The handlers answer "<host>\n<route>\n<parameter>" (the parameter last: it may hold any byte)
    void add_routes(virtual_host& host, const std::string& name) {
        auto answer = [name](const std::string& route, const std::string& parameter = {}) {
            return [name, route, parameter](request& req, response& res) {
                res.send(name + "\n" + route + "\n" + (parameter.empty() ? std::string() : req[parameter]));
            };
        };
        // Static segments
        host.get("/", answer("root"));
        host.get("/health", answer("health"));
        host.get("/api/v1/status", answer("status"));
        // Parameters, alone and after a static segment
        host.get("/users/:id", answer("user", "id"));
        // Character classes with repetitions (compiled, no std::regex)
        host.get("/users/:id/items/:item([0-9]+)", answer("item", "item"));
        host.get("/uuid/:id([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})", answer("uuid", "id"));
        host.get("/tags/:tag([a-zA-Z0-9_-]{1,32})", answer("tag", "tag"));
        // Word alternatives
        host.get("/devices/:device/:resource(properties|buckets|logs)", answer("resource", "resource"));
        // Text mixed with parameters in a segment
        host.get("/files/:name.:ext", answer("file", "ext"));
        host.get("/versions/:a([a-z]+)-:b([a-z0-9-]+)", answer("version", "b"));
        // Regexes the route tree does not compile: std::regex on the segment (a group), and on the
        // whole path (an anchor or a word boundary depends on the text around the parameter)
        host.get("/fallback/:code((ab|cd)+)", answer("fallback", "code"));
        host.get("/words/:word(\\w+\\b)", answer("word", "word"));
        // Parameters that may match '/': with more route after them, and as serve_static() registers
        host.get("/:path(.+)/raw", answer("raw", "path"));
        host.get("/static/:path(.*)", answer("static", "path"));
        host.set_not_found_handler([name](response& res) { res.send(name + "\n404\n"); });
    }

    struct application {
        http_application app;

        application() {
            add_routes(app, "default");
            add_routes(app.host("api.example.com"), "exact");
            add_routes(app.host("*.example.com"), "wildcard");
            add_routes(app.host(":tenant.example.org"), "tenant");
            add_routes(app.host(":device([a-z0-9-]+).devices.example.net"), "device");
            add_routes(app.host_regex("(.+)\\.(eu|us)\\.regex\\.example"), "regex");
        }
    };

    // Whether `name` is a single non-empty label followed by `suffix` (what "*" or ":name" match)
    bool is_label_of(std::string_view name, std::string_view suffix) {
        if (!name.ends_with(suffix)) return false;
        auto label = name.substr(0, name.size() - suffix.size());
        return !label.empty() && label.find('.') == std::string_view::npos;
    }

    // Line `index` of an answer, the last one with the rest of it
    std::string_view field(std::string_view text, size_t index, bool last = false) {
        for (size_t i = 0; i < index; ++i) {
            auto newline = text.find('\n');
            if (newline == std::string_view::npos) return {};
            text.remove_prefix(newline + 1);
        }
        return last ? text : text.substr(0, text.find('\n'));
    }

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static auto* fixture = new application();

    std::string_view input(reinterpret_cast<const char*>(data), size);
    auto newline = input.find('\n');
    std::string host(input.substr(0, newline));
    std::string target = newline == std::string_view::npos ? std::string("/") : std::string(input.substr(newline + 1));
    if (!target.starts_with('/')) target.insert(target.begin(), '/');

    auto http_request = std::make_shared<thinger::http::http_request>();
    http_request->set_method(thinger::http::method::GET);
    http_request->set_uri(target);
    http_request->set_header(thinger::http::header::host, host);

    auto result = fuzz::run(fixture->app.dispatch(http_request));
    fuzz::check(result != nullptr, "dispatch without a response");
    const std::string& body = result->get_content();
    auto served = field(body, 0);
    auto route = field(body, 1);
    auto parameter = field(body, 2, true);
    auto detail = "host: " + fuzz::escape(host) + "\ntarget: " + fuzz::escape(target) + "\nanswer: " + fuzz::escape(body);
    fuzz::check(!served.empty() && !route.empty(), "answer not from a handler", detail);

    // Host resolution: exact names first; a name longer than any DNS name never reaches the
    // patterns; otherwise "*.example.com" (the first pattern) takes every label it matches
    auto name = thinger::http::virtual_host::normalize_host(host.empty() ? http_request->get_host() : host);
    if (name == "api.example.com") {
        fuzz::check(served == "exact", "exact host not found", detail);
    } else if (name.size() > max_host_name_size) {
        fuzz::check(served == "default", "long host name served by a pattern host", detail);
    } else {
        fuzz::check((served == "wildcard") == is_label_of(name, ".example.com"), "wildcard host mismatch", detail);
        if (served == "tenant") fuzz::check(is_label_of(name, ".example.org"), "tenant host mismatch", detail);
        if (served == "device") fuzz::check(is_label_of(name, ".devices.example.net"), "device host mismatch", detail);
        if (served == "regex") fuzz::check(name.ends_with(".regex.example"), "regex host mismatch", detail);
    }

    // Route matching, on the path as sent (up to '?')
    std::string_view path(target);
    path = path.substr(0, path.find('?'));
    if (route == "user") {
        fuzz::check(path == "/users/" + std::string(parameter) && !parameter.empty() &&
                    parameter.find('/') == std::string_view::npos, "parameter is not the segment", detail);
    } else if (route == "item") {
        fuzz::check(!parameter.empty() && parameter.find_first_not_of("0123456789") == std::string_view::npos &&
                    path.ends_with("/items/" + std::string(parameter)), "[0-9]+ parameter mismatch", detail);
    } else if (route == "static") {
        fuzz::check(path == "/static/" + std::string(parameter), "catch-all parameter is not the rest of the path", detail);
    } else if (route == "raw") {
        fuzz::check(path == "/" + std::string(parameter) + "/raw" && !parameter.empty(), "/:path(.+)/raw mismatch", detail);
    } else if (route == "resource") {
        fuzz::check(parameter == "properties" || parameter == "buckets" || parameter == "logs", "word not in the alternatives", detail);
    } else if (route == "root") {
        fuzz::check(path == "/", "root route for another path", detail);
    }
    return 0;
}

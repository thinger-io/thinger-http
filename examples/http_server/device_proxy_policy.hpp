#ifndef THINGER_HTTP_EXAMPLES_DEVICE_PROXY_POLICY_HPP
#define THINGER_HTTP_EXAMPLES_DEVICE_PROXY_POLICY_HPP

// Policy of an application proxying the web interface of devices, shown inside a console
// (e.g. in an iframe) under a path of its own: /devices/<id>/services/web/...
//
// None of these rules are defaults of http::reverse_proxy, which never touches the headers
// of the target beyond the hop-by-hop ones. Each rule weakens a protection that the device
// pages ask for, so an application applies only those it needs, knowing what they cost:
//
//   - Frame protections (X-Frame-Options, CSP frame-ancestors) stop other sites from
//     embedding the page and tricking users into clicking on it (clickjacking). Removing
//     them lets any site embed the device pages. The recommended variant replaces them with
//     a frame-ancestors directive allowing only the console origin.
//   - Cookies: dropping Domain keeps them on the proxy host (they were meant for the device
//     host), which is safe. Forcing "SameSite=None; Secure" makes the browser send them on
//     requests from other sites too, widening the exposure to cross-site request forgery:
//     only for a console on another site than the proxy, over HTTPS.
//   - Location: a redirect to the device itself (its host, or an absolute path) would leave
//     the proxy; it is rewritten to the proxy path. The URL is parsed, never matched as a
//     substring, and redirects to other hosts are left alone.

#include <thinger/http/common/http_response.hpp>
#include <thinger/util/ascii.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace device_proxy {

    namespace ascii = thinger::util::ascii;

    struct policy {
        // Path the device is served under, e.g. "/devices/d1/services/web"
        std::string base_path;

        // Frame protections. With an origin (e.g. "https://console.example.com"), the page may
        // be embedded by it only (recommended). Empty: they are kept, unless `allow_any_frame`
        // removes them (any site may then embed the page: clickjacking).
        std::string console_origin;
        bool allow_any_frame = false;

        // Make cookies cross-site ("SameSite=None; Secure"): only for a console on another site
        bool cross_site_cookies = false;
    };

    inline std::string_view trim(std::string_view value) {
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
        return value;
    }

    // Name of a "name=value" (or "name") cookie attribute or CSP directive
    inline std::string_view name_of(std::string_view item) {
        item = trim(item);
        auto end = item.find_first_of("= \t");
        return item.substr(0, end);
    }

    inline std::vector<std::string_view> split(std::string_view value, char separator) {
        std::vector<std::string_view> items;
        while (true) {
            auto position = value.find(separator);
            items.push_back(value.substr(0, position));
            if (position == std::string_view::npos) return items;
            value.remove_prefix(position + 1);
        }
    }

    // Set-Cookie without Domain, and cross-site if asked (adding what is missing)
    inline std::string rewrite_cookie(std::string_view cookie, bool cross_site) {
        auto parts = split(cookie, ';');
        std::string result(trim(parts.front()));    // name=value
        bool secure = false;
        bool same_site = false;
        for (size_t i = 1; i < parts.size(); ++i) {
            auto attribute = trim(parts[i]);
            auto name = name_of(attribute);
            if (name.empty() || ascii::iequals(name, "Domain")) continue;
            if (ascii::iequals(name, "Secure")) secure = true;
            if (ascii::iequals(name, "SameSite")) {
                // replaced, as another value would keep the cookie from cross-site requests
                if (cross_site) continue;
                same_site = true;
            }
            result += "; ";
            result += attribute;
        }
        if (cross_site) {
            if (!secure) result += "; Secure";
            if (!same_site) result += "; SameSite=None";
        }
        return result;
    }

    // Content-Security-Policy whose frame-ancestors directive allows only `origin`
    inline std::string rewrite_csp(std::string_view csp, const std::string& origin) {
        std::string result;
        for (auto directive : split(csp, ';')) {
            directive = trim(directive);
            if (directive.empty() || ascii::iequals(name_of(directive), "frame-ancestors")) continue;
            if (!result.empty()) result += "; ";
            result += directive;
        }
        if (!result.empty()) result += "; ";
        return result + "frame-ancestors " + origin;
    }

    // Location of a redirect, kept under the proxy: absolute URLs of the device itself (its
    // host is one of `device_hosts`) become paths, and absolute paths go under the base path.
    // URLs of other hosts, and relative references, are left as they are.
    inline std::string rewrite_location(std::string_view location, const std::string& base_path,
                                        const std::vector<std::string>& device_hosts) {
        std::string path(location);
        if (auto scheme_end = location.find("://"); scheme_end != std::string_view::npos) {
            auto scheme = location.substr(0, scheme_end);
            if (!ascii::iequals(scheme, "http") && !ascii::iequals(scheme, "https")) return std::string(location);
            auto rest = location.substr(scheme_end + 3);
            auto authority_end = rest.find_first_of("/?#");
            auto authority = rest.substr(0, authority_end);
            // userinfo is not a host ("http://device@evil.example/" is evil.example)
            if (authority.find('@') != std::string_view::npos) return std::string(location);
            bool device = false;
            for (const auto& host : device_hosts) device = device || ascii::iequals(authority, host);
            if (!device) return std::string(location);
            // the path, query and fragment of the device, from its root
            path = authority_end == std::string_view::npos ? "" : std::string(rest.substr(authority_end));
            if (!path.starts_with("/")) path.insert(0, "/");
        } else if (location.starts_with("//") || !location.starts_with("/")) {
            // another host (network-path reference), or relative to the current path
            return std::string(location);
        }
        std::string_view resource(path);
        resource = resource.substr(0, resource.find_first_of("?#"));
        if (resource == base_path || resource.starts_with(base_path + "/")) return path;
        return base_path + path;
    }

    // Apply the policy to the response of a device. `device_hosts` are the hosts the device may
    // use to refer to itself: the Host sent to it, and the one of the client if it was preserved.
    inline void apply(const policy& rules, const std::vector<std::string>& device_hosts,
                      thinger::http::http_response& response) {
        if (!rules.console_origin.empty() || rules.allow_any_frame) {
            // X-Frame-Options cannot allow a single origin: frame-ancestors replaces it
            response.remove_headers("X-Frame-Options");
        }
        auto& headers = response.get_headers();
        for (auto it = headers.begin(); it != headers.end();) {
            auto& [name, value] = *it;
            if (ascii::iequals(name, "Content-Security-Policy")) {
                if (!rules.console_origin.empty()) {
                    value = rewrite_csp(value, rules.console_origin);
                } else if (rules.allow_any_frame) {
                    it = headers.erase(it);
                    continue;
                }
            } else if (ascii::iequals(name, "Set-Cookie")) {
                value = rewrite_cookie(value, rules.cross_site_cookies);
            } else if (ascii::iequals(name, "Location")) {
                value = rewrite_location(value, rules.base_path, device_hosts);
            }
            ++it;
        }
        // pages without a policy of their own get one, if a single origin may embed them
        if (!rules.console_origin.empty() && !response.has_header("Content-Security-Policy")) {
            response.add_header("Content-Security-Policy", "frame-ancestors " + rules.console_origin);
        }
    }

}

#endif

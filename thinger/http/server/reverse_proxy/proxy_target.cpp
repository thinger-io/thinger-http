// Targets of the reverse proxy, and their URLs
#include "proxy_exchange.hpp"
#include "../../../util/ascii.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>

namespace thinger::http {

namespace {

    using proxy_detail::is_ip_address;
    using proxy_detail::is_request_target;
    namespace ascii = ::thinger::util::ascii;

    // Transport and default port of a URL scheme; false if it is not one of a target
    bool set_scheme(std::string_view scheme, proxy_target& target) {
        if (ascii::iequals(scheme, "http") || ascii::iequals(scheme, "ws")) {
            target.type = proxy_target::transport::tcp;
            target.port = 80;
        } else if (ascii::iequals(scheme, "https") || ascii::iequals(scheme, "wss")) {
            target.type = proxy_target::transport::tls;
            target.port = 443;
        } else {
            return false;
        }
        return true;
    }

    // A path to prepend to the forwarded ones (empty for none), without query nor fragment
    bool is_base_path(std::string_view path) {
        return path.empty() || (path.find('?') == std::string_view::npos && is_request_target(path));
    }

    bool is_host_name(std::string_view host) {
        return !host.empty() && std::all_of(host.begin(), host.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '-' || c == '_';
        });
    }

    // A port number, 1 to 65535
    bool parse_port(std::string_view text, uint16_t& port) {
        unsigned value = 0;
        auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || end != text.data() + text.size() || value == 0 || value > 65535) return false;
        port = static_cast<uint16_t>(value);
        return true;
    }

    // Host and port of an authority: "host[:port]" or "[IPv6 address][:port]"
    bool set_authority(std::string_view authority, proxy_target& target) {
        bool bracketed = authority.front() == '[';
        size_t host_end = bracketed ? authority.find(']') : authority.find(':');
        if (bracketed && host_end == std::string_view::npos) return false;
        auto host = bracketed ? authority.substr(1, host_end - 1) : authority.substr(0, host_end);
        size_t port_start = bracketed ? host_end + 1 : host_end;
        if (port_start < authority.size()) {
            if (authority[port_start] != ':' || !parse_port(authority.substr(port_start + 1), target.port)) return false;
        }
        target.host = std::string(host);
        return bracketed ? is_ip_address(target.host) : is_host_name(host);
    }

}

std::optional<proxy_target> proxy_target::from_url(std::string_view url) {
    auto scheme_end = url.find("://");
    proxy_target target;
    if (scheme_end == std::string_view::npos || !set_scheme(url.substr(0, scheme_end), target)) return std::nullopt;

    auto rest = url.substr(scheme_end + 3);
    auto path_start = rest.find('/');
    auto authority = rest.substr(0, path_start);
    auto base_path = path_start == std::string_view::npos ? std::string_view{} : rest.substr(path_start);
    // No user information, query nor fragment
    if (authority.empty() || authority.find_first_of("@?#") != std::string_view::npos || !is_base_path(base_path) ||
        !set_authority(authority, target)) {
        return std::nullopt;
    }
    while (base_path.ends_with('/')) base_path.remove_suffix(1);
    target.base_path = std::string(base_path);
    return target;
}

proxy_target proxy_target::unix_socket(std::string path, std::string base_path) {
    proxy_target target;
    target.type = transport::unix_socket;
    target.socket_path = std::move(path);
    target.base_path = std::move(base_path);
    return target;
}

std::string proxy_target::authority() const {
    if (type == transport::unix_socket) return "localhost";
    auto name = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    bool default_port = (type == transport::tls && port == 443) || (type == transport::tcp && port == 80);
    return default_port ? name : name + ":" + std::to_string(port);
}

} // namespace thinger::http

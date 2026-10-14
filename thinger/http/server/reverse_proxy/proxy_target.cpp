// Targets of the reverse proxy, and their URLs
#include "proxy_exchange.hpp"
#include "../../../util/ascii.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>

namespace thinger::http {

namespace {

    using proxy_detail::is_ip_address;
    using proxy_detail::is_request_target;
    namespace ascii = ::thinger::util::ascii;

}

std::optional<proxy_target> proxy_target::from_url(std::string_view url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) return std::nullopt;
    auto scheme = url.substr(0, scheme_end);

    proxy_target target;
    if (ascii::iequals(scheme, "http") || ascii::iequals(scheme, "ws")) {
        target.type = transport::tcp;
        target.port = 80;
    } else if (ascii::iequals(scheme, "https") || ascii::iequals(scheme, "wss")) {
        target.type = transport::tls;
        target.port = 443;
    } else {
        return std::nullopt;
    }

    auto rest = url.substr(scheme_end + 3);
    auto path_start = rest.find('/');
    auto authority = rest.substr(0, path_start);
    if (path_start != std::string_view::npos) target.base_path = std::string(rest.substr(path_start));
    if (authority.empty() || authority.find_first_of("@?#") != std::string_view::npos ||
        target.base_path.find_first_of("?#") != std::string::npos ||
        (!target.base_path.empty() && !is_request_target(target.base_path))) {
        return std::nullopt;
    }

    // host, [IPv6] host, and port
    std::string_view port;
    if (authority.front() == '[') {
        auto close = authority.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        target.host = std::string(authority.substr(1, close - 1));
        auto after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') return std::nullopt;
            port = after.substr(1);
        }
        if (!is_ip_address(target.host)) return std::nullopt;
    } else {
        auto colon = authority.find(':');
        target.host = std::string(authority.substr(0, colon));
        if (colon != std::string_view::npos) port = authority.substr(colon + 1);
        bool valid_host = !target.host.empty() && std::all_of(target.host.begin(), target.host.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '-' || c == '_';
        });
        if (!valid_host) return std::nullopt;
    }
    if (port.data() != nullptr && !port.empty()) {
        unsigned value = 0;
        auto [end, ec] = std::from_chars(port.data(), port.data() + port.size(), value);
        if (ec != std::errc{} || end != port.data() + port.size() || value == 0 || value > 65535) return std::nullopt;
        target.port = static_cast<uint16_t>(value);
    } else if (authority.back() == ':') {
        return std::nullopt;
    }

    while (!target.base_path.empty() && target.base_path.back() == '/') target.base_path.pop_back();
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

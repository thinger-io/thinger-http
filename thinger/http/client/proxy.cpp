#include "proxy.hpp"
#include "../../util/ascii.hpp"
#include "../../util/base64.hpp"
#include <algorithm>
#include <cstdlib>
#include <initializer_list>

namespace thinger::http {

namespace {

    using boost::asio::ip::address;

    std::string_view trim(std::string_view value) {
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
        return value;
    }

    std::string lowercase(std::string_view value) {
        std::string result(value);
        util::ascii::to_lower(result);
        return result;
    }

    bool all_digits(std::string_view value) {
        return !value.empty() && std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
    }

    bool is_port(std::string_view port) {
        if (port.size() > 5 || !all_digits(port)) return false;
        auto value = std::stoul(std::string(port));
        return value > 0 && value <= 65535;
    }

    int hex_digit(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    // Percent-decoding of URL user info ('+' is not a space there, unlike in a query)
    std::optional<std::string> percent_decode(std::string_view in) {
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); ++i) {
            if (in[i] != '%') {
                out += in[i];
                continue;
            }
            if (i + 2 >= in.size()) return std::nullopt;
            int hi = hex_digit(in[i + 1]);
            int lo = hex_digit(in[i + 2]);
            if (hi < 0 || lo < 0) return std::nullopt;
            out += static_cast<char>((hi << 4) | lo);
            i += 2;
        }
        return out;
    }

    // An IP address (IPv4-mapped IPv6 addresses as IPv4), or nothing
    std::optional<address> parse_ip(std::string_view value) {
        boost::system::error_code ec;
        auto ip = boost::asio::ip::make_address(std::string(value), ec);
        if (ec) return std::nullopt;
        if (ip.is_v6() && ip.to_v6().is_v4_mapped()) {
            return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, ip.to_v6());
        }
        return ip;
    }

    bool in_network(const address& ip, const address& network, unsigned prefix) {
        if (ip.is_v4() != network.is_v4()) return false;
        auto bytes = [](const address& a) {
            std::vector<unsigned char> result;
            if (a.is_v4()) {
                auto b = a.to_v4().to_bytes();
                result.assign(b.begin(), b.end());
            } else {
                auto b = a.to_v6().to_bytes();
                result.assign(b.begin(), b.end());
            }
            return result;
        };
        auto a = bytes(ip);
        auto b = bytes(network);
        for (size_t i = 0; i < a.size() && prefix > 0; ++i) {
            unsigned bits = std::min(prefix, 8u);
            auto mask = static_cast<unsigned char>(0xFF << (8 - bits));
            if ((a[i] & mask) != (b[i] & mask)) return false;
            prefix -= bits;
        }
        return true;
    }

    // Host name for comparisons: lowercase, without IPv6 brackets nor the trailing dot of a
    // fully qualified name
    std::string normalize_host(std::string_view host) {
        if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
            host = host.substr(1, host.size() - 2);
        }
        if (host.size() > 1 && host.back() == '.') host.remove_suffix(1);
        return lowercase(host);
    }

    // Take the scheme out of a proxy URL: why it is not supported, or empty if it is http or
    // there is none
    std::string take_scheme(std::string_view& url) {
        auto separator = url.find("://");
        if (separator == std::string_view::npos) return {};
        auto scheme = lowercase(url.substr(0, separator));
        if (scheme == "https") return "HTTPS proxies (TLS to the proxy) are not supported: use an http:// proxy URL";
        if (scheme.starts_with("socks")) return "SOCKS proxies are not supported";
        if (scheme != "http") return "unsupported proxy scheme: " + scheme;
        url.remove_prefix(separator + 3);
        return {};
    }

    // Basic credentials from the user info of a URL; nothing if its percent-encoding is invalid
    std::optional<std::string> basic_credentials(std::string_view user_info) {
        auto colon = user_info.find(':');
        auto user = percent_decode(user_info.substr(0, colon));
        auto password = percent_decode(colon == std::string_view::npos ? std::string_view{} : user_info.substr(colon + 1));
        if (!user || !password) return std::nullopt;
        return "Basic " + util::base64::encode(*user + ":" + *password);
    }

    bool is_host_name(std::string_view host) {
        return !host.empty() && std::all_of(host.begin(), host.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '.' || c == '_';
        });
    }

    // The host and port of a proxy URL authority (without user info); false if the host is
    // invalid
    bool split_authority(std::string_view authority, std::string_view& host, std::string_view& port) {
        if (!authority.starts_with('[')) {
            auto colon = authority.find(':');
            host = authority.substr(0, colon);
            if (colon != std::string_view::npos) port = authority.substr(colon + 1);
            return is_host_name(host);
        }
        auto close = authority.find(']');
        if (close == std::string_view::npos) return false;
        host = authority.substr(1, close - 1);
        auto rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return false;
            port = rest.substr(1);
        }
        auto ip = parse_ip(host);
        return ip && ip->is_v6();
    }

    // ".example.com" and "*.example.com" are "example.com": the domain and its subdomains
    std::string_view no_proxy_domain(std::string_view host) {
        if (host.starts_with("*.")) host.remove_prefix(1);
        while (host.starts_with('.')) host.remove_prefix(1);
        if (host.ends_with('.')) host.remove_suffix(1);
        return host;
    }

    // Split a NO_PROXY entry into its host, port and the bits of a CIDR range: "[::1]",
    // "[::1]:8080", "[fd00::]/8", "host:port", "10.0.0.0/8". False if it cannot be parsed
    bool split_no_proxy_entry(std::string_view& host, std::string& port, std::optional<std::string_view>& bits) {
        if (host.starts_with('[')) {
            auto close = host.find(']');
            if (close == std::string_view::npos) return false;
            auto rest = host.substr(close + 1);
            host = host.substr(1, close - 1);
            if (rest.starts_with(':')) {
                port = std::string(rest.substr(1));
            } else if (rest.starts_with('/')) {
                bits = rest.substr(1);
            } else if (!rest.empty()) {
                return false;
            }
        } else if (std::count(host.begin(), host.end(), ':') == 1) {
            // A bare IPv6 address has more than one colon
            auto colon = host.find(':');
            port = std::string(host.substr(colon + 1));
            host = host.substr(0, colon);
        }
        if (!bits) {
            if (auto slash = host.find('/'); slash != std::string_view::npos) {
                bits = host.substr(slash + 1);
                host = host.substr(0, slash);
            }
        }
        return true;
    }

}

// --- proxy_config ---

proxy_config proxy_config::from_environment() {
    return from_environment([](const char* name) -> std::optional<std::string> {
        const char* value = std::getenv(name);
        if (!value) return std::nullopt;
        return std::string(value);
    });
}

proxy_config proxy_config::from_environment(const env_reader& getenv) {
    auto first = [&](std::initializer_list<const char*> names) -> std::string {
        for (const char* name : names) {
            auto value = getenv(name);
            if (!value) continue;
            auto trimmed = trim(*value);
            if (!trimmed.empty()) return std::string(trimmed);
        }
        return {};
    };

    proxy_config config;
    // Never HTTP_PROXY: a CGI program gets the Proxy header of its request there (httpoxy)
    config.http = first({"http_proxy"});
    config.https = first({"https_proxy", "HTTPS_PROXY"});
    auto all = first({"all_proxy", "ALL_PROXY"});
    if (config.http.empty()) config.http = all;
    if (config.https.empty()) config.https = all;
    config.no_proxy = first({"no_proxy", "NO_PROXY"});
    return config;
}

// --- proxy_endpoint ---

std::optional<proxy_endpoint> proxy_endpoint::parse(std::string_view url, std::string* error) {
    auto fail = [&](std::string why) -> std::optional<proxy_endpoint> {
        if (error) *error = std::move(why);
        return std::nullopt;
    };

    url = trim(url);
    // Without a scheme, an HTTP proxy, as curl and Go take it
    if (auto why = take_scheme(url); !why.empty()) return fail(std::move(why));

    // The authority ends the URL, or a path (ignored)
    auto authority = url.substr(0, url.find_first_of("/?#"));

    proxy_endpoint endpoint;
    if (auto at = authority.rfind('@'); at != std::string_view::npos) {
        auto credentials = basic_credentials(authority.substr(0, at));
        if (!credentials) return fail("invalid percent-encoding in the proxy credentials");
        endpoint.authorization = std::move(*credentials);
        authority.remove_prefix(at + 1);
    }

    std::string_view host;
    std::string_view port;
    if (!split_authority(authority, host, port)) return fail("invalid proxy host");
    // The default port of the scheme, as Go and Node (curl uses 1080)
    if (port.empty()) port = "80";
    if (!is_port(port)) return fail("invalid proxy port");

    endpoint.host = lowercase(host);
    endpoint.port = std::string(port);
    return endpoint;
}

std::string proxy_endpoint::url() const {
    auto name = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    return "http://" + name + ":" + port;
}

std::string proxy_endpoint::key() const {
    return host + "|" + port + "|" + authorization;
}

// --- no_proxy_list ---

no_proxy_list::no_proxy_list(std::string_view list) {
    while (!list.empty()) {
        auto comma = list.find(',');
        auto token = lowercase(trim(list.substr(0, comma)));
        list.remove_prefix(comma == std::string_view::npos ? list.size() : comma + 1);
        if (token == "*") {
            all_ = true;
        } else if (!token.empty()) {
            if (auto e = parse_entry(token)) entries_.push_back(std::move(*e));
        }
    }
}

std::optional<no_proxy_list::entry> no_proxy_list::parse_entry(std::string_view host) {
    // Unparsable entries are ignored, as curl and Go do
    entry e;
    std::optional<std::string_view> bits;     // of a CIDR range
    if (!split_no_proxy_entry(host, e.port, bits)) return std::nullopt;
    if (!e.port.empty() && !is_port(e.port)) return std::nullopt;
    if (bits) {
        if (!all_digits(*bits) || bits->size() > 3) return std::nullopt;
        e.prefix = static_cast<unsigned>(std::stoul(std::string(*bits)));
        e.range = true;
    }

    if (auto ip = parse_ip(host)) {
        if (e.range && e.prefix > (ip->is_v4() ? 32u : 128u)) return std::nullopt;
        e.ip = *ip;
        return e;
    }
    auto domain = no_proxy_domain(host);
    if (e.range || domain.empty()) return std::nullopt;
    e.domain = std::string(domain);
    return e;
}

bool no_proxy_list::matches(std::string_view host, std::string_view port) const {
    if (all_) return true;
    auto name = normalize_host(host);
    auto ip = parse_ip(name);
    for (const auto& e : entries_) {
        if (!e.port.empty() && e.port != port) continue;
        if (e.ip) {
            if (ip && (e.range ? in_network(*ip, *e.ip, e.prefix) : *ip == *e.ip)) return true;
        } else if (name == e.domain ||
                   (name.size() > e.domain.size() && name.ends_with(e.domain) &&
                    name[name.size() - e.domain.size() - 1] == '.')) {
            return true;
        }
    }
    return false;
}

bool is_loopback_host(std::string_view host) {
    auto name = normalize_host(host);
    if (name == "localhost" || name.ends_with(".localhost")) return true;
    auto ip = parse_ip(name);
    return ip && ip->is_loopback();
}

// --- proxy_resolver ---

proxy_resolver::proxy proxy_resolver::parse(const std::string& url) {
    proxy result;
    if (url.empty()) return result;
    result.configured = true;
    std::string error;
    if (auto endpoint = proxy_endpoint::parse(url, &error)) {
        result.endpoint = std::make_shared<const proxy_endpoint>(std::move(*endpoint));
    } else {
        result.error = std::move(error);
    }
    return result;
}

proxy_resolver::proxy_resolver(const proxy_config& config)
    : http_(parse(config.http))
    , https_(parse(config.https))
    , no_proxy_(config.no_proxy) {
}

proxy_resolver::choice proxy_resolver::select(bool secure, std::string_view host, std::string_view port) const {
    const auto& proxy = secure ? https_ : http_;
    if (!proxy.configured) return {};
    // Whatever a proxy would do with them, these are this host, or listed as reached directly
    if (is_loopback_host(host) || no_proxy_.matches(host, port)) return {};
    return {proxy.endpoint, proxy.error};
}

}

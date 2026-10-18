#include "trusted_proxies.hpp"
#include "../common/headers.hpp"
#include <boost/algorithm/string.hpp>
#include <algorithm>
#include <charconv>
#include <optional>

namespace thinger::http {

namespace {

    using boost::asio::ip::address;

    std::string_view trim(std::string_view value) {
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
        return value;
    }

    // IPv4-mapped IPv6 addresses (::ffff:10.0.0.1, as reported by dual-stack sockets) as IPv4
    address normalize(const address& ip) {
        if (ip.is_v6() && ip.to_v6().is_v4_mapped()) {
            return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, ip.to_v6());
        }
        return ip;
    }

    // Address of a forwarded entry: "1.2.3.4", "1.2.3.4:80", "2001:db8::1", "[2001:db8::1]:80"
    std::optional<address> parse_address(std::string_view entry) {
        entry = trim(entry);
        if (entry.starts_with('[')) {
            auto close = entry.find(']');
            if (close == std::string_view::npos) return std::nullopt;
            entry = entry.substr(1, close - 1);
        } else if (std::count(entry.begin(), entry.end(), ':') == 1) {
            entry = entry.substr(0, entry.find(':'));
        }
        boost::system::error_code ec;
        auto ip = boost::asio::ip::make_address(std::string(entry), ec);
        if (ec) return std::nullopt;
        return normalize(ip);
    }

    // Split by `separator`, except inside quoted strings
    std::vector<std::string_view> split_unquoted(std::string_view value, char separator) {
        std::vector<std::string_view> parts;
        bool quoted = false;
        size_t start = 0;
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\\' && quoted) {
                ++i;
            } else if (value[i] == '"') {
                quoted = !quoted;
            } else if (value[i] == separator && !quoted) {
                parts.push_back(value.substr(start, i - start));
                start = i + 1;
            }
        }
        parts.push_back(value.substr(start));
        return parts;
    }

    std::string unquote(std::string_view value) {
        value = trim(value);
        if (value.size() < 2 || value.front() != '"' || value.back() != '"') return std::string(value);
        std::string result;
        for (size_t i = 1; i + 1 < value.size(); ++i) {
            if (value[i] == '\\' && i + 2 < value.size()) ++i;
            result += value[i];
        }
        return result;
    }

    std::string to_entry(const std::optional<address>& ip) {
        return ip ? ip->to_string() : std::string{};
    }

    // An element of a Forwarded header: its address (empty if it is not an IP) and its proto
    struct forwarded_element {
        std::string address;
        std::string proto;
    };

    std::vector<forwarded_element> parse_forwarded_elements(std::string_view value) {
        std::vector<forwarded_element> result;
        if (trim(value).empty()) return result;
        for (auto element : split_unquoted(value, ',')) {
            // element: pairs such as for=192.0.2.60;proto=http;by=203.0.113.43
            std::optional<address> ip;
            std::string proto;
            bool has_for = false;
            bool has_proto = false;
            for (auto pair : split_unquoted(element, ';')) {
                auto equals = pair.find('=');
                if (equals == std::string_view::npos) continue;
                auto name = trim(pair.substr(0, equals));
                // the first one of each parameter (each may appear once)
                if (!has_for && boost::iequals(name, "for")) {
                    has_for = true;
                    ip = parse_address(unquote(pair.substr(equals + 1)));
                } else if (!has_proto && boost::iequals(name, "proto")) {
                    has_proto = true;
                    proto = unquote(pair.substr(equals + 1));
                }
            }
            result.push_back({to_entry(ip), std::move(proto)});
        }
        return result;
    }

}

bool trusted_proxies::add(std::string_view value) {
    value = trim(value);
    auto slash = value.find('/');

    boost::system::error_code ec;
    auto ip = boost::asio::ip::make_address(std::string(value.substr(0, slash)), ec);
    if (ec) return false;

    unsigned max_prefix = ip.is_v4() ? 32 : 128;
    unsigned prefix = max_prefix;
    if (slash != std::string_view::npos) {
        auto bits = value.substr(slash + 1);
        auto [end, error] = std::from_chars(bits.data(), bits.data() + bits.size(), prefix);
        if (error != std::errc{} || end != bits.data() + bits.size() || bits.empty() || prefix > max_prefix) {
            return false;
        }
    }

    // A range of IPv4-mapped addresses is an IPv4 range
    if (ip.is_v6() && ip.to_v6().is_v4_mapped() && prefix >= 96) {
        ip = normalize(ip);
        prefix -= 96;
    }

    networks_.push_back({ip, prefix});
    return true;
}

bool trusted_proxies::in_network(const address& ip, const network& net) {
    if (ip.is_v4() != net.address.is_v4()) return false;

    auto compare = [&](const auto& a, const auto& b) {
        unsigned full_bytes = net.prefix / 8;
        unsigned remaining_bits = net.prefix % 8;
        if (!std::equal(a.begin(), a.begin() + full_bytes, b.begin())) return false;
        if (remaining_bits == 0) return true;
        auto mask = static_cast<uint8_t>(0xFF << (8 - remaining_bits));
        return (a[full_bytes] & mask) == (b[full_bytes] & mask);
    };

    if (ip.is_v4()) return compare(ip.to_v4().to_bytes(), net.address.to_v4().to_bytes());
    return compare(ip.to_v6().to_bytes(), net.address.to_v6().to_bytes());
}

bool trusted_proxies::contains(std::string_view ip) const {
    boost::system::error_code ec;
    auto parsed = boost::asio::ip::make_address(std::string(trim(ip)), ec);
    if (ec) return false;
    parsed = normalize(parsed);
    return std::any_of(networks_.begin(), networks_.end(), [&](const network& net) {
        return in_network(parsed, net);
    });
}

const std::string& trusted_proxies::header_name() const {
    static const std::string x_forwarded_for = "X-Forwarded-For";
    static const std::string forwarded = "Forwarded";
    return header_ == forwarded_header::forwarded ? forwarded : x_forwarded_for;
}

std::string trusted_proxies::client_ip(const std::string& peer, const std::vector<std::string>& header_values) const {
    if (!contains(peer)) return peer;

    // Forwarded addresses, left (client) to right (nearest proxy)
    std::vector<std::string> chain;
    for (const auto& value : header_values) {
        auto entries = header_ == forwarded_header::forwarded ? parse_forwarded(value) : parse_x_forwarded_for(value);
        chain.insert(chain.end(), entries.begin(), entries.end());
    }

    std::string client = peer;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (it->empty()) break;     // not an address: the proxy after it is the best we know
        client = *it;
        if (!contains(client)) break;
    }
    return client;
}

forwarded_client trusted_proxies::resolve(const std::string& peer, std::string_view scheme,
                                          const headers& request) const {
    forwarded_client client{peer, std::string(scheme)};
    if (!contains(peer)) return client;

    // Entries, left (client) to right (nearest proxy), with their scheme
    std::vector<forwarded_element> chain;
    // The scheme of every entry with X-Forwarded-For when the counts do not match: the last one
    std::optional<std::string> nearest_scheme;
    if (header_ == forwarded_header::forwarded) {
        for (const auto& value : request.get_headers_with_key(header_name())) {
            auto elements = parse_forwarded_elements(value);
            chain.insert(chain.end(), std::make_move_iterator(elements.begin()), std::make_move_iterator(elements.end()));
        }
    } else {
        for (const auto& value : request.get_headers_with_key(header_name())) {
            for (auto& address : parse_x_forwarded_for(value)) chain.push_back({std::move(address), {}});
        }
        std::vector<std::string> schemes;
        for (const auto& value : request.get_headers_with_key("X-Forwarded-Proto")) {
            if (trim(value).empty()) continue;
            for (auto entry : split_unquoted(value, ',')) schemes.emplace_back(trim(entry));
        }
        if (schemes.size() == chain.size()) {
            for (size_t i = 0; i < chain.size(); ++i) chain[i].proto = std::move(schemes[i]);
        } else if (!schemes.empty()) {
            nearest_scheme = std::move(schemes.back());
        }
    }

    // Walked from the right, as client_ip()
    const forwarded_element* last = nullptr;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        last = &*it;
        if (it->address.empty()) break;     // not an address: the proxy after it is the best we know
        client.ip = it->address;
        if (!contains(client.ip)) break;
    }

    auto forwarded = parse_scheme(nearest_scheme ? *nearest_scheme : last ? last->proto : std::string{});
    if (!forwarded.empty()) client.scheme = std::move(forwarded);
    return client;
}

std::string trusted_proxies::parse_scheme(std::string_view value) {
    auto scheme = unquote(value);
    if (boost::iequals(scheme, "https")) return "https";
    if (boost::iequals(scheme, "http")) return "http";
    return {};
}

std::vector<std::string> trusted_proxies::parse_x_forwarded_for(std::string_view value) {
    std::vector<std::string> result;
    if (trim(value).empty()) return result;
    for (auto entry : split_unquoted(value, ',')) {
        result.push_back(to_entry(parse_address(unquote(entry))));
    }
    return result;
}

std::vector<std::string> trusted_proxies::parse_forwarded(std::string_view value) {
    std::vector<std::string> result;
    for (auto& element : parse_forwarded_elements(value)) result.push_back(std::move(element.address));
    return result;
}

} // namespace thinger::http

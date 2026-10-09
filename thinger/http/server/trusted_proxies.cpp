#include "trusted_proxies.hpp"
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
    if (trim(value).empty()) return result;
    for (auto element : split_unquoted(value, ',')) {
        // element: pairs such as for=192.0.2.60;proto=http;by=203.0.113.43
        std::optional<address> ip;
        for (auto pair : split_unquoted(element, ';')) {
            auto equals = pair.find('=');
            if (equals == std::string_view::npos) continue;
            if (boost::iequals(trim(pair.substr(0, equals)), "for")) {
                ip = parse_address(unquote(pair.substr(equals + 1)));
                break;
            }
        }
        result.push_back(to_entry(ip));
    }
    return result;
}

} // namespace thinger::http

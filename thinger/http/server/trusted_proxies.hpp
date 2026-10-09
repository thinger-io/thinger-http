#ifndef THINGER_HTTP_SERVER_TRUSTED_PROXIES_HPP
#define THINGER_HTTP_SERVER_TRUSTED_PROXIES_HPP

#include <boost/asio/ip/address.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace thinger::http {

// Header the trusted proxies use to report the client address. Only one is read: a proxy
// that appends to one of them passes the other one through, as sent by the client.
enum class forwarded_header {
    x_forwarded_for,    // X-Forwarded-For: client, proxy1, proxy2
    forwarded           // Forwarded: for=client, for=proxy1 (RFC 7239)
};

// Proxies (IPs and CIDR ranges, IPv4 and IPv6) allowed to report the address of the client
// they forward a request for
class trusted_proxies {
public:
    explicit trusted_proxies(forwarded_header header = forwarded_header::x_forwarded_for)
        : header_(header) {}

    // Trust an IP ("10.0.0.1", "::1") or a CIDR range ("10.0.0.0/8", "fd00::/8");
    // returns false if it is not valid
    bool add(std::string_view network);

    // Whether an address belongs to a trusted proxy (IPv4-mapped IPv6 addresses are
    // compared as IPv4); false for anything that is not an IP address
    bool contains(std::string_view ip) const;

    bool empty() const { return networks_.empty(); }

    forwarded_header header() const { return header_; }

    // Name of the header read ("X-Forwarded-For" or "Forwarded")
    const std::string& header_name() const;

    // Client address of a request received from `peer`, given the values of the forwarding
    // header (all its occurrences, in order). The peer itself unless it is trusted; then the
    // forwarded addresses are walked from right to left, skipping trusted proxies, and the
    // first untrusted one is returned (the leftmost if all of them are trusted). An entry
    // that is not an IP ("unknown") ends the walk at the address after it.
    std::string client_ip(const std::string& peer, const std::vector<std::string>& header_values) const;

    // Addresses in the forwarding headers, left to right, without ports nor brackets;
    // empty strings for entries that are not IP addresses
    static std::vector<std::string> parse_x_forwarded_for(std::string_view value);
    static std::vector<std::string> parse_forwarded(std::string_view value);

private:
    struct network {
        boost::asio::ip::address address;
        unsigned prefix;
    };

    static bool in_network(const boost::asio::ip::address& address, const network& net);

    std::vector<network> networks_;
    forwarded_header header_;
};

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_TRUSTED_PROXIES_HPP

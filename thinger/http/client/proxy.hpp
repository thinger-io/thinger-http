#ifndef THINGER_HTTP_CLIENT_PROXY_HPP
#define THINGER_HTTP_CLIENT_PROXY_HPP

#include <boost/asio/ip/address.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace thinger::http {

/**
 * Outbound proxy settings of a client or of a request (see http_client_base::proxy()).
 *
 * Plain http:// requests are sent to the proxy with an absolute-form target (RFC 9112,
 * section 3.2.2); https://, ws:// and wss:// ones go through a tunnel opened with CONNECT
 * (RFC 9110, section 9.3.6), with TLS end to end to the origin. Hosts in `no_proxy`, and
 * localhost, loopback addresses and Unix sockets always, are reached directly.
 */
struct proxy_config {
    /// Proxy for http:// and ws:// requests, and for https:// and wss:// ones:
    /// "http://[user:password@]host[:port]" (the scheme is optional, the port 80 by default,
    /// user and password percent-encoded). Empty to connect directly. Only HTTP proxies are
    /// supported: an https:// (TLS to the proxy) or socks URL fails the requests using it.
    std::string http;
    std::string https;

    /// Hosts reached directly, comma separated (case-insensitive, spaces trimmed):
    ///  - "example.com" or ".example.com": that domain and its subdomains ("*.example.com" too);
    ///  - an IP address ("10.1.2.3", "::1" or "[::1]") or a CIDR range ("10.0.0.0/8");
    ///  - any of them with a port ("example.com:8080", "[::1]:8080"): only for that port;
    ///  - "*": every host.
    std::string no_proxy;

    /// Read an environment variable: its value, or nothing if it is not set
    using env_reader = std::function<std::optional<std::string>(const char* name)>;

    /**
     * Settings from the environment, as curl reads them (the first variable set and not
     * empty wins):
     *  - http:  http_proxy (lowercase only: HTTP_PROXY is ignored, as in a CGI program it is
     *           the Proxy header of the request it serves: "httpoxy", CVE-2016-5385);
     *  - https: https_proxy, HTTPS_PROXY;
     *  - both, when not set above: all_proxy, ALL_PROXY;
     *  - no_proxy, NO_PROXY.
     */
    static proxy_config from_environment();
    static proxy_config from_environment(const env_reader& getenv);
};

/// An HTTP proxy, from its URL (see proxy_config)
struct proxy_endpoint {
    std::string host;           // lowercase, an IPv6 address without brackets
    std::string port;
    std::string authorization;  // Proxy-Authorization value ("Basic ..."), empty without credentials

    /// The proxy of a URL; nothing (and why in `error`) if the URL is invalid or not of an
    /// HTTP proxy
    static std::optional<proxy_endpoint> parse(std::string_view url, std::string* error = nullptr);

    /// "http://host:port", without the credentials (for logs and proxy_for())
    std::string url() const;

    /// Identifies the proxy and its credentials (connections through it are only reused by
    /// requests with the same one)
    std::string key() const;
};

/// Hosts reached without a proxy, from a NO_PROXY list (see proxy_config::no_proxy)
class no_proxy_list {
public:
    no_proxy_list() = default;
    explicit no_proxy_list(std::string_view list);

    /// Whether a request to host:port goes directly
    bool matches(std::string_view host, std::string_view port) const;

    bool empty() const { return !all_ && entries_.empty(); }

private:
    struct entry {
        std::string domain;                             // a host name, without leading dot
        std::optional<boost::asio::ip::address> ip;     // an address or the start of a range
        unsigned prefix = 0;                            // range bits (ip only)
        bool range = false;
        std::string port;                               // empty for any port
    };
    // An entry of the list (lowercase); nothing if it cannot be parsed
    static std::optional<entry> parse_entry(std::string_view token);

    bool all_ = false;
    std::vector<entry> entries_;
};

/// Whether a host is never proxied: localhost (and *.localhost) or a loopback address
bool is_loopback_host(std::string_view host);

/// Proxy settings parsed once, to choose the proxy of each request
class proxy_resolver {
public:
    /// What to do with a request
    struct choice {
        std::shared_ptr<const proxy_endpoint> proxy;    // null to connect directly
        std::string error;                              // the configured proxy cannot be used
    };

    proxy_resolver() = default;
    explicit proxy_resolver(const proxy_config& config);

    /// The proxy of a request to host:port, over TLS (https, wss) or not
    choice select(bool secure, std::string_view host, std::string_view port) const;

private:
    struct proxy {
        std::shared_ptr<const proxy_endpoint> endpoint;
        std::string error;
        bool configured = false;
    };

    static proxy parse(const std::string& url);

    proxy http_;
    proxy https_;
    no_proxy_list no_proxy_;
};

}

#endif

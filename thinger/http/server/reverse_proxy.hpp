#ifndef THINGER_HTTP_SERVER_REVERSE_PROXY_HPP
#define THINGER_HTTP_SERVER_REVERSE_PROXY_HPP

#include "../common/http_request.hpp"
#include "../common/http_response.hpp"
#include "../../util/types.hpp"
#include <boost/asio/ssl/context.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

namespace thinger::http {

class request;
class response;

// TLS settings to reach a target over HTTPS (or WSS)
struct proxy_tls_options {
    // Verify the certificate of the target: its chain, against the trusted CAs, and that it
    // was issued for `server_name`. Only disable it for testing.
    bool verify = true;

    // Name sent with SNI and verified in the certificate (a host name or an IP address);
    // the target host if empty
    std::string server_name;

    // CAs trusted to verify the target instead of the system ones: a PEM file, or PEM text.
    // A target with a certificate from a private CA is only accepted when its CA is set here.
    std::string ca_file;
    std::string ca_pem;
};

// Where a reverse proxy forwards a request: a TCP address, over TLS or not, or a Unix
// socket, with a base path prepended to the forwarded path
struct proxy_target {
    enum class transport { tcp, tls, unix_socket };

    transport type = transport::tcp;

    // TCP and TLS targets: host name or IP address (IPv6 without brackets), and port
    std::string host;
    uint16_t port = 80;

    // Unix socket targets
    std::string socket_path;

    // Prepended to the path forwarded (e.g. "/api"); empty for none
    std::string base_path;

    // TLS targets
    proxy_tls_options tls;

    // Target from a URL: "http://host[:port][/base]", "https://..." (TLS), or "ws://" and
    // "wss://" alike. Empty if the URL is not valid (or has a query or fragment).
    static std::optional<proxy_target> from_url(std::string_view url);

    // Unix socket target
    static proxy_target unix_socket(std::string path, std::string base_path = {});

    // Host header for the target: its host, with the port unless it is the default one
    // ("localhost" for Unix sockets)
    std::string authority() const;
};

// Forwarding headers added to the requests sent to the target
enum class forwarding_headers {
    x_forwarded,    // X-Forwarded-For, X-Forwarded-Proto and X-Forwarded-Host
    forwarded,      // Forwarded (RFC 7239): for=...;proto=...;host=...
    none            // none added
};

// How a request is forwarded
struct proxy_options {
    // Path sent to the target (after the target base path), e.g. "/" + req["path"], as it is
    // to be sent (percent-encoded). The path of the request, as received, if empty. The query
    // of the request is appended to a path without one.
    std::string path;

    // Host header sent: the target authority by default, `host` if set, or the Host of the
    // request if `preserve_host` (the original host is also in the forwarding headers)
    bool preserve_host = false;
    std::string host;

    // Headers reporting the client to the target. Those received from a trusted proxy (see
    // http_application::set_trusted_proxies, the header it sets) are kept and extended; from
    // any other peer, the forwarding headers are dropped, as the client could forge them.
    forwarding_headers forwarding = forwarding_headers::x_forwarded;

    // Time to connect to the target (TLS handshake included); 504 if exceeded
    std::chrono::milliseconds connect_timeout{10000};

    // Maximum time without any data exchanged once connected (waiting for the response
    // included): 504 if the response has not started, or the connection to the client is
    // closed if it has. Zero disables it (e.g. for event streams).
    std::chrono::milliseconds idle_timeout{60000};

    // Maximum time for the whole exchange, connection included; zero for no limit
    std::chrono::milliseconds total_timeout{0};

    // Maximum request body forwarded (413 if larger); zero for no limit. The maximum body
    // size of the route (route::max_body_size, or the application one) applies too, as to
    // any body read: the smaller one wins. The body is never held whole: it is streamed to
    // the target as it arrives.
    size_t max_body_size = 0;

    // Forward protocol upgrades (WebSocket...) as a tunnel between the client and the target
    bool allow_upgrade = true;

    // Change the request sent to the target before it is sent: its method, path (get_uri())
    // and headers, e.g. to add credentials or remove the Authorization of the client. The
    // body framing headers are set by the proxy afterwards.
    std::function<void(request& incoming, http_request& outgoing)> on_request;

    // Change the response of the target before it is sent to the client: its status and
    // headers (the framing headers are set by the proxy afterwards). Also called for the
    // response of a protocol upgrade (101).
    std::function<void(request& incoming, http_response& response)> on_response;
};

// What happened to a forwarded request
struct proxy_result {
    // Status sent to the client: the target's, or the error of the proxy (502, 504...);
    // 0 if nothing could be sent (e.g. the response was already sent)
    int status = 0;

    // Whether the response came from the target (then `status` is its status)
    bool from_target = false;

    // Whether the connection became a tunnel to the target (protocol upgrade)
    bool upgraded = false;

    // Whether the exchange was complete: the request body sent, and the response received
    // and sent whole to the client
    bool complete = false;

    // Body bytes forwarded: of the request to the target, and of the response to the client
    size_t request_bytes = 0;
    size_t response_bytes = 0;

    // Why the exchange failed, empty if it did not
    std::string error;
};

// Reverse proxy: forwards the request of a route handler to a target (TCP, TLS or Unix
// socket), and its response back, streaming both bodies with flow control, or turns the
// connection into a tunnel to the target on a protocol upgrade (WebSocket). Use it from a
// coroutine handler that has not read the body:
//
//     http::reverse_proxy proxy;
//     auto target = *http::proxy_target::from_url("http://127.0.0.1:3000");
//     server.host("app.example.com").set_not_found_handler(
//         [&](http::request& req, http::response& res) -> thinger::awaitable<void> {
//             co_await proxy.forward(req, res, target);
//         });
//
// It is thread-safe: one proxy can serve every route and thread. The request is forwarded
// without hop-by-hop headers and with forwarding headers; the body is framed again by the
// proxy, from what the server parser accepted. The response of the target is parsed
// strictly: a response that cannot be framed safely is answered with 502. The connection
// to the target is used for a single request.
class reverse_proxy {
public:
    reverse_proxy() = default;

    // Proxy with the options used by forward() calls that do not give their own
    explicit reverse_proxy(proxy_options defaults) : defaults_(std::move(defaults)) {}

    // Forward the request to `target` and send its response (errors through the error
    // formatter: 502 if the target cannot be reached or its response is invalid, 504 on a
    // timeout). Call it before reading the request body, with the response not sent yet.
    awaitable<proxy_result> forward(request& req, response& res, const proxy_target& target);
    awaitable<proxy_result> forward(request& req, response& res, const proxy_target& target,
                                    const proxy_options& options);

    // Options used by default (set them before forwarding)
    proxy_options& options() { return defaults_; }
    const proxy_options& options() const { return defaults_; }

    // TLS context for targets with the given settings (shared by the targets with the same
    // CAs and verification); throws if the CAs cannot be loaded
    std::shared_ptr<boost::asio::ssl::context> tls_context(const proxy_tls_options& tls);

private:
    proxy_options defaults_;

    std::mutex tls_mutex_;
    std::map<std::tuple<bool, std::string, std::string>, std::shared_ptr<boost::asio::ssl::context>> tls_contexts_;
};

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_REVERSE_PROXY_HPP

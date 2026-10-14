// The request forwarded by the reverse proxy: its path, headers and framing
#include "proxy_exchange.hpp"
#include "../trusted_proxies.hpp"
#include "../../common/syntax.hpp"
#include "../../../util/ascii.hpp"
#include <boost/asio/ip/address.hpp>
#include <algorithm>

namespace thinger::http::proxy_detail {

namespace {

    namespace ascii = ::thinger::util::ascii;

    // Hop-by-hop headers (RFC 9110, section 7.6.1), meaningful only for a single connection,
    // and the Proxy-* ones, addressed to proxies
    bool is_hop_by_hop(std::string_view name) {
        static constexpr std::string_view names[] = {
            "Connection", "Keep-Alive", "TE", "Trailer", "Transfer-Encoding", "Upgrade"
        };
        for (auto hop : names) {
            if (ascii::iequals(name, hop)) return true;
        }
        return ascii::istarts_with(name, "Proxy-");
    }

    bool is_forwarding_header(std::string_view name) {
        return ascii::iequals(name, "Forwarded") || ascii::iequals(name, "X-Forwarded-For") ||
               ascii::iequals(name, "X-Forwarded-Proto") || ascii::iequals(name, "X-Forwarded-Host");
    }

    // Values of all the `name` headers, as a single comma-separated list
    std::string joined(const headers& message, std::string_view name) {
        std::string list;
        for (const auto& value : message.get_headers_with_key(name)) {
            auto item = syntax::trim(value);
            if (item.empty()) continue;
            if (!list.empty()) list += ", ";
            list += item;
        }
        return list;
    }

    // Node of the Forwarded header (RFC 7239, section 6): IPv6 addresses in brackets and quoted
    std::string forwarded_node(const std::string& ip) {
        if (!is_ip_address(ip)) return "unknown";
        if (ip.find(':') != std::string::npos) return "\"[" + ip + "]\"";
        return ip;
    }

    // Parameter value of the Forwarded header: a token, or a quoted string
    std::string forwarded_value(std::string_view value) {
        if (syntax::is_token(value)) return std::string(value);
        std::string quoted = "\"";
        for (char c : value) {
            if (c == '"' || c == '\\') quoted += '\\';
            quoted += c;
        }
        return quoted + "\"";
    }

    std::string serialize_head(const http_request& outgoing) {
        std::string head = get_method(outgoing.get_method()) + " " + outgoing.get_uri() + " HTTP/1.1\r\n";
        for (const auto& [name, value] : outgoing.get_headers()) {
            head += name;
            head += ": ";
            head += value;
            head += "\r\n";
        }
        head += "\r\n";
        return head;
    }

}

bool is_request_target(std::string_view target) {
    return !target.empty() && target.front() == '/' &&
           std::all_of(target.begin(), target.end(), [](unsigned char c) {
               return c > 0x20 && c != 0x7f && c != '#';
           });
}

bool has_valid_headers(const headers& message) {
    return std::all_of(message.get_headers().begin(), message.get_headers().end(), [](const auto& header) {
        return syntax::is_token(header.first) && syntax::is_field_value(header.second);
    });
}

std::vector<std::string> connection_options(const headers& message) {
    std::vector<std::string> options;
    for (const auto& value : message.get_headers_with_key(header::connection)) {
        syntax::for_each_element(value, [&](std::string_view option) { options.emplace_back(option); });
    }
    return options;
}

bool is_listed(const std::vector<std::string>& options, std::string_view name) {
    return std::any_of(options.begin(), options.end(), [&](const std::string& option) {
        return ascii::iequals(option, name);
    });
}

bool is_end_to_end(std::string_view name, const std::vector<std::string>& connection) {
    return !is_hop_by_hop(name) && !is_listed(connection, name);
}

void remove_framing_headers(headers& message) {
    for (auto name : {header::content_length, header::transfer_encoding, header::connection,
                      std::string("Keep-Alive"), header::upgrade}) {
        message.remove_headers(name);
    }
}

void copy_end_to_end(const http_response& head, http_response& out) {
    auto connection = connection_options(head);
    for (const auto& [name, value] : head.get_headers()) {
        if (!is_end_to_end(name, connection) || headers::is_header(name, header::content_length)) continue;
        out.add_header(name, value);
    }
}

bool is_ip_address(const std::string& value) {
    boost::system::error_code ec;
    boost::asio::ip::make_address(value, ec);
    return !ec;
}

std::string proxy_exchange::target_path() const {
    const auto& uri = incoming_->get_uri();
    std::string path = options_.path;
    if (path.empty()) {
        path = uri;
    } else if (path.find('?') == std::string::npos) {
        // keep the query of the request
        if (auto query = uri.find('?'); query != std::string::npos) path += uri.substr(query);
    }
    std::string base = target_.base_path;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (!base.empty() && !path.empty() && path.front() != '/') base += '/';
    return base + path;
}

std::string proxy_exchange::target_host() const {
    if (options_.preserve_host) return incoming_->get_header(header::host);
    if (!options_.host.empty()) return options_.host;
    return target_.authority();
}

void proxy_exchange::add_forwarding_headers(http_request& outgoing) const {
    // The forwarding headers of the request are kept only if they come from a trusted
    // proxy, and only the one it sets: the others are as the client sent them
    const auto& proxies = req_.get_trusted_proxies();
    auto peer = req_.get_peer_ip();
    bool trusted_peer = proxies && !peer.empty() && proxies->contains(peer);
    bool keep_x_forwarded = trusted_peer && proxies->header() == forwarded_header::x_forwarded_for;
    bool keep_forwarded = trusted_peer && proxies->header() == forwarded_header::forwarded;

    for (const auto& [name, value] : incoming_->get_headers()) {
        if (!is_forwarding_header(name)) continue;
        bool forwarded = ascii::iequals(name, "Forwarded");
        // the ones this proxy sets are added below, once
        bool set_here = options_.forwarding == (forwarded ? forwarding_headers::forwarded : forwarding_headers::x_forwarded);
        if ((forwarded ? keep_forwarded : keep_x_forwarded) && !set_here) outgoing.add_header(name, value);
    }

    if (options_.forwarding == forwarding_headers::x_forwarded) {
        add_x_forwarded(outgoing, peer, keep_x_forwarded);
    } else if (options_.forwarding == forwarding_headers::forwarded) {
        add_forwarded(outgoing, peer, keep_forwarded);
    }
}

void proxy_exchange::add_x_forwarded(http_request& outgoing, const std::string& peer, bool keep_received) const {
    std::string node = is_ip_address(peer) ? peer : "unknown";
    auto chain = keep_received ? joined(*incoming_, "X-Forwarded-For") : std::string{};
    outgoing.add_header("X-Forwarded-For", chain.empty() ? node : chain + ", " + node);

    auto proto = keep_received ? incoming_->get_header("X-Forwarded-Proto") : std::string{};
    if (proto.empty()) proto = incoming_->is_ssl() ? "https" : "http";
    outgoing.add_header("X-Forwarded-Proto", proto);

    auto host = keep_received ? incoming_->get_header("X-Forwarded-Host") : std::string{};
    if (host.empty()) host = incoming_->get_header(header::host);
    if (!host.empty()) outgoing.add_header("X-Forwarded-Host", host);
}

void proxy_exchange::add_forwarded(http_request& outgoing, const std::string& peer, bool keep_received) const {
    auto chain = keep_received ? joined(*incoming_, "Forwarded") : std::string{};
    std::string element = "for=" + forwarded_node(peer) + ";proto=" + (incoming_->is_ssl() ? "https" : "http");
    const auto& host = incoming_->get_header(header::host);
    if (!host.empty()) element += ";host=" + forwarded_value(host);
    outgoing.add_header("Forwarded", chain.empty() ? element : chain + ", " + element);
}

bool proxy_exchange::check_method() {
    auto method = incoming_->get_method();
    if (method == method::UNKNOWN || method == method::CONNECT) {
        fail(http_response::status::not_implemented, "Method not supported", "method not supported");
        return false;
    }
    if (method == method::TRACE) {
        fail(http_response::status::not_allowed, "", "TRACE is not forwarded");
        return false;
    }
    return true;
}

bool proxy_exchange::check_upgrade(const std::vector<std::string>& connection) {
    // A protocol upgrade (Connection: upgrade, and Upgrade), except to HTTP/2 over cleartext,
    // which this proxy does not speak: the request is forwarded without it
    const auto& protocol = incoming_->get_header(header::upgrade);
    upgrade_requested_ = options_.allow_upgrade && is_listed(connection, "upgrade") && !protocol.empty() &&
                         !ascii::istarts_with(protocol, "h2c");
    if (upgrade_requested_ && !res_.get_connection()) {
        fail(http_response::status::not_implemented, "Protocol upgrades are not available for in-memory requests",
             "protocol upgrade without a connection");
        return false;
    }
    return true;
}

void proxy_exchange::copy_request_headers(http_request& outgoing, const std::vector<std::string>& connection) const {
    // Not Expect: a client waiting for 100 Continue gets it from the server once the body is
    // streamed to the target, the first time it is read, as nginx does
    for (const auto& [name, value] : incoming_->get_headers()) {
        if (!is_end_to_end(name, connection) || is_forwarding_header(name) ||
            headers::is_header(name, header::host) || headers::is_header(name, header::content_length) ||
            headers::is_header(name, "Expect")) {
            continue;
        }
        outgoing.add_header(name, value);
    }
}

bool proxy_exchange::check_outgoing(const http_request& outgoing) {
    auto method = outgoing.get_method();
    if (method == method::UNKNOWN || method == method::CONNECT) {
        fail(http_response::status::internal_server_error, "", "invalid method for the target");
        return false;
    }
    if (!is_request_target(outgoing.get_uri())) {
        fail(http_response::status::internal_server_error, "", "invalid path for the target: " + outgoing.get_uri());
        return false;
    }
    if (!has_valid_headers(outgoing)) {
        fail(http_response::status::internal_server_error, "", "invalid header for the target");
        return false;
    }
    return true;
}

bool proxy_exchange::frame_body(http_request& outgoing) {
    // The body as the server parser framed it: streamed with its Content-Length, or chunked
    // again by the proxy. Never the framing headers of the client, so the target cannot
    // read the body (and what follows) any other way.
    if (req_.has_pending_body() && req_.is_chunked()) {
        body_mode_ = body_mode::chunked;
        outgoing.add_header(header::transfer_encoding, "chunked");
    } else if (req_.has_pending_body()) {
        body_mode_ = body_mode::length;
        auto length = incoming_->get_content_length();
        if (options_.max_body_size && length > options_.max_body_size) {
            reject_large_body();
            return false;
        }
        outgoing.add_header(header::content_length, std::to_string(length));
    } else if (!incoming_->get_body().empty()) {
        // already read by the server (e.g. a route that is not deferred)
        body_mode_ = body_mode::length;
        body_buffered_ = true;
        outgoing.add_header(header::content_length, std::to_string(incoming_->get_body().size()));
    } else if (incoming_->has_content_length_header() || outgoing_method_ == method::POST ||
               outgoing_method_ == method::PUT || outgoing_method_ == method::PATCH) {
        outgoing.add_header(header::content_length, "0");
    }
    return true;
}

void proxy_exchange::add_connection_headers(http_request& outgoing) const {
    if (upgrade_requested_) {
        outgoing.add_header(header::connection, "Upgrade");
        outgoing.add_header(header::upgrade, incoming_->get_header(header::upgrade));
    } else {
        // a connection for this request only (see reverse_proxy)
        outgoing.add_header(header::connection, "close");
    }
}

bool proxy_exchange::prepare() {
    if (res_.has_responded()) {
        result_.error = "response already sent";
        LOG_ERROR("reverse proxy {}: the response was already sent", describe());
        return false;
    }
    if (!check_method()) return false;
    auto connection = connection_options(*incoming_);
    if (!check_upgrade(connection)) return false;

    // Host first, then the end-to-end headers of the request
    auto outgoing = std::make_shared<http_request>();
    outgoing->set_method(incoming_->get_method());
    outgoing->set_uri(target_path());
    if (auto host = target_host(); !host.empty()) outgoing->add_header(header::host, host);
    copy_request_headers(*outgoing, connection);
    add_forwarding_headers(*outgoing);

    // Framing and connection headers are set here, whatever the hook did
    if (options_.on_request) options_.on_request(req_, *outgoing);
    remove_framing_headers(*outgoing);
    outgoing_method_ = outgoing->get_method();
    if (!check_outgoing(*outgoing) || !frame_body(*outgoing)) return false;
    add_connection_headers(*outgoing);

    request_head_ = serialize_head(*outgoing);
    return true;
}

}

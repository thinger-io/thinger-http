#include "reverse_proxy.hpp"
#include "request.hpp"
#include "response.hpp"
#include "body_reader.hpp"
#include "trusted_proxies.hpp"
#include "../client/response_factory.hpp"
#include "../../asio/socket_pipe.hpp"
#include "../../asio/sockets/ssl_socket.hpp"
#include "../../asio/sockets/tcp_socket.hpp"
#include "../../asio/sockets/unix_socket.hpp"
#include "../../util/ascii.hpp"
#include "../../util/logger.hpp"
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>

namespace thinger::http {

namespace {

    using clock = std::chrono::steady_clock;
    namespace ascii = ::thinger::util::ascii;

    // Bytes read or written at once on each side
    constexpr size_t buffer_size = 16 * 1024;

    // Interim responses (1xx) skipped before giving up on a target
    constexpr unsigned max_interim_responses = 16;

    // Longest status line accepted from a target
    constexpr size_t max_status_line = 8192;

    // tchar (RFC 9110, section 5.6.2)
    bool is_token_char(unsigned char c) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
        switch (c) {
            case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
            case '-': case '.': case '^': case '_': case '`': case '|': case '~':
                return true;
            default:
                return false;
        }
    }

    bool is_token(std::string_view value) {
        return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return is_token_char(c); });
    }

    // field-value: visible characters, spaces and tabs (and obs-text), never CR, LF or NUL
    bool is_field_value(std::string_view value) {
        return std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return c == '\t' || (c >= 0x20 && c != 0x7f);
        });
    }

    // Path and query sent in the request line: an absolute path, without whitespace,
    // control characters or a fragment
    bool is_request_target(std::string_view target) {
        return !target.empty() && target.front() == '/' &&
               std::all_of(target.begin(), target.end(), [](unsigned char c) {
                   return c > 0x20 && c != 0x7f && c != '#';
               });
    }

    // Headers that can be sent as they are: valid names and values
    bool has_valid_headers(const headers& message) {
        return std::all_of(message.get_headers().begin(), message.get_headers().end(), [](const auto& header) {
            return is_token(header.first) && is_field_value(header.second);
        });
    }

    std::string_view trim(std::string_view value) {
        constexpr std::string_view whitespace = " \t";
        auto first = value.find_first_not_of(whitespace);
        if (first == std::string_view::npos) return {};
        return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
    }

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

    // Names listed by the Connection headers: hop-by-hop headers of this message
    std::vector<std::string> connection_options(const headers& message) {
        std::vector<std::string> options;
        for (const auto& value : message.get_headers_with_key(header::connection)) {
            std::string_view list = value;
            while (!list.empty()) {
                auto comma = list.find(',');
                auto option = trim(list.substr(0, comma));
                if (!option.empty()) options.emplace_back(option);
                if (comma == std::string_view::npos) break;
                list.remove_prefix(comma + 1);
            }
        }
        return options;
    }

    bool is_listed(const std::vector<std::string>& options, std::string_view name) {
        return std::any_of(options.begin(), options.end(), [&](const std::string& option) {
            return ascii::iequals(option, name);
        });
    }

    // Whether a header goes from one hop to the next: not hop-by-hop, nor listed in Connection
    bool is_end_to_end(std::string_view name, const std::vector<std::string>& connection) {
        return !is_hop_by_hop(name) && !is_listed(connection, name);
    }

    bool is_forwarding_header(std::string_view name) {
        return ascii::iequals(name, "Forwarded") || ascii::iequals(name, "X-Forwarded-For") ||
               ascii::iequals(name, "X-Forwarded-Proto") || ascii::iequals(name, "X-Forwarded-Host");
    }

    // Values of all the `name` headers, as a single comma-separated list
    std::string joined(const headers& message, std::string_view name) {
        std::string list;
        for (const auto& value : message.get_headers_with_key(name)) {
            auto item = trim(value);
            if (item.empty()) continue;
            if (!list.empty()) list += ", ";
            list += item;
        }
        return list;
    }

    bool is_ip_address(const std::string& value) {
        boost::system::error_code ec;
        boost::asio::ip::make_address(value, ec);
        return !ec;
    }

    // Node of the Forwarded header (RFC 7239, section 6): IPv6 addresses in brackets and quoted
    std::string forwarded_node(const std::string& ip) {
        if (!is_ip_address(ip)) return "unknown";
        if (ip.find(':') != std::string::npos) return "\"[" + ip + "]\"";
        return ip;
    }

    // Parameter value of the Forwarded header: a token, or a quoted string
    std::string forwarded_value(std::string_view value) {
        if (is_token(value)) return std::string(value);
        std::string quoted = "\"";
        for (char c : value) {
            if (c == '"' || c == '\\') quoted += '\\';
            quoted += c;
        }
        return quoted + "\"";
    }

    // Status line of a target: "HTTP/1.x DDD [reason]\r\n"; its status code, or 0 if it is
    // not valid. The response parser reads it too, but would truncate longer status codes.
    int parse_status_line(std::string_view line) {
        if (line.size() < 15 || !line.starts_with("HTTP/1.") || (line[7] != '0' && line[7] != '1') ||
            line[8] != ' ' || !line.ends_with("\r\n")) {
            return 0;
        }
        int code = 0;
        for (size_t i = 9; i < 12; ++i) {
            if (line[i] < '0' || line[i] > '9') return 0;
            code = code * 10 + (line[i] - '0');
        }
        if (line[12] != ' ' || code < 100 || code > 599) return 0;
        auto reason = line.substr(13, line.size() - 15);
        return is_field_value(reason) ? code : 0;
    }

    // Whether a read from the target ended at the end of its stream (not an error). A TLS
    // target closing without close_notify (stream_truncated) does not end a body delimited
    // by the connection: it may have been cut, so it is relayed as incomplete
    bool is_end_of_stream(const boost::system::error_code& ec) {
        return ec == boost::asio::error::eof;
    }

    // The io_context of an executor (of one of its executor or a strand of it), or null
    boost::asio::io_context* io_context_of(const boost::asio::any_io_executor& executor) {
        using untracked = boost::asio::io_context::basic_executor_type<std::allocator<void>, 0>;
        using tracked = boost::asio::io_context::basic_executor_type<std::allocator<void>, 4>;
        if (auto* inner = executor.target<untracked>()) return &inner->context();
        if (auto* inner = executor.target<tracked>()) return &inner->context();
        if (auto* strand = executor.target<boost::asio::strand<boost::asio::any_io_executor>>()) {
            return io_context_of(strand->get_inner_executor());
        }
        if (auto* strand = executor.target<boost::asio::strand<untracked>>()) {
            return &strand->get_inner_executor().context();
        }
        return nullptr;
    }

    // Tunnel between a client and a target after a protocol upgrade
    awaitable<void> run_tunnel(std::shared_ptr<asio::socket> client, std::shared_ptr<asio::socket> target,
                               std::string to_client, std::string to_target) {
        // Bytes each side sent right after the handshake go first
        bool ok = true;
        if (!to_client.empty()) {
            auto [ec, bytes] = co_await client->write(to_client);
            ok = !ec;
        }
        if (ok && !to_target.empty()) {
            auto [ec, bytes] = co_await target->write(to_target);
            ok = !ec;
        }
        if (!ok) {
            client->close();
            target->close();
            co_return;
        }
        auto pipe = std::make_shared<asio::socket_pipe>(client, target);
        co_await pipe->run();
    }

    // A request forwarded to a target, and its response
    class proxy_exchange {
    public:
        proxy_exchange(reverse_proxy& proxy, request& req, response& res, const proxy_target& target,
                       const proxy_options& options)
            : proxy_(proxy), req_(req), res_(res), target_(target), options_(options),
              incoming_(req.get_http_request()) {}

        awaitable<proxy_result> run();

    private:
        enum class body_mode { none, length, chunked };

        // Build the request for the target; false if it cannot be forwarded (answered)
        bool prepare();
        std::string target_path() const;
        void add_forwarding_headers(http_request& outgoing) const;

        // Connect to the target within the connect timeout; false if it failed (answered)
        awaitable<bool> connect();
        awaitable<boost::system::error_code> connect_target();

        // Send the request, read the response head, then relay the response or tunnel
        awaitable<void> exchange();
        awaitable<bool> send_head();
        awaitable<void> stream_body();
        awaitable<bool> read_head();
        void request_done();
        awaitable<void> relay_response();
        void upgrade();

        // Relay the response body, as framed by the target
        awaitable<bool> relay_length(size_t length);
        awaitable<bool> relay_chunked();
        awaitable<bool> relay_until_close();

        // End the exchange once the idle or total timeout passes, or once the client closed
        // its connection (after sending the whole request)
        awaitable<void> watchdog();
        awaitable<void> watch_client();
        awaitable<void> wait_forever();

        // I/O with the target and the client, tracking activity for the idle timeout
        awaitable<io_result> read_target(uint8_t* buffer, size_t size);
        awaitable<bool> write_target(const std::vector<boost::asio::const_buffer>& buffers);
        awaitable<bool> send_to_client(std::string data);
        void touch() { last_activity_ = clock::now(); }

        // Answer an error to the client, or abort the response if it already started
        void fail(http_response::status status, const std::string& message, const std::string& detail);

        void close_target();
        boost::asio::io_context* io_context();
        std::string describe() const;

        reverse_proxy& proxy_;
        request& req_;
        response& res_;
        const proxy_target& target_;
        const proxy_options& options_;
        std::shared_ptr<http_request> incoming_;
        boost::asio::any_io_executor executor_;

        // Why the exchange was stopped before it ended: its failures are a consequence
        enum class stop_reason { none, timeout, client_left, request_failed };

        // Request for the target
        std::string request_head_;
        method outgoing_method_ = method::UNKNOWN;
        body_mode body_mode_ = body_mode::none;
        bool body_buffered_ = false;
        bool upgrade_requested_ = false;
        bool request_sent_ = false;

        std::shared_ptr<asio::socket> target_socket_;
        bool tls_config_error_ = false;

        // Bytes read from the target and not consumed yet
        std::string pending_;

        // Response head of the target
        std::shared_ptr<http_response> head_;
        int status_code_ = 0;

        bool response_started_ = false;
        // The whole response is queued: the connection may close after it (not the client leaving)
        bool response_ended_ = false;
        stop_reason stopped_ = stop_reason::none;
        std::optional<boost::asio::steady_timer> request_sent_signal_;
        clock::time_point start_;
        clock::time_point last_activity_;

        proxy_result result_;
    };

    boost::asio::io_context* proxy_exchange::io_context() {
        if (auto socket = req_.get_socket()) return &socket->get_io_context();
        // a request dispatched in memory: the context running the handler, if it is an
        // io_context (null otherwise: the sockets need one)
        return io_context_of(executor_);
    }

    std::string proxy_exchange::describe() const {
        std::string where;
        switch (target_.type) {
            case proxy_target::transport::unix_socket:
                where = "unix:" + target_.socket_path;
                break;
            case proxy_target::transport::tls:
                where = "https://" + target_.authority();
                break;
            default:
                where = "http://" + target_.authority();
                break;
        }
        return get_method(incoming_->get_method()) + " " + incoming_->get_uri() + " -> " + where;
    }

    void proxy_exchange::fail(http_response::status status, const std::string& message, const std::string& detail) {
        // Once stopped (timeout, client gone, invalid request body), what the exchange waits
        // for fails as a consequence: that is answered by whoever stopped it
        if (stopped_ != stop_reason::none) return;
        LOG_WARNING("reverse proxy {}: {}", describe(), detail);
        if (result_.error.empty()) result_.error = detail;
        result_.complete = false;
        if (response_started_) {
            res_.abort_stream();
            return;
        }
        if (res_.has_responded()) return;
        res_.error(status, message);
        result_.status = static_cast<int>(status);
        result_.from_target = false;
    }

    void proxy_exchange::close_target() {
        if (target_socket_) target_socket_->close();
    }

    // --- Request ---

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

    void proxy_exchange::add_forwarding_headers(http_request& outgoing) const {
        // The forwarding headers of the request are kept only if they come from a trusted
        // proxy, and only the one it sets: the others are as the client sent them
        const auto& proxies = req_.get_trusted_proxies();
        auto peer = req_.get_peer_ip();
        bool trusted_peer = proxies && !peer.empty() && proxies->contains(peer);
        bool keep_x_forwarded = trusted_peer && proxies->header() == forwarded_header::x_forwarded_for;
        bool keep_forwarded = trusted_peer && proxies->header() == forwarded_header::forwarded;

        std::string proto = incoming_->is_ssl() ? "https" : "http";
        const auto& host = incoming_->get_header(header::host);
        std::string node = is_ip_address(peer) ? peer : "unknown";

        auto keep = [&](std::string_view name) {
            if (ascii::iequals(name, "Forwarded")) return keep_forwarded;
            return keep_x_forwarded;
        };
        for (const auto& [name, value] : incoming_->get_headers()) {
            if (!is_forwarding_header(name) || !keep(name)) continue;
            // the ones this proxy sets are added below, once
            if (options_.forwarding == forwarding_headers::x_forwarded && !ascii::iequals(name, "Forwarded")) continue;
            if (options_.forwarding == forwarding_headers::forwarded && ascii::iequals(name, "Forwarded")) continue;
            outgoing.add_header(name, value);
        }

        if (options_.forwarding == forwarding_headers::x_forwarded) {
            auto chain = keep_x_forwarded ? joined(*incoming_, "X-Forwarded-For") : std::string{};
            outgoing.add_header("X-Forwarded-For", chain.empty() ? node : chain + ", " + node);
            auto original_proto = keep_x_forwarded ? incoming_->get_header("X-Forwarded-Proto") : std::string{};
            outgoing.add_header("X-Forwarded-Proto", original_proto.empty() ? proto : original_proto);
            auto original_host = keep_x_forwarded ? incoming_->get_header("X-Forwarded-Host") : std::string{};
            if (original_host.empty()) original_host = host;
            if (!original_host.empty()) outgoing.add_header("X-Forwarded-Host", original_host);
        } else if (options_.forwarding == forwarding_headers::forwarded) {
            auto chain = keep_forwarded ? joined(*incoming_, "Forwarded") : std::string{};
            std::string element = "for=" + forwarded_node(peer) + ";proto=" + proto;
            if (!host.empty()) element += ";host=" + forwarded_value(host);
            outgoing.add_header("Forwarded", chain.empty() ? element : chain + ", " + element);
        }
    }

    bool proxy_exchange::prepare() {
        if (res_.has_responded()) {
            result_.error = "response already sent";
            LOG_ERROR("reverse proxy {}: the response was already sent", describe());
            return false;
        }

        auto method = incoming_->get_method();
        if (method == method::UNKNOWN || method == method::CONNECT) {
            fail(http_response::status::not_implemented, "Method not supported", "method not supported");
            return false;
        }
        if (method == method::TRACE) {
            fail(http_response::status::not_allowed, "", "TRACE is not forwarded");
            return false;
        }

        // A protocol upgrade (Connection: upgrade, and Upgrade), except to HTTP/2 over
        // cleartext, which this proxy does not speak (the request is forwarded without it)
        const auto& upgrade_protocol = incoming_->get_header(header::upgrade);
        auto connection = connection_options(*incoming_);
        upgrade_requested_ = options_.allow_upgrade && is_listed(connection, "upgrade") &&
                             !upgrade_protocol.empty() && !ascii::istarts_with(upgrade_protocol, "h2c");
        if (upgrade_requested_ && !res_.get_connection()) {
            fail(http_response::status::not_implemented, "Protocol upgrades are not available for in-memory requests",
                 "protocol upgrade without a connection");
            return false;
        }

        auto outgoing = std::make_shared<http_request>();
        outgoing->set_method(method);
        outgoing->set_uri(target_path());

        // Host first, then the end-to-end headers of the request
        std::string host;
        if (options_.preserve_host) {
            host = incoming_->get_header(header::host);
        } else if (!options_.host.empty()) {
            host = options_.host;
        } else {
            host = target_.authority();
        }
        if (!host.empty()) outgoing->add_header(header::host, host);

        for (const auto& [name, value] : incoming_->get_headers()) {
            if (!is_end_to_end(name, connection) || is_forwarding_header(name) ||
                headers::is_header(name, header::host) || headers::is_header(name, header::content_length) ||
                headers::is_header(name, "Expect")) {
                continue;
            }
            outgoing->add_header(name, value);
        }
        add_forwarding_headers(*outgoing);

        if (options_.on_request) options_.on_request(req_, *outgoing);

        // Framing and connection headers are set here, whatever the hook did
        for (auto name : {header::content_length, header::transfer_encoding, header::connection,
                          std::string("Keep-Alive"), header::upgrade}) {
            outgoing->remove_headers(name);
        }

        method = outgoing->get_method();
        outgoing_method_ = method;
        if (method == method::UNKNOWN || method == method::CONNECT) {
            fail(http_response::status::internal_server_error, "", "invalid method for the target");
            return false;
        }
        if (!is_request_target(outgoing->get_uri())) {
            fail(http_response::status::internal_server_error, "", "invalid path for the target: " + outgoing->get_uri());
            return false;
        }
        if (!has_valid_headers(*outgoing)) {
            fail(http_response::status::internal_server_error, "", "invalid header for the target");
            return false;
        }

        // The body as the server parser framed it: streamed with its Content-Length, or chunked
        // again by the proxy. Never the framing headers of the client, so the target cannot
        // read the body (and what follows) any other way.
        if (req_.has_pending_body()) {
            if (req_.is_chunked()) {
                body_mode_ = body_mode::chunked;
                outgoing->add_header(header::transfer_encoding, "chunked");
            } else {
                body_mode_ = body_mode::length;
                auto length = incoming_->get_content_length();
                if (options_.max_body_size && length > options_.max_body_size) {
                    if (auto stream = req_.get_http_stream()) stream->set_keep_alive(false);
                    incoming_->set_keep_alive(false);
                    fail(http_response::status::payload_too_large, "Payload Too Large", "request body too large");
                    return false;
                }
                outgoing->add_header(header::content_length, std::to_string(length));
            }
        } else if (!incoming_->get_body().empty()) {
            // already read by the server (e.g. a route that is not deferred)
            body_mode_ = body_mode::length;
            body_buffered_ = true;
            outgoing->add_header(header::content_length, std::to_string(incoming_->get_body().size()));
        } else if (incoming_->has_content_length_header() || method == method::POST ||
                   method == method::PUT || method == method::PATCH) {
            outgoing->add_header(header::content_length, "0");
        }

        if (upgrade_requested_) {
            outgoing->add_header(header::connection, "Upgrade");
            outgoing->add_header(header::upgrade, upgrade_protocol);
        } else {
            // a connection for this request only (see reverse_proxy)
            outgoing->add_header(header::connection, "close");
        }

        request_head_ = get_method(method) + " " + outgoing->get_uri() + " HTTP/1.1\r\n";
        for (const auto& [name, value] : outgoing->get_headers()) {
            request_head_ += name;
            request_head_ += ": ";
            request_head_ += value;
            request_head_ += "\r\n";
        }
        request_head_ += "\r\n";
        return true;
    }

    // --- Connection ---

    awaitable<boost::system::error_code> proxy_exchange::connect_target() {
        auto& io = *io_context();
        // The connect timeout is enforced by connect(): this one only bounds the socket
        auto limit = std::chrono::ceil<std::chrono::seconds>(options_.connect_timeout) + std::chrono::seconds(1);

        if (target_.type == proxy_target::transport::unix_socket) {
            auto socket = std::make_shared<asio::unix_socket>("reverse_proxy", io);
            target_socket_ = socket;
            co_return co_await socket->connect(target_.socket_path, limit);
        }

        auto tcp = std::make_shared<asio::tcp_socket>("reverse_proxy", io);
        target_socket_ = tcp;
        auto ec = co_await tcp->connect(target_.host, std::to_string(target_.port), limit);
        if (ec) co_return ec;
        tcp->enable_tcp_no_delay();
        if (target_.type != proxy_target::transport::tls) co_return ec;

        std::shared_ptr<boost::asio::ssl::context> context;
        try {
            context = proxy_.tls_context(target_.tls);
        } catch (const std::exception& e) {
            LOG_ERROR("reverse proxy: invalid TLS settings for {}: {}", target_.authority(), e.what());
            tls_config_error_ = true;
            co_return boost::asio::error::invalid_argument;
        }

        auto tls = std::make_shared<asio::ssl_socket>("reverse_proxy", tcp, context);
        target_socket_ = tls;
        const auto& name = target_.tls.server_name.empty() ? target_.host : target_.tls.server_name;
        if (target_.tls.verify) {
            // the certificate must be issued for the name (or address) of the target
            auto* ssl = tls->native_handle();
            int set = is_ip_address(name) ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), name.c_str())
                                          : SSL_set1_host(ssl, name.c_str());
            if (set != 1) {
                LOG_ERROR("reverse proxy: cannot verify the name {} of {}", name, target_.authority());
                tls_config_error_ = true;
                co_return boost::asio::error::invalid_argument;
            }
        }
        co_return co_await tls->handshake(name);
    }

    awaitable<bool> proxy_exchange::connect() {
        auto limit = options_.connect_timeout;
        if (options_.total_timeout.count() > 0) limit = std::min(limit, options_.total_timeout);

        // On timeout the socket is closed, so the connection or handshake in progress fails
        boost::asio::steady_timer timer(executor_, limit);
        auto timeout = [&]() -> awaitable<void> {
            auto [ec] = co_await timer.async_wait(use_nothrow_awaitable);
            if (!ec) close_target();
        };
        auto result = co_await (connect_target() || timeout());

        boost::system::error_code ec = boost::asio::error::timed_out;
        if (result.index() == 0) ec = std::get<0>(result);
        if (!ec) {
            touch();
            co_return true;
        }

        close_target();
        target_socket_.reset();
        if (ec == boost::asio::error::timed_out) {
            fail(http_response::status::gateway_timeout, "The target did not answer in time",
                 "timeout connecting to the target");
        } else if (tls_config_error_) {
            fail(http_response::status::bad_gateway, "Cannot connect to the target", "invalid TLS settings");
        } else {
            fail(http_response::status::bad_gateway, "Cannot connect to the target",
                 "cannot connect to the target: " + ec.message());
        }
        co_return false;
    }

    // --- I/O ---

    awaitable<io_result> proxy_exchange::read_target(uint8_t* buffer, size_t size) {
        auto result = co_await target_socket_->read_some(buffer, size);
        if (!std::get<0>(result)) touch();
        co_return result;
    }

    awaitable<bool> proxy_exchange::write_target(const std::vector<boost::asio::const_buffer>& buffers) {
        auto [ec, bytes] = co_await target_socket_->write(buffers);
        if (ec) co_return false;
        touch();
        co_return true;
    }

    awaitable<bool> proxy_exchange::send_to_client(std::string data) {
        auto size = data.size();
        if (!co_await res_.write_stream(std::move(data))) co_return false;
        result_.response_bytes += size;
        touch();
        co_return true;
    }

    // --- Exchange ---

    void proxy_exchange::request_done() {
        request_sent_ = true;
        if (request_sent_signal_) request_sent_signal_->cancel();
    }

    awaitable<bool> proxy_exchange::send_head() {
        std::vector<boost::asio::const_buffer> buffers{boost::asio::buffer(request_head_)};
        if (body_buffered_) buffers.push_back(boost::asio::buffer(incoming_->get_body()));
        bool written = co_await write_target(buffers);
        if (body_buffered_) result_.request_bytes = incoming_->get_body().size();
        co_return written;
    }

    awaitable<void> proxy_exchange::stream_body() {
        // The body goes at the pace of the target: each part is read from the client once
        // the target took the previous one
        std::vector<uint8_t> buffer(buffer_size);
        std::vector<boost::asio::const_buffer> buffers;
        static const std::string crlf = "\r\n";
        size_t sent = 0;
        bool target_open = true;
        while (target_open) {
            size_t bytes = co_await req_.read_some(buffer.data(), buffer.size());
            if (bytes == 0) break;
            touch();
            sent += bytes;
            result_.request_bytes = sent;
            if (options_.max_body_size && sent > options_.max_body_size) {
                if (auto stream = req_.get_http_stream()) stream->set_keep_alive(false);
                incoming_->set_keep_alive(false);
                fail(http_response::status::payload_too_large, "Payload Too Large", "request body too large");
                stopped_ = stop_reason::request_failed;
                close_target();
                co_return;
            }
            buffers.clear();
            std::string size_line;
            if (body_mode_ == body_mode::chunked) {
                char hex[20];
                auto [end, ec] = std::to_chars(hex, hex + sizeof(hex), bytes, 16);
                size_line.assign(hex, end);
                size_line += crlf;
                buffers.push_back(boost::asio::buffer(size_line));
            }
            buffers.push_back(boost::asio::buffer(buffer.data(), bytes));
            if (body_mode_ == body_mode::chunked) buffers.push_back(boost::asio::buffer(crlf));
            target_open = co_await write_target(buffers);
        }

        // A body the client did not send whole (or malformed, too large): the server answers
        // it and closes the connection; the target never gets its end
        if (req_.get_body_error() != body_error::none) {
            result_.error = "invalid or incomplete request body";
            LOG_WARNING("reverse proxy {}: {}", describe(), result_.error);
            stopped_ = stop_reason::request_failed;
            close_target();
            co_return;
        }

        if (target_open && body_mode_ == body_mode::chunked) {
            static const std::string last_chunk = "0\r\n\r\n";
            target_open = co_await write_target({boost::asio::buffer(last_chunk)});
        }
        // If the target stopped reading it (it may have answered early), what is left of
        // the body is not read: the server discards it, or closes the connection
        if (target_open && !req_.has_pending_body()) request_done();

        // The response ends the exchange, not the body
        co_await wait_forever();
    }

    awaitable<bool> proxy_exchange::read_head() {
        std::vector<uint8_t> buffer(buffer_size);
        for (unsigned interim = 0; ; ++interim) {
            response_factory parser;
            std::string status_line;
            bool line_complete = false;

            while (true) {
                if (pending_.empty()) {
                    auto [ec, bytes] = co_await read_target(buffer.data(), buffer.size());
                    if (ec || bytes == 0) {
                        fail(http_response::status::bad_gateway, "Invalid response from the target",
                             "the target closed the connection without a response" +
                             (ec ? ": " + ec.message() : std::string{}));
                        co_return false;
                    }
                    pending_.assign(reinterpret_cast<const char*>(buffer.data()), bytes);
                }

                // the status line, to check it as it is
                if (!line_complete) {
                    auto newline = pending_.find('\n');
                    status_line.append(pending_, 0, newline == std::string::npos ? pending_.size() : newline + 1);
                    line_complete = newline != std::string::npos;
                    if (status_line.size() > max_status_line) {
                        fail(http_response::status::bad_gateway, "Invalid response from the target",
                             "status line too long");
                        co_return false;
                    }
                }

                const char* begin = pending_.data();
                const char* end = begin + pending_.size();
                boost::tribool parsed = parser.parse_some(begin, end, true);
                pending_.erase(0, static_cast<size_t>(begin - pending_.data()));
                if (parsed) break;
                if (!parsed) {
                    fail(http_response::status::bad_gateway, "Invalid response from the target",
                         "invalid response head");
                    co_return false;
                }
            }

            auto head = parser.consume_response();
            int code = parse_status_line(status_line);
            if (!head || code == 0) {
                fail(http_response::status::bad_gateway, "Invalid response from the target", "invalid status line");
                co_return false;
            }

            // Interim responses (100 Continue, 103 Early Hints...) are not relayed
            if (code < 200 && code != 101) {
                if (interim + 1 >= max_interim_responses) {
                    fail(http_response::status::bad_gateway, "Invalid response from the target",
                         "too many interim responses");
                    co_return false;
                }
                continue;
            }

            head_ = std::move(head);
            status_code_ = code;
            co_return true;
        }
    }

    awaitable<bool> proxy_exchange::relay_length(size_t length) {
        while (length > 0) {
            std::string data;
            if (!pending_.empty()) {
                auto size = std::min(length, pending_.size());
                data = pending_.substr(0, size);
                pending_.erase(0, size);
            } else {
                data.resize(std::min(length, buffer_size));
                auto [ec, bytes] = co_await read_target(reinterpret_cast<uint8_t*>(data.data()), data.size());
                if (ec || bytes == 0) {
                    fail(http_response::status::bad_gateway, "", "response body truncated by the target");
                    co_return false;
                }
                data.resize(bytes);
            }
            length -= data.size();
            if (!co_await send_to_client(std::move(data))) {
                result_.error = "client connection lost";
                co_return false;
            }
        }
        co_return true;
    }

    awaitable<bool> proxy_exchange::relay_chunked() {
        // Decoded strictly (the decoder of the request bodies), and chunked again (or not)
        // for the client: chunk extensions and trailers are dropped
        chunked_decoder decoder;
        std::vector<uint8_t> buffer(buffer_size);
        while (!decoder.done()) {
            if (pending_.empty()) {
                auto [ec, bytes] = co_await read_target(buffer.data(), buffer.size());
                if (ec || bytes == 0) {
                    fail(http_response::status::bad_gateway, "", "chunked response body truncated by the target");
                    co_return false;
                }
                pending_.assign(reinterpret_cast<const char*>(buffer.data()), bytes);
            }
            std::string data(buffer_size, '\0');
            size_t produced = 0;
            auto consumed = decoder.decode(reinterpret_cast<const uint8_t*>(pending_.data()), pending_.size(),
                                           reinterpret_cast<uint8_t*>(data.data()), data.size(), produced);
            pending_.erase(0, consumed);
            if (decoder.failed()) {
                fail(http_response::status::bad_gateway, "", "malformed chunked response body");
                co_return false;
            }
            if (produced == 0) continue;
            data.resize(produced);
            if (!co_await send_to_client(std::move(data))) {
                result_.error = "client connection lost";
                co_return false;
            }
        }
        co_return true;
    }

    awaitable<bool> proxy_exchange::relay_until_close() {
        if (!pending_.empty() && !co_await send_to_client(std::exchange(pending_, {}))) {
            result_.error = "client connection lost";
            co_return false;
        }
        while (true) {
            std::string data(buffer_size, '\0');
            auto [ec, bytes] = co_await read_target(reinterpret_cast<uint8_t*>(data.data()), data.size());
            if (is_end_of_stream(ec)) co_return true;
            if (ec) {
                fail(http_response::status::bad_gateway, "", "response body interrupted: " + ec.message());
                co_return false;
            }
            data.resize(bytes);
            if (!co_await send_to_client(std::move(data))) {
                result_.error = "client connection lost";
                co_return false;
            }
        }
    }

    awaitable<void> proxy_exchange::relay_response() {
        const auto& head = *head_;
        int code = status_code_;
        // Whether the target sends a body depends on the request it got (the head for the
        // client, on the request of the client: see response::start_stream())
        bool head_request = outgoing_method_ == method::HEAD;
        bool no_body = head_request || code < 200 || code == 204 || code == 304;

        // The framing of the target must be unambiguous: a valid Content-Length, or
        // "Transfer-Encoding: chunked" alone, or neither (the body ends with the connection)
        auto transfer_encoding = head.get_headers_with_key(header::transfer_encoding);
        bool chunked = false;
        if (!transfer_encoding.empty()) {
            if (transfer_encoding.size() != 1 || !ascii::iequals(trim(transfer_encoding.front()), "chunked") ||
                head.has_content_length_header()) {
                fail(http_response::status::bad_gateway, "Invalid response from the target",
                     "ambiguous response framing (Transfer-Encoding)");
                co_return;
            }
            chunked = true;
        }
        if (head.has_invalid_content_length()) {
            fail(http_response::status::bad_gateway, "Invalid response from the target",
                 "invalid Content-Length in the response");
            co_return;
        }
        std::optional<size_t> length;
        if (!chunked && head.has_content_length_header()) length = head.get_content_length();

        // Head for the client: the status and end-to-end headers of the target
        http_response out;
        out.set_status(static_cast<uint16_t>(code));
        out.set_reason_phrase(head.get_reason());
        auto connection = connection_options(head);
        for (const auto& [name, value] : head.get_headers()) {
            if (!is_end_to_end(name, connection) || headers::is_header(name, header::content_length)) continue;
            out.add_header(name, value);
        }
        if (options_.on_response) options_.on_response(req_, out);
        if (!has_valid_headers(out)) {
            fail(http_response::status::bad_gateway, "Invalid response from the target", "invalid response header");
            co_return;
        }

        // Without a body, the length of the resource is only advertised (HEAD, 304)
        std::optional<size_t> advertised = length;
        if (no_body && !(head_request || incoming_->get_method() == method::HEAD || code == 304)) advertised.reset();
        if (!res_.start_stream(out, advertised)) {
            result_.error = "the response could not be sent";
            co_return;
        }
        response_started_ = true;
        result_.status = out.get_status_code();
        result_.from_target = true;

        bool relayed = true;
        if (!no_body) {
            if (chunked) {
                relayed = co_await relay_chunked();
            } else if (length) {
                relayed = co_await relay_length(*length);
            } else {
                relayed = co_await relay_until_close();
            }
        }
        if (!relayed) {
            // the client sees the response incomplete (no last chunk, or connection closed)
            res_.abort_stream();
            co_return;
        }
        response_ended_ = true;
        if (!co_await res_.end_stream()) {
            result_.error = "client connection lost";
            co_return;
        }
        result_.complete = request_sent_;
    }

    void proxy_exchange::upgrade() {
        const auto& head = *head_;
        const auto& protocol = head.get_header(header::upgrade);
        if (!upgrade_requested_ || protocol.empty()) {
            fail(http_response::status::bad_gateway, "Invalid response from the target", "unexpected protocol switch");
            return;
        }

        http_response out;
        out.set_status(http_response::status::switching_protocols);
        auto connection = connection_options(head);
        for (const auto& [name, value] : head.get_headers()) {
            if (!is_end_to_end(name, connection) || headers::is_header(name, header::content_length)) continue;
            out.add_header(name, value);
        }
        if (options_.on_response) options_.on_response(req_, out);
        for (auto name : {header::content_length, header::transfer_encoding, header::connection,
                          std::string("Keep-Alive"), header::upgrade}) {
            out.remove_headers(name);
        }
        if (!has_valid_headers(out)) {
            fail(http_response::status::bad_gateway, "Invalid response from the target", "invalid response header");
            return;
        }

        res_.status(http_response::status::switching_protocols);
        for (const auto& [name, value] : out.get_headers()) res_.header(name, value);
        res_.header(header::upgrade, protocol);
        res_.header(header::connection, "Upgrade");

        // Once the 101 is written, the client connection and the target become a tunnel,
        // starting with what each one sent past its head
        auto target = std::exchange(target_socket_, nullptr);
        res_.take_over([target, early = std::exchange(pending_, {})](std::shared_ptr<asio::socket> client, std::string buffered) {
            co_spawn(client->get_io_context(), run_tunnel(client, target, early, std::move(buffered)), detached);
        });
        result_.status = 101;
        result_.from_target = true;
        result_.upgraded = true;
        result_.complete = true;
    }

    awaitable<void> proxy_exchange::exchange() {
        bool target_open = co_await send_head();
        if (target_open && body_mode_ != body_mode::none && !body_buffered_) {
            // The response is read while the body is sent: a target may answer before reading
            // it all (e.g. 401, 413), and stop reading it
            auto result = co_await (read_head() || stream_body());
            if (stopped_ != stop_reason::none) co_return;
            if (result.index() != 0 || !std::get<0>(result)) co_return;
            if (!request_sent_) {
                // answered before the end of the body: what is left of it is not read, and the
                // connection with the client is closed after the response
                if (auto stream = req_.get_http_stream()) stream->set_keep_alive(false);
                incoming_->set_keep_alive(false);
            }
        } else {
            if (target_open) request_done();
            if (!co_await read_head()) co_return;
        }
        if (status_code_ == 101) {
            upgrade();
            co_return;
        }
        co_await relay_response();
    }

    awaitable<void> proxy_exchange::wait_forever() {
        boost::asio::steady_timer timer(executor_, clock::time_point::max());
        co_await timer.async_wait(use_nothrow_awaitable);
    }

    awaitable<void> proxy_exchange::watch_client() {
        // Only once the client sent the whole request: then reading from its connection
        // tells whether it closed it, without taking anything that belongs to the request
        if (!request_sent_) {
            co_await request_sent_signal_->async_wait(use_nothrow_awaitable);
            if (!request_sent_) co_return;  // the exchange ended
        }
        auto socket = req_.get_socket();
        // (data of the next request already read: the client is there)
        if (!socket || req_.read_ahead_available() > 0) {
            co_await wait_forever();
            co_return;
        }
        auto ec = co_await socket->wait(boost::asio::socket_base::wait_read);
        bool closed = ec || co_await socket->peer_closed();
        // (the exchange ended meanwhile: the wait was cancelled)
        auto state = co_await boost::asio::this_coro::cancellation_state;
        if (state.cancelled() != boost::asio::cancellation_type::none) co_return;
        // Not closed: the next request (pipelined), kept for the server, so the client is there.
        // Or the server closed the connection itself once the whole response was queued
        if (!closed || response_ended_) {
            co_await wait_forever();
            co_return;
        }
        // Gone: nobody would take the response (e.g. an event stream that stays silent)
        if (stopped_ == stop_reason::none) stopped_ = stop_reason::client_left;
        close_target();
    }

    awaitable<void> proxy_exchange::watchdog() {
        boost::asio::steady_timer timer(executor_);
        while (true) {
            auto deadline = clock::time_point::max();
            if (options_.idle_timeout.count() > 0) deadline = last_activity_ + options_.idle_timeout;
            if (options_.total_timeout.count() > 0) deadline = std::min(deadline, start_ + options_.total_timeout);
            if (clock::now() >= deadline) break;
            timer.expires_at(deadline);
            auto [ec] = co_await timer.async_wait(use_nothrow_awaitable);
            if (ec) co_return;  // the exchange ended
        }

        // Make whatever the exchange waits for fail: the target, and a client that does
        // not take the response (its connection is closed, as the response is incomplete)
        if (stopped_ == stop_reason::none) stopped_ = stop_reason::timeout;
        close_target();
        if (response_started_) {
            if (auto connection = res_.get_connection()) connection->release();
        }
    }

    awaitable<proxy_result> proxy_exchange::run() {
        executor_ = co_await boost::asio::this_coro::executor;
        start_ = clock::now();
        last_activity_ = start_;
        if (!incoming_) {
            result_.error = "no request";
            co_return result_;
        }
        if (!prepare()) co_return result_;
        if (!io_context()) {
            fail(http_response::status::not_implemented, "", "the handler does not run on an io_context");
            co_return result_;
        }
        if (!co_await connect()) co_return result_;

        request_sent_signal_.emplace(executor_, clock::time_point::max());
        co_await (exchange() || watchdog() || watch_client());

        switch (std::exchange(stopped_, stop_reason::none)) {
            case stop_reason::timeout:
                if (response_started_) {
                    result_.complete = false;
                    result_.error = "timeout relaying the response";
                    LOG_WARNING("reverse proxy {}: {}", describe(), result_.error);
                    res_.abort_stream();
                } else {
                    // what the exchange failed with is the consequence of the timeout
                    result_.error.clear();
                    fail(http_response::status::gateway_timeout, "The target did not answer in time",
                         "timeout waiting for the target");
                }
                break;
            case stop_reason::client_left:
                result_.complete = false;
                result_.error = "the client closed the connection";
                LOG_DEBUG("reverse proxy {}: {}", describe(), result_.error);
                if (response_started_) res_.abort_stream();
                break;
            default:
                break;
        }
        close_target();
        co_return result_;
    }

}

// --- proxy_target ---

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

// --- reverse_proxy ---

std::shared_ptr<boost::asio::ssl::context> reverse_proxy::tls_context(const proxy_tls_options& tls) {
    auto key = std::make_tuple(tls.verify, tls.ca_file, tls.ca_pem);
    std::lock_guard lock(tls_mutex_);
    if (auto it = tls_contexts_.find(key); it != tls_contexts_.end()) return it->second;

    auto context = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_client);
    context->set_options(boost::asio::ssl::context::default_workarounds | boost::asio::ssl::context::no_sslv2 |
                         boost::asio::ssl::context::no_sslv3 | boost::asio::ssl::context::no_tlsv1 |
                         boost::asio::ssl::context::no_tlsv1_1);
    if (tls.verify) {
        context->set_verify_mode(boost::asio::ssl::verify_peer);
        if (!tls.ca_file.empty()) {
            context->load_verify_file(tls.ca_file);
        } else if (!tls.ca_pem.empty()) {
            context->add_certificate_authority(boost::asio::buffer(tls.ca_pem));
        } else {
            context->set_default_verify_paths();
        }
    } else {
        context->set_verify_mode(boost::asio::ssl::verify_none);
    }
    tls_contexts_.emplace(key, context);
    return context;
}

awaitable<proxy_result> reverse_proxy::forward(request& req, response& res, const proxy_target& target) {
    co_return co_await forward(req, res, target, defaults_);
}

awaitable<proxy_result> reverse_proxy::forward(request& req, response& res, const proxy_target& target,
                                               const proxy_options& options) {
    proxy_exchange exchange(*this, req, res, target, options);
    co_return co_await exchange.run();
}

} // namespace thinger::http

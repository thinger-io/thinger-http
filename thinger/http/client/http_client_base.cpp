#include "http_client_base.hpp"
#include "client_error.hpp"
#include "websocket_util.hpp"
#include "../../util/logger.hpp"
#include "../../asio/sockets/ssl_socket.hpp"
#include "../../asio/sockets/unix_socket.hpp"
#include "../../asio/sockets/websocket.hpp"

#include <algorithm>
#include <filesystem>
#include <boost/algorithm/string.hpp>

namespace thinger::http {

http_client_base::http_client_base()
    : env_proxy_(std::make_shared<const proxy_resolver>(proxy_config::from_environment())) {
}

http_client_base::~http_client_base() {
    LOG_DEBUG("Destroying HTTP client base");
    pool_.clear();
}

bool http_client_base::is_same_origin(const std::string& url1, const std::string& url2) {
    http_request req1, req2;
    if (!req1.set_url(url1) || !req2.set_url(url2)) {
        return false;
    }
    return req1.get_base_path() == req2.get_base_path();
}

std::shared_ptr<http_request> http_client_base::create_request(method m, const std::string& url) {
    auto request = std::make_shared<http_request>();
    request->set_method(m);
    request->set_url(url);
    if (!unix_socket_.empty()) {
        request->set_unix_socket(unix_socket_);
    }
    apply_default_headers(request);
    return request;
}

void http_client_base::apply_default_headers(const std::shared_ptr<http_request>& request) {
    if (!request->has_header("User-Agent")) {
        request->add_header("User-Agent", user_agent_);
    }
    if (auto_decompress_ && !request->has_header("Accept-Encoding")) {
        request->add_header("Accept-Encoding", "gzip, deflate");
    }
}

http_client_base& http_client_base::proxy(const std::string& url, const std::string& no_proxy) {
    return proxy(proxy_config{url, url, no_proxy});
}

http_client_base& http_client_base::proxy(const proxy_config& config) {
    proxy_ = std::make_shared<const proxy_resolver>(config);
    return *this;
}

http_client_base& http_client_base::proxy_from_environment(bool enabled) {
    env_proxy_ = enabled ? std::make_shared<const proxy_resolver>(proxy_config::from_environment()) : nullptr;
    return *this;
}

proxy_resolver::choice http_client_base::select_proxy(const http_request& request) const {
    if (!request.get_unix_socket().empty()) return {};
    // The settings of the request, of the client, or of the environment: the first set
    const auto& host = request.get_host();
    const auto& port = request.get_port();
    if (const auto& config = request.get_proxy_config()) {
        return proxy_resolver(*config).select(request.is_ssl(), host, port);
    }
    if (proxy_) return proxy_->select(request.is_ssl(), host, port);
    if (env_proxy_) return env_proxy_->select(request.is_ssl(), host, port);
    return {};
}

std::string http_client_base::proxy_for(const http_request& request) const {
    auto choice = select_proxy(request);
    return choice.proxy ? choice.proxy->url() : std::string{};
}

std::string http_client_base::proxy_for(const std::string& url) const {
    http_request request;
    if (!request.set_url(url)) return {};
    return proxy_for(request);
}

std::shared_ptr<boost::asio::ssl::context> http_client_base::tls_context(
    const http_request& request, uint64_t& id, boost::system::error_code& ec) {

    // What the context trusts identifies it: files by path, modification time and size, so a
    // file changed in place is loaded again
    std::vector<const std::string*> files;
    std::vector<const std::string*> pems;
    std::string key = "none";
    if (verify_ssl_) {
        for (const auto& file : ca_files_) files.push_back(&file);
        for (const auto& file : request.get_ca_files()) files.push_back(&file);
        for (const auto& pem : ca_pems_) pems.push_back(&pem);
        for (const auto& pem : request.get_ca_pems()) pems.push_back(&pem);
        key = system_ca_ ? "system" : "custom";
        for (const auto* file : files) {
            std::error_code time_ec, size_ec;
            auto time = std::filesystem::last_write_time(*file, time_ec);
            auto size = std::filesystem::file_size(*file, size_ec);
            key += "\nfile:" + *file + ":" + (time_ec ? "-" : std::to_string(time.time_since_epoch().count())) +
                   ":" + (size_ec ? "-" : std::to_string(size));
        }
        for (const auto* pem : pems) key += "\npem:" + *pem;
    }

    std::lock_guard lock(tls_mutex_);
    if (auto it = tls_contexts_.find(key); it != tls_contexts_.end()) {
        id = it->second.id;
        return it->second.context;
    }

    auto context = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::sslv23_client);
    if (!verify_ssl_) {
        context->set_verify_mode(boost::asio::ssl::verify_none);
    } else {
        context->set_verify_mode(boost::asio::ssl::verify_peer);
        boost::system::error_code load_ec;
        if (system_ca_) {
            context->set_default_verify_paths(load_ec);
            if (load_ec) LOG_WARNING("cannot load the system CAs: {}", load_ec.message());
        }
        for (const auto* file : files) {
            context->load_verify_file(*file, load_ec);
            if (load_ec) {
                LOG_ERROR("cannot load the CA file {}: {}", *file, load_ec.message());
                ec = client_error::invalid_ca_certificate;
                return nullptr;
            }
        }
        for (const auto* pem : pems) {
            context->add_certificate_authority(boost::asio::buffer(*pem), load_ec);
            if (load_ec) {
                LOG_ERROR("cannot load a CA certificate (PEM): {}", load_ec.message());
                ec = client_error::invalid_ca_certificate;
                return nullptr;
            }
        }
    }

    // Bounded: CAs that change (or many per-request sets) would grow it forever. The
    // connections keep the contexts they use
    if (tls_contexts_.size() >= MAX_TLS_CONTEXTS) tls_contexts_.clear();
    id = ++next_tls_id_;
    tls_contexts_.emplace(std::move(key), tls_entry{id, context});
    return context;
}

http_client_base::connection_plan http_client_base::plan_connection(const http_request& request,
                                                                     boost::system::error_code& ec) {
    connection_plan plan;
    uint64_t tls_id = 0;
    if (request.is_ssl()) {
        plan.tls = tls_context(request, tls_id, ec);
        if (ec) return plan;
    }

    auto choice = select_proxy(request);
    if (!choice.error.empty()) {
        LOG_ERROR("cannot use the proxy for {}: {}", request.get_url(), choice.error);
        ec = client_error::invalid_proxy;
        return plan;
    }

    // Connections are shared only by requests reaching their origin the same way, and
    // trusting the same CAs: never a direct one for a proxied request, nor the other way
    plan.key = "tls:" + std::to_string(tls_id);
    if (choice.proxy) {
        plan.route.proxy = choice.proxy;
        // Plain requests are forwarded by the proxy. TLS goes end to end through a tunnel, as
        // do protocol upgrades (WebSocket), which a forwarding proxy may not pass on (as
        // browsers do)
        plan.route.tunnel = request.is_ssl() || request.has_header(header::upgrade);
        plan.route.tls = plan.tls;
        plan.key += (plan.route.tunnel ? "|connect:" : "|forward:") + choice.proxy->key();
    }
    return plan;
}

std::shared_ptr<client_connection> http_client_base::make_connection(const connection_plan& plan) {
    auto& io_context = get_io_context();
    std::shared_ptr<client_connection> connection;
    if (plan.route.proxy) {
        connection = std::make_shared<client_connection>(io_context, plan.route, timeout_);
    } else {
        std::shared_ptr<thinger::asio::socket> sock;
        if (plan.tls) {
            sock = std::make_shared<thinger::asio::ssl_socket>("http_client", io_context, plan.tls);
        } else {
            sock = std::make_shared<thinger::asio::tcp_socket>("http_client", io_context);
        }
        connection = std::make_shared<client_connection>(sock, timeout_);
    }
    connection->set_max_content_size(max_content_size_);
    connection->set_decompress(auto_decompress_);
    return connection;
}

connection_pool::lease http_client_base::get_connection(const std::shared_ptr<http_request>& request,
                                                        boost::system::error_code& ec, bool reuse) {
    ec = {};
    const std::string& socket_path = request->get_unix_socket();

    // Connections are interchangeable if they reach the same origin the same way. Through a
    // forwarding proxy, they reach the proxy: any origin is requested on them (as curl does)
    std::string key;
    std::string origin;
    connection_plan plan;
    if (!socket_path.empty()) {
        // Unix sockets are local: never through a proxy
        key = origin = "unix:" + socket_path;
    } else {
        plan = plan_connection(*request, ec);
        if (ec) return {};
        if (plan.route.proxy && !plan.route.tunnel) {
            key = plan.key;
            origin = "proxy " + plan.route.proxy->url();
        } else {
            origin = (request->is_ssl() ? "https://" : "http://") + request->get_host() + ":" + request->get_port();
            key = origin + "|" + plan.key;
        }
    }

    if (reuse) {
        if (auto lease = pool_.acquire(key)) {
            LOG_DEBUG("Reusing connection from pool for {}", origin);
            lease->set_max_content_size(max_content_size_);
            lease->set_decompress(auto_decompress_);
            lease->set_timeout(timeout_);
            return lease;
        }
    }

    std::shared_ptr<client_connection> connection;
    if (!socket_path.empty()) {
        auto sock = std::make_shared<thinger::asio::unix_socket>("http_client", get_io_context());
        connection = std::make_shared<client_connection>(sock, socket_path, timeout_);
        connection->set_max_content_size(max_content_size_);
        connection->set_decompress(auto_decompress_);
    } else {
        LOG_DEBUG("Creating new connection for {}{}", request->get_host(),
                  plan.route.proxy ? " through the proxy " + plan.route.proxy->url() : std::string{});
        connection = make_connection(plan);
    }
    return pool_.add(key, origin, std::move(connection));
}

http_client_base& http_client_base::max_idle_connections_per_host(size_t max) {
    auto config = pool_.get_config();
    config.max_idle_per_host = max;
    pool_.configure(config);
    return *this;
}

http_client_base& http_client_base::max_idle_connections(size_t max) {
    auto config = pool_.get_config();
    config.max_idle = max;
    pool_.configure(config);
    return *this;
}

http_client_base& http_client_base::idle_timeout(std::chrono::milliseconds timeout) {
    auto config = pool_.get_config();
    config.idle_timeout = timeout;
    pool_.configure(config);
    return *this;
}

bool http_client_base::can_retry(const client_connection& connection, const http_request& request) {
    // A server may close an idle connection at any time (RFC 9112 section 9.3.1): a request
    // failing on a reused one before any byte of its response arrived most likely met that.
    // It is sent again on a new connection if that is safe, as Go does: nothing of it was
    // written, or it is idempotent (RFC 9110 section 9.2.2) or has an idempotency key.
    // Otherwise (e.g. a POST) the server may have processed it, and the error is returned.
    // Never after a timeout: the server may be just slow
    const auto& ec = connection.last_error();
    if (!ec || !connection.reused() || connection.response_started()) return false;
    if (ec == boost::asio::error::timed_out || ec == boost::asio::error::operation_aborted) return false;
    if (connection.request_unsent()) return true;
    switch (request.get_method()) {
        case method::GET:
        case method::HEAD:
        case method::OPTIONS:
        case method::TRACE:
        case method::PUT:
        case method::DELETE:
            return true;
        default:
            return request.has_header("Idempotency-Key") || request.has_header("X-Idempotency-Key");
    }
}

awaitable<std::shared_ptr<http_response>> http_client_base::send_request(const std::shared_ptr<http_request>& request,
                                                                          connection_pool::lease& lease) {
    auto response = co_await lease->send_request(request);
    if (!response && can_retry(*lease, *request)) {
        LOG_DEBUG("reused connection to {} failed before the response ({}): sending again on a new one",
                  request->get_host(), lease->last_error().message());
        boost::system::error_code ec;
        if (auto fresh = get_connection(request, ec, false)) {
            lease = std::move(fresh);
            response = co_await lease->send_request(request);
        }
    }
    co_return response;
}

// HTTP methods implementation
awaitable<client_response> http_client_base::get(const std::string& url, headers_map headers) {
    auto request = create_request(method::GET, url);
    for (const auto& [key, value] : headers) {
        request->add_header(key, value);
    }
    co_return co_await send(request);
}

awaitable<client_response> http_client_base::post(const std::string& url, std::string body,
                                                  std::string content_type, headers_map headers) {
    auto request = create_request(method::POST, url);
    if (!body.empty()) {
        request->set_content(std::move(body), std::move(content_type));
    }
    for (const auto& [key, value] : headers) {
        request->add_header(key, value);
    }
    co_return co_await send(request);
}

awaitable<client_response> http_client_base::post(const std::string& url, const form& form, headers_map headers) {
    return post(url, form.body(), form.content_type(), std::move(headers));
}

awaitable<client_response> http_client_base::put(const std::string& url, std::string body,
                                                 std::string content_type, headers_map headers) {
    auto request = create_request(method::PUT, url);
    if (!body.empty()) {
        request->set_content(std::move(body), std::move(content_type));
    }
    for (const auto& [key, value] : headers) {
        request->add_header(key, value);
    }
    co_return co_await send(request);
}

awaitable<client_response> http_client_base::patch(const std::string& url, std::string body,
                                                   std::string content_type, headers_map headers) {
    auto request = create_request(method::PATCH, url);
    if (!body.empty()) {
        request->set_content(std::move(body), std::move(content_type));
    }
    for (const auto& [key, value] : headers) {
        request->add_header(key, value);
    }
    co_return co_await send(request);
}

awaitable<client_response> http_client_base::del(const std::string& url, headers_map headers) {
    auto request = create_request(method::DELETE, url);
    for (const auto& [key, value] : headers) {
        request->add_header(key, value);
    }
    co_return co_await send(request);
}

awaitable<client_response> http_client_base::head(const std::string& url, headers_map headers) {
    auto request = create_request(method::HEAD, url);
    for (const auto& [key, value] : headers) {
        request->add_header(key, value);
    }
    co_return co_await send(request);
}

awaitable<client_response> http_client_base::options(const std::string& url, headers_map headers) {
    auto request = create_request(method::OPTIONS, url);
    for (const auto& [key, value] : headers) {
        request->add_header(key, value);
    }
    co_return co_await send(request);
}

awaitable<client_response> http_client_base::send(std::shared_ptr<http_request> request) {
    co_return co_await send_with_redirects(std::move(request));
}

awaitable<stream_result> http_client_base::send_streaming(std::shared_ptr<http_request> request,
                                                          stream_callback callback) {
    // Force no compression for streaming - we can't decompress chunks on the fly
    // Set this BEFORE apply_default_headers so it won't add gzip/deflate
    if (!request->has_header("Accept-Encoding")) {
        request->add_header("Accept-Encoding", "identity");
    }
    apply_default_headers(request);
    boost::system::error_code ec;
    auto lease = get_connection(request, ec);
    if (!lease) {
        stream_result result;
        result.error = ec.message();
        co_return result;
    }
    // (the callback is not called before the response starts: it is not called twice for a
    // request sent again)
    auto forward = [&callback](const stream_info& info) { return callback(info); };
    auto result = co_await lease->send_request_streaming(request, forward);
    if (lease->last_error() && can_retry(*lease, *request)) {
        LOG_DEBUG("reused connection to {} failed before the response ({}): sending again on a new one",
                  request->get_host(), lease->last_error().message());
        if (auto fresh = get_connection(request, ec, false)) {
            lease = std::move(fresh);
            result = co_await lease->send_request_streaming(request, forward);
        }
    }
    // Back to the pool only if the body was read whole (not aborted by the callback)
    co_return result;
}

awaitable<std::pair<client_response, std::shared_ptr<client_connection>>>
http_client_base::send_with_connection(std::shared_ptr<http_request> request) {
    std::shared_ptr<client_connection> connection;
    auto response = co_await send_with_redirects(std::move(request), &connection);
    co_return std::make_pair(std::move(response), std::move(connection));
}

awaitable<client_response> http_client_base::send_with_redirects(std::shared_ptr<http_request> request,
                                                                 std::shared_ptr<client_connection>* used) {
    for (unsigned int redirect_count = 0;; ++redirect_count) {
        boost::system::error_code ec;
        auto lease = get_connection(request, ec);
        if (!lease) co_return client_response(ec, nullptr);

        auto response = co_await send_request(request, lease);
        if (used) *used = lease.connection();

        if (!response) {
            ec = lease->last_error();
            // (with the answer of a proxy refusing the tunnel, if that was the error)
            co_return client_response(ec ? ec : boost::asio::error::invalid_argument, lease->proxy_response());
        }

        // Back to the pool (if it can carry another request): the redirect may reuse it
        lease.release();

        if (!follow_redirects_ || !response->is_redirect_response() ||
            redirect_count >= max_redirects_ || !response->has_header("Location")) {
            co_return client_response(boost::system::error_code{}, response);
        }

        std::string location = response->get_header("Location");
        LOG_DEBUG("Following redirect #{} to: {}", redirect_count + 1, location);

        // Determine redirect method based on status code
        method redirect_method = request->get_method();
        int status = response->get_status_code();

        // 303 See Other always changes to GET
        if (status == 303) {
            redirect_method = method::GET;
        }
        // 301/302 traditionally change POST/PUT/DELETE to GET (browser behavior)
        else if ((status == 301 || status == 302) &&
                 (request->get_method() == method::POST ||
                  request->get_method() == method::PUT ||
                  request->get_method() == method::DELETE)) {
            redirect_method = method::GET;
        }
        // 307 and 308 preserve the original method

        // Create redirect request
        auto redirect_request = create_request(redirect_method, location);

        // Preserve body for 307/308
        if ((status == 307 || status == 308) && !request->get_body().empty()) {
            redirect_request->set_content(request->get_body());
            if (request->has_header("Content-Type")) {
                redirect_request->add_header("Content-Type", request->get_header("Content-Type"));
            }
            if (request->has_header("Content-Length")) {
                redirect_request->add_header("Content-Length", request->get_header("Content-Length"));
            }
        }

        // Copy Authorization only for same origin
        std::string original_url = request->get_url();
        if (request->has_header("Authorization") && is_same_origin(original_url, location)) {
            redirect_request->add_header("Authorization", request->get_header("Authorization"));
            LOG_DEBUG("Preserving Authorization header for same-origin redirect");
        }

        // Handle cookies
        if (request->get_cookie_store().update_from_headers(*response)) {
            redirect_request->set_header(header::cookie, request->get_cookie_store().get_cookie_string());
        }

        // The proxy and CAs of the request apply to every hop; whether the proxy is used is
        // decided again for the host of each one (NO_PROXY)
        if (const auto& config = request->get_proxy_config()) {
            redirect_request->set_proxy_config(*config);
        }
        for (const auto& file : request->get_ca_files()) redirect_request->add_ca_file(file);
        for (const auto& pem : request->get_ca_pems()) redirect_request->add_ca_pem(pem);

        request = std::move(redirect_request);
    }
}

// Simple URL version delegates to the request version
awaitable<std::optional<websocket_client>> http_client_base::upgrade_websocket(
    const std::string& url, const std::string& subprotocol) {
    auto request = std::make_shared<http_request>();
    request->set_url(url);
    co_return co_await upgrade_websocket(std::move(request), subprotocol);
}

// Main implementation with request (supports custom headers)
awaitable<std::optional<websocket_client>> http_client_base::upgrade_websocket(
    std::shared_ptr<http_request> request, const std::string& subprotocol) {

    // Get URL from request
    const std::string& url = request->get_url();

    // Parse WebSocket URL (handle ws://, wss://, http://, https:// schemes)
    auto components = websocket_util::parse_websocket_url(url);
    if (!components) {
        // Try parsing as http/https URL
        auto ws_url = url;
        if (ws_url.starts_with("http://")) {
            ws_url = "ws://" + ws_url.substr(7);
        } else if (ws_url.starts_with("https://")) {
            ws_url = "wss://" + ws_url.substr(8);
        }
        components = websocket_util::parse_websocket_url(ws_url);
        if (!components) {
            LOG_ERROR("Invalid WebSocket URL: {}", url);
            co_return std::nullopt;
        }
    }

    // Set correct URL in request
    std::string http_url = (components->secure ? "https://" : "http://")
                          + components->host + ":" + components->port + components->path;
    request->set_url(http_url);
    request->set_method(method::GET);

    // Add WebSocket-specific headers (use set_header to avoid duplicates if request already has these)
    request->set_header("Upgrade", "websocket");
    request->set_header("Connection", "Upgrade");

    std::string ws_key = websocket_util::generate_websocket_key();
    request->add_header("Sec-WebSocket-Key", ws_key);
    request->add_header("Sec-WebSocket-Version", "13");

    if (!subprotocol.empty()) {
        request->add_header("Sec-WebSocket-Protocol", subprotocol);
    }

    apply_default_headers(request);

    // A connection of its own, never pooled: it becomes the WebSocket. Through a proxy, it
    // is a tunnel (CONNECT) for ws:// too, as the request has Upgrade
    boost::system::error_code ec;
    auto plan = plan_connection(*request, ec);
    if (ec) {
        LOG_ERROR("WebSocket connect error: {}", ec.message());
        co_return std::nullopt;
    }
    auto connection = make_connection(plan);

    // Send upgrade request
    auto response = co_await connection->send_request(request);

    if (!response || response->get_status_code() != 101) {
        if (!response) {
            LOG_ERROR("WebSocket connect error: {}", connection->last_error().message());
        }
        LOG_ERROR("WebSocket upgrade failed: {}",
                 response ? std::to_string(response->get_status_code()) : "no response");
        co_return std::nullopt;
    }

    // Validate accept key
    auto accept_key = response->get_header("Sec-WebSocket-Accept");
    if (!websocket_util::validate_accept_key(accept_key, ws_key)) {
        LOG_ERROR("Invalid Sec-WebSocket-Accept key");
        co_return std::nullopt;
    }

    // The subprotocol accepted must be one offered (RFC 6455, 4.1)
    auto accepted_protocol = response->get_header("Sec-WebSocket-Protocol");
    if (!accepted_protocol.empty()) {
        std::vector<std::string> offered;
        boost::split(offered, subprotocol, boost::is_any_of(","));
        for (auto& protocol : offered) boost::trim(protocol);
        if (std::find(offered.begin(), offered.end(), accepted_protocol) == offered.end()) {
            LOG_ERROR("WebSocket server accepted a subprotocol not offered: {}", accepted_protocol);
            co_return std::nullopt;
        }
    }

    // Upgrade to WebSocket
    auto raw_socket = connection->release_socket();
    auto ws = std::make_shared<asio::websocket>(raw_socket, false, false);
    ws->set_protocol(accepted_protocol);
    // Frames the server sent right after its response, read along with it
    ws->set_read_ahead(connection->take_buffered());

    LOG_INFO("WebSocket connected to {}", url);

    co_return websocket_client(std::move(ws));
}

} // namespace thinger::http

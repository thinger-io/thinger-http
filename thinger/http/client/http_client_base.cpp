#include "http_client_base.hpp"
#include "websocket_util.hpp"
#include "../../util/logger.hpp"
#include "../../asio/sockets/ssl_socket.hpp"
#include "../../asio/sockets/unix_socket.hpp"
#include "../../asio/sockets/websocket.hpp"

namespace thinger::http {

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
    apply_default_headers(request);
    return request;
}

std::shared_ptr<http_request> http_client_base::create_request(method m, const std::string& url,
                                                                const std::string& unix_socket) {
    auto request = std::make_shared<http_request>();
    request->set_method(m);
    request->set_url(url);
    request->set_unix_socket(unix_socket);
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

std::shared_ptr<client_connection> http_client_base::get_or_create_connection(
    const std::shared_ptr<http_request>& request) {

    auto& io_context = get_io_context();
    const std::string& socket_path = request->get_unix_socket();

    // Try to get existing connection from pool
    std::shared_ptr<client_connection> connection;
    if (socket_path.empty()) {
        connection = pool_.get_connection(request->get_host(),
                                          std::stoi(request->get_port()),
                                          request->is_ssl());
    } else {
        connection = pool_.get_unix_connection(socket_path);
    }

    // If found in pool and still open, reuse it
    if (connection && connection->is_open()) {
        LOG_DEBUG("Reusing connection from pool for {}", request->get_host());
        return connection;
    }

    // Create new connection
    LOG_DEBUG("Creating new connection for {}", request->get_host());

    if (socket_path.empty()) {
        std::shared_ptr<thinger::asio::socket> sock;
        if (!request->is_ssl()) {
            sock = std::make_shared<thinger::asio::tcp_socket>("http_client", io_context);
        } else {
            auto ssl_context = std::make_shared<boost::asio::ssl::context>(
                boost::asio::ssl::context::sslv23_client);
            ssl_context->set_default_verify_paths();
            if (!verify_ssl_) {
                ssl_context->set_verify_mode(boost::asio::ssl::verify_none);
            }
            sock = std::make_shared<thinger::asio::ssl_socket>("http_client", io_context, ssl_context);
        }
        connection = std::make_shared<client_connection>(sock, timeout_);

        // Store in pool for reuse
        pool_.store_connection(request->get_host(),
                              std::stoi(request->get_port()),
                              request->is_ssl(),
                              connection);
    } else {
        auto sock = std::make_shared<thinger::asio::unix_socket>("http_client", io_context);
        connection = std::make_shared<client_connection>(sock, socket_path, timeout_);

        // Store in pool for reuse
        pool_.store_unix_connection(socket_path, connection);
    }

    return connection;
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

// Unix socket variants
awaitable<client_response> http_client_base::get(const std::string& url, const std::string& unix_socket,
                                                  headers_map headers) {
    auto request = create_request(method::GET, url, unix_socket);
    for (const auto& [key, value] : headers) {
        request->add_header(key, value);
    }
    co_return co_await send(request);
}

awaitable<client_response> http_client_base::post(const std::string& url, const std::string& unix_socket,
                                                   std::string body, std::string content_type,
                                                   headers_map headers) {
    auto request = create_request(method::POST, url, unix_socket);
    if (!body.empty()) {
        request->set_content(std::move(body), std::move(content_type));
    }
    for (const auto& [key, value] : headers) {
        request->add_header(key, value);
    }
    co_return co_await send(request);
}

awaitable<client_response> http_client_base::send(std::shared_ptr<http_request> request) {
    auto connection = get_or_create_connection(request);
    co_return co_await send_with_redirects(std::move(request), connection, 0);
}

awaitable<stream_result> http_client_base::send_streaming(std::shared_ptr<http_request> request,
                                                          stream_callback callback) {
    // Force no compression for streaming - we can't decompress chunks on the fly
    // Set this BEFORE apply_default_headers so it won't add gzip/deflate
    if (!request->has_header("Accept-Encoding")) {
        request->add_header("Accept-Encoding", "identity");
    }
    apply_default_headers(request);
    auto connection = get_or_create_connection(request);
    co_return co_await connection->send_request_streaming(std::move(request), std::move(callback));
}

awaitable<std::pair<client_response, std::shared_ptr<client_connection>>>
http_client_base::send_with_connection(std::shared_ptr<http_request> request) {
    auto connection = get_or_create_connection(request);
    auto response = co_await send_with_redirects(request, connection, 0);
    co_return std::make_pair(std::move(response), connection);
}

awaitable<client_response> http_client_base::send_with_redirects(
    std::shared_ptr<http_request> request,
    std::shared_ptr<client_connection> connection,
    unsigned int redirect_count) {

    // Send request
    auto response = co_await connection->send_request(request);

    if (!response) {
        co_return client_response(boost::asio::error::invalid_argument, nullptr);
    }

    // Handle redirects
    if (follow_redirects_ && response->is_redirect_response() &&
        redirect_count < max_redirects_ && response->has_header("Location")) {

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

        // Get connection for redirect (may be different host)
        auto redirect_connection = get_or_create_connection(redirect_request);

        co_return co_await send_with_redirects(redirect_request, redirect_connection, redirect_count + 1);
    }

    co_return client_response(boost::system::error_code{}, response);
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

    auto& io_context = get_io_context();

    // Create socket based on scheme
    std::shared_ptr<asio::socket> socket;
    if (components->secure) {
        auto ssl_context = std::make_shared<boost::asio::ssl::context>(
            boost::asio::ssl::context::sslv23_client);
        ssl_context->set_default_verify_paths();
        if (!verify_ssl_) {
            ssl_context->set_verify_mode(boost::asio::ssl::verify_none);
        }
        socket = std::make_shared<asio::ssl_socket>("wss_client", io_context, ssl_context);
    } else {
        socket = std::make_shared<asio::tcp_socket>("ws_client", io_context);
    }

    // Connect
    auto ec = co_await socket->connect(components->host, components->port, timeout_);
    if (ec) {
        LOG_ERROR("WebSocket connect error: {}", ec.message());
        co_return std::nullopt;
    }

    // Create connection for HTTP upgrade
    auto connection = std::make_shared<client_connection>(socket, timeout_);

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

    // Send upgrade request
    auto response = co_await connection->send_request(request);

    if (!response || response->get_status_code() != 101) {
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

    // Upgrade to WebSocket
    auto raw_socket = connection->release_socket();
    auto ws = std::make_shared<asio::websocket>(raw_socket, false, false);

    LOG_INFO("WebSocket connected to {}", url);

    co_return websocket_client(std::move(ws));
}

} // namespace thinger::http

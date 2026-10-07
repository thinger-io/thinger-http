#include "http_server_base.hpp"
#include "server_connection.hpp"
#include "request.hpp"
#include "response.hpp"
#include "../../util/logger.hpp"
#include "../../util/base64.hpp"
#include <filesystem>

namespace thinger::http {

// Route registration methods - GET
route& http_server_base::get(const std::string& path, route_callback_response_only handler) {
    return router_[method::GET][path] = handler;
}

route& http_server_base::get(const std::string& path, route_callback_json_response handler) {
    return router_[method::GET][path] = handler;
}

route& http_server_base::get(const std::string& path, route_callback_request_response handler) {
    return router_[method::GET][path] = handler;
}

route& http_server_base::get(const std::string& path, route_callback_request_json_response handler) {
    return router_[method::GET][path] = handler;
}

// Route registration methods - POST
route& http_server_base::post(const std::string& path, route_callback_response_only handler) {
    return router_[method::POST][path] = handler;
}

route& http_server_base::post(const std::string& path, route_callback_json_response handler) {
    return router_[method::POST][path] = handler;
}

route& http_server_base::post(const std::string& path, route_callback_request_response handler) {
    return router_[method::POST][path] = handler;
}

route& http_server_base::post(const std::string& path, route_callback_request_json_response handler) {
    return router_[method::POST][path] = handler;
}

// Route registration methods - PUT
route& http_server_base::put(const std::string& path, route_callback_response_only handler) {
    return router_[method::PUT][path] = handler;
}

route& http_server_base::put(const std::string& path, route_callback_json_response handler) {
    return router_[method::PUT][path] = handler;
}

route& http_server_base::put(const std::string& path, route_callback_request_response handler) {
    return router_[method::PUT][path] = handler;
}

route& http_server_base::put(const std::string& path, route_callback_request_json_response handler) {
    return router_[method::PUT][path] = handler;
}

// Route registration methods - DELETE
route& http_server_base::del(const std::string& path, route_callback_response_only handler) {
    return router_[method::DELETE][path] = handler;
}

route& http_server_base::del(const std::string& path, route_callback_json_response handler) {
    return router_[method::DELETE][path] = handler;
}

route& http_server_base::del(const std::string& path, route_callback_request_response handler) {
    return router_[method::DELETE][path] = handler;
}

route& http_server_base::del(const std::string& path, route_callback_request_json_response handler) {
    return router_[method::DELETE][path] = handler;
}

// Route registration methods - PATCH
route& http_server_base::patch(const std::string& path, route_callback_response_only handler) {
    return router_[method::PATCH][path] = handler;
}

route& http_server_base::patch(const std::string& path, route_callback_json_response handler) {
    return router_[method::PATCH][path] = handler;
}

route& http_server_base::patch(const std::string& path, route_callback_request_response handler) {
    return router_[method::PATCH][path] = handler;
}

route& http_server_base::patch(const std::string& path, route_callback_request_json_response handler) {
    return router_[method::PATCH][path] = handler;
}

// Route registration methods - HEAD
route& http_server_base::head(const std::string& path, route_callback_response_only handler) {
    return router_[method::HEAD][path] = handler;
}

route& http_server_base::head(const std::string& path, route_callback_request_response handler) {
    return router_[method::HEAD][path] = handler;
}

// Route registration methods - OPTIONS
route& http_server_base::options(const std::string& path, route_callback_response_only handler) {
    return router_[method::OPTIONS][path] = handler;
}

route& http_server_base::options(const std::string& path, route_callback_request_response handler) {
    return router_[method::OPTIONS][path] = handler;
}

// Middleware
void http_server_base::use(async_middleware_function middleware) {
    middlewares_.push_back(std::move(middleware));
}

void http_server_base::use(middleware_function middleware) {
    use([middleware = std::move(middleware)](request& req, response& res) -> awaitable<bool> {
        // shared flag: a misbehaving middleware may still call next() after returning
        auto passed = std::make_shared<bool>(false);
        middleware(req, res, [passed]() { *passed = true; });
        co_return *passed;
    });
}

// Basic Auth helpers
void http_server_base::set_basic_auth(const std::string& path_prefix, 
                                 const std::string& realm,
                                 auth_verify_function verify) {
    use([path_prefix, realm, verify](request& req, response& res) -> awaitable<bool> {
        // Get the request path
        auto http_request = req.get_http_request();
        if (!http_request) co_return true;

        const std::string& path = http_request->get_uri();

        // Check if this path requires auth
        if (!path.starts_with(path_prefix)) co_return true;
        
        // Check for Authorization header
        if (!http_request->has_header("Authorization")) {
            res.status(http_response::status::unauthorized);
            res.header("WWW-Authenticate", "Basic realm=\"" + realm + "\"");
            res.send("Authentication required");
            co_return false;
        }
        
        auto auth_header = http_request->get_header("Authorization");
        if (!auth_header.starts_with("Basic ")) {
            res.status(http_response::status::unauthorized);
            res.header("WWW-Authenticate", "Basic realm=\"" + realm + "\"");
            res.send("Invalid authentication");
            co_return false;
        }
        
        // Decode base64 credentials
        auto encoded = auth_header.substr(6);
        std::string decoded;
        try {
            decoded = ::thinger::util::base64::decode(encoded);
        } catch (...) {
            res.status(http_response::status::unauthorized);
            res.send("Invalid credentials format");
            co_return false;
        }
        
        // Parse username:password
        auto colon_pos = decoded.find(':');
        if (colon_pos == std::string::npos) {
            res.status(http_response::status::unauthorized);
            res.send("Invalid credentials format");
            co_return false;
        }
        
        std::string username = decoded.substr(0, colon_pos);
        std::string password = decoded.substr(colon_pos + 1);
        
        // Verify credentials
        if (verify(username, password)) {
            req.set_auth_user(username);
            co_return true;
        }

        res.status(http_response::status::unauthorized);
        res.header("WWW-Authenticate", "Basic realm=\"" + realm + "\"");
        res.send("Invalid username or password");
        co_return false;
    });
}

void http_server_base::set_basic_auth(const std::string& path_prefix,
                                 const std::string& realm,
                                 const std::string& username,
                                 const std::string& password) {
    set_basic_auth(path_prefix, realm, 
        [username, password](const std::string& u, const std::string& p) {
            return u == username && p == password;
        });
}

void http_server_base::set_basic_auth(const std::string& path_prefix,
                                 const std::string& realm,
                                 const std::map<std::string, std::string>& users) {
    set_basic_auth(path_prefix, realm,
        [users](const std::string& u, const std::string& p) {
            auto it = users.find(u);
            return it != users.end() && it->second == p;
        });
}

// Fallback handlers
void http_server_base::set_not_found_handler(route_callback_response_only handler) {
    router_.set_fallback_handler([handler](request& req, response& res) {
        handler(res);
    });
}

void http_server_base::set_not_found_handler(route_callback_request_response handler) {
    router_.set_fallback_handler(handler);
}

// Configuration
void http_server_base::enable_cors(bool enabled) {
    cors_enabled_ = enabled;
}

void http_server_base::enable_ssl(bool enabled) {
    ssl_enabled_ = enabled;
}

void http_server_base::set_connection_timeout(std::chrono::seconds timeout) {
    connection_timeout_ = timeout;
}

void http_server_base::set_max_body_size(size_t size) {
    max_body_size_ = size;
}

void http_server_base::set_max_listening_attempts(int attempts) {
    max_listening_attempts_ = attempts;
}

// Static file serving
void http_server_base::serve_static(const std::string& url_prefix,
                               const std::string& directory,
                               const std::string& fallback) {
    namespace fs = std::filesystem;

    // Normalize route: avoid double slash when prefix is "/"
    std::string route = url_prefix;
    if (!route.empty() && route.back() == '/') route.pop_back();
    route += "/:path(.*)";

    get(route, [directory, fallback](request& req, response& res) {
        std::string path = req["path"];

        auto canonical_dir = fs::canonical(directory);
        bool has_fallback = !fallback.empty();

        // Empty path means root request — try fallback file directly
        if (path.empty()) {
            if (has_fallback) {
                auto fallback_file = canonical_dir / fallback;
                if (fs::exists(fallback_file) && fs::is_regular_file(fallback_file)) {
                    res.send_file(fallback_file);
                    return;
                }
            }
            res.status(http_response::status::not_found);
            res.send("Not found");
            return;
        }

        // Construct full file path
        fs::path file_path = fs::path(directory) / path;
        auto canonical_file = fs::weakly_canonical(file_path);

        // Security: ensure the resolved path is within the directory
        if (!canonical_file.string().starts_with(canonical_dir.string())) {
            res.status(http_response::status::forbidden);
            res.send("Access denied");
            return;
        }

        // Serve file if it exists
        if (fs::exists(canonical_file)) {
            if (fs::is_regular_file(canonical_file)) {
                res.send_file(canonical_file);
                return;
            }
            if (fs::is_directory(canonical_file) && has_fallback) {
                auto fallback_file = canonical_file / fallback;
                if (fs::exists(fallback_file) && fs::is_regular_file(fallback_file)) {
                    res.send_file(fallback_file);
                    return;
                }
            }
        }

        // SPA fallback: serve root fallback file for non-existent paths
        if (has_fallback) {
            auto fallback_file = canonical_dir / fallback;
            if (fs::exists(fallback_file) && fs::is_regular_file(fallback_file)) {
                res.send_file(fallback_file);
                return;
            }
        }

        res.status(http_response::status::not_found);
        res.send("Not found");
    });
}

// Server control
bool http_server_base::listen(const std::string& host, uint16_t port) {
    host_ = host;
    port_ = std::to_string(port);
    use_unix_socket_ = false;
    
    // Create socket server using virtual method
    socket_server_ = create_socket_server(host, port_);
    if (!socket_server_) {
        LOG_ERROR("Failed to create socket server");
        return false;
    }
    
    // Configure socket server
    socket_server_->set_max_listening_attempts(max_listening_attempts_);
    
    // Setup connection handler
    setup_connection_handler();
    
    // Start listening
    return socket_server_->start();
}

bool http_server_base::listen_unix(const std::string& unix_path) {
    unix_path_ = unix_path;
    use_unix_socket_ = true;
    
    // Create Unix socket server using virtual method
    socket_server_ = create_unix_socket_server(unix_path);
    if (!socket_server_) {
        LOG_ERROR("Failed to create Unix socket server");
        return false;
    }
    
    // Configure socket server
    socket_server_->set_max_listening_attempts(max_listening_attempts_);
    
    // Setup connection handler
    setup_connection_handler();
    
    // Start listening
    return socket_server_->start();
}

bool http_server_base::start(uint16_t port) {
    return start("0.0.0.0", port);
}

bool http_server_base::start(uint16_t port, const std::function<void()> &on_listening) {
    return start("0.0.0.0", port, on_listening);
}

bool http_server_base::start(const std::string& host, uint16_t port) {
    return start(host, port, nullptr);
}

bool http_server_base::start(const std::string& host, uint16_t port, const std::function<void()> &on_listening) {
    if (!listen(host, port)) {
        return false;
    }
    if (on_listening) {
        on_listening();
    }
    wait();
    return true;
}

bool http_server_base::start_unix(const std::string& unix_path) {
    return start_unix(unix_path, nullptr);
}

bool http_server_base::start_unix(const std::string& unix_path, const std::function<void()> &on_listening) {
    if (!listen_unix(unix_path)) {
        return false;
    }
    if (on_listening) {
        on_listening();
    }
    wait();
    return true;
}

bool http_server_base::stop() {
    if (socket_server_) {
        bool result = socket_server_->stop();
        socket_server_.reset();
        return result;
    }
    return false;
}

bool http_server_base::is_listening() const {
    return socket_server_ != nullptr && socket_server_->is_running();
}

uint16_t http_server_base::local_port() const {
    return socket_server_ ? socket_server_->local_port() : 0;
}


// Private methods
void http_server_base::setup_connection_handler() {
    socket_server_->set_handler([this](std::shared_ptr<asio::socket> socket) {
        // Create HTTP connection
        auto connection = std::make_shared<server_connection>(socket);

        // Set request handler — awaitable coroutine with three-way dispatch
        connection->set_handler([this](std::shared_ptr<request> req) -> awaitable<void> {
            auto http_connection = req->get_http_connection();
            auto stream = req->get_http_stream();
            auto http_request = req->get_http_request();

            if (!http_connection || !stream) {
                LOG_ERROR("Invalid connection or stream");
                co_return;
            }

            // 1. Match route
            auto* matched_route = router_.find_route(req);

            // 2. Run middlewares (before reading the body), sharing the handler response
            response res(http_connection, stream, http_request, cors_enabled_);

            if (!co_await run_middlewares(*req, res)) {
                co_await discard_unread_body(*req);
                co_return;
            }

            // 3. Three-way dispatch
            if (!matched_route) {
                // No route matched → fallback / 404
                router_.handle_unmatched(req, res);
            } else if (matched_route->is_deferred_body()) {
                // DEFERRED: handler reads body at its discretion
                co_await matched_route->handle_request_coro(*req, res);
            } else if (req->has_pending_body()) {
                // PENDING BODY: check size limit, read, then dispatch
                if (!http_request->is_chunked_transfer() && req->content_length() > max_body_size_) {
                    // body left unread on the socket: close after responding
                    stream->set_keep_alive(false);
                    res.error(http_response::status::payload_too_large, "Payload Too Large");
                    co_return;
                }
                req->set_max_body_size(max_body_size_);
                bool ok = co_await req->read_body();
                if (!ok) {
                    stream->set_keep_alive(false);
                    res.error(http_response::status::payload_too_large, "Payload Too Large");
                    co_return;
                }
                co_await matched_route->handle_request_coro(*req, res);
            } else {
                // NO BODY: dispatch directly
                co_await matched_route->handle_request_coro(*req, res);
            }

            // Drop any body left unread (unmatched route, or a deferred handler that did
            // not read it all) so it is not parsed as the next request
            co_await discard_unread_body(*req);
        });

        // Start handling the connection with configured timeout
        connection->start(connection_timeout_);
    });
}

awaitable<bool> http_server_base::run_middlewares(request& req, response& res) {
    for (const auto& middleware : middlewares_) {
        bool next = false;
        try {
            next = co_await middleware(req, res);
        } catch (const std::exception& e) {
            LOG_ERROR("Exception in middleware: {}", e.what());
            if (!res.has_responded()) res.error(http_response::status::internal_server_error);
            co_return false;
        }

        if (res.has_responded()) co_return false;

        if (!next) {
            // stopped without responding: answer anyway so the stream is not left pending
            LOG_ERROR("Middleware stopped the request without sending a response");
            res.error(http_response::status::internal_server_error);
            co_return false;
        }
    }
    co_return true;
}

awaitable<void> http_server_base::discard_unread_body(request& req) {
    // A connection taken over belongs to its new owner, unread body included
    if (auto connection = req.get_http_connection(); connection && connection->is_taken_over()) {
        co_return;
    }

    // Consume a body nobody read so the connection can be reused for the next request,
    // or close it after the response if the body is too large or cannot be read.
    if (!co_await req.discard_body(max_body_size_)) {
        if (auto stream = req.get_http_stream()) {
            stream->set_keep_alive(false);
        }
    }
}

} // namespace thinger::http
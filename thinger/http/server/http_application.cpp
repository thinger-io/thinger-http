#include "http_application.hpp"
#include "request.hpp"
#include "response.hpp"
#include "memory_response.hpp"
#include "../../util/logger.hpp"
#include "../../util/base64.hpp"
#include "../../util/ascii.hpp"
#include <boost/algorithm/string.hpp>
#include <optional>

namespace thinger::http {

// Middleware
void http_application::use(async_middleware_function middleware) {
    middlewares_.push_back(std::move(middleware));
}

void http_application::use(middleware_function middleware) {
    use([middleware = std::move(middleware)](request& req, response& res) -> awaitable<bool> {
        // shared flag: a misbehaving middleware may still call next() after returning
        auto passed = std::make_shared<bool>(false);
        middleware(req, res, [passed]() { *passed = true; });
        co_return *passed;
    });
}

// Basic Auth helpers
void http_application::set_basic_auth(const std::string& path_prefix, 
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
            res.header("WWW-Authenticate", "Basic realm=\"" + realm + "\"");
            res.error(http_response::status::unauthorized, "Authentication required");
            co_return false;
        }
        
        auto auth_header = http_request->get_header("Authorization");
        if (!auth_header.starts_with("Basic ")) {
            res.header("WWW-Authenticate", "Basic realm=\"" + realm + "\"");
            res.error(http_response::status::unauthorized, "Invalid authentication");
            co_return false;
        }
        
        // Decode base64 credentials
        auto encoded = auth_header.substr(6);
        std::string decoded;
        try {
            decoded = ::thinger::util::base64::decode(encoded);
        } catch (...) {
            res.error(http_response::status::unauthorized, "Invalid credentials format");
            co_return false;
        }
        
        // Parse username:password
        auto colon_pos = decoded.find(':');
        if (colon_pos == std::string::npos) {
            res.error(http_response::status::unauthorized, "Invalid credentials format");
            co_return false;
        }
        
        std::string username = decoded.substr(0, colon_pos);
        std::string password = decoded.substr(colon_pos + 1);
        
        // Verify credentials
        if (verify(username, password)) {
            req.set_auth_user(username);
            co_return true;
        }

        res.header("WWW-Authenticate", "Basic realm=\"" + realm + "\"");
        res.error(http_response::status::unauthorized, "Invalid username or password");
        co_return false;
    });
}

void http_application::set_basic_auth(const std::string& path_prefix,
                                 const std::string& realm,
                                 const std::string& username,
                                 const std::string& password) {
    set_basic_auth(path_prefix, realm, 
        [username, password](const std::string& u, const std::string& p) {
            return u == username && p == password;
        });
}

void http_application::set_basic_auth(const std::string& path_prefix,
                                 const std::string& realm,
                                 const std::map<std::string, std::string>& users) {
    set_basic_auth(path_prefix, realm,
        [users](const std::string& u, const std::string& p) {
            auto it = users.find(u);
            return it != users.end() && it->second == p;
        });
}

// Virtual hosts
virtual_host& http_application::host(const std::string& name) {
    if (name.empty() || name == "*") return *this;

    if (!virtual_host::is_host_pattern(name)) {
        auto key = virtual_host::normalize_host(name);
        auto& host = exact_hosts_[key];
        if (!host) host = std::make_unique<virtual_host>(key);
        return *host;
    }

    // Registering the same pattern again returns the existing host
    auto pattern = name;
    ::thinger::util::ascii::to_lower(pattern);
    for (const auto& host : pattern_hosts_) {
        if (host->name() == pattern) return *host;
    }
    return *pattern_hosts_.emplace_back(std::make_unique<virtual_host>(pattern));
}

virtual_host& http_application::host_regex(const std::string& pattern) {
    return *pattern_hosts_.emplace_back(
        std::make_unique<virtual_host>(pattern, std::regex(pattern, std::regex::icase)));
}

virtual_host& http_application::resolve_host(request& req) {
    // Without virtual hosts, every request is served by the application
    if (exact_hosts_.empty() && pattern_hosts_.empty()) {
        req.set_virtual_host(this);
        return *this;
    }

    auto http_request = req.get_http_request();
    const auto& host_header = http_request->get_header(header::host);
    auto name = virtual_host::normalize_host(host_header.empty() ? http_request->get_host() : host_header);

    virtual_host* host = this;
    if (auto it = exact_hosts_.find(name); it != exact_hosts_.end()) {
        host = it->second.get();
    } else {
        for (const auto& candidate : pattern_hosts_) {
            std::smatch matches;
            if (!candidate->matches(name, matches)) continue;

            std::vector<std::string> captures;
            for (size_t i = 0; i < matches.size(); ++i) {
                captures.push_back(matches[i].str());
                const auto& parameters = candidate->get_parameters();
                if (i > 0 && i <= parameters.size() && !parameters[i - 1].empty() && matches[i].matched) {
                    req.set_uri_parameter(parameters[i - 1], matches[i].str());
                }
            }
            req.set_host_matches(std::move(captures));
            host = candidate.get();
            break;
        }
    }

    req.set_virtual_host(host);
    return *host;
}

// Error format
void http_application::set_error_formatter(error_formatter formatter) {
    error_formatter_ = formatter ? std::make_shared<const error_formatter>(std::move(formatter)) : nullptr;
}

std::shared_ptr<http_response> http_application::make_error_response(http_response::status status,
                                                                     const std::string& message) const {
    auto result = std::make_shared<http_response>();
    result->set_status(status);
    format_error(error_formatter_.get(), {status, message, nullptr}, *result);
    return result;
}

// Trusted proxies
bool http_application::set_trusted_proxies(const std::vector<std::string>& proxies, forwarded_header header) {
    auto trusted = std::make_shared<trusted_proxies>(header);
    for (const auto& proxy : proxies) {
        if (!trusted->add(proxy)) {
            LOG_ERROR("Invalid trusted proxy: '{}'", proxy);
            trusted_proxies_.reset();
            return false;
        }
    }
    trusted_proxies_ = trusted->empty() ? nullptr : std::move(trusted);
    return true;
}

// OpenAPI
route& http_application::serve_openapi(const std::string& path) {
    return get(path, [this](response& res) {
        res.json(openapi_.generate());
    }).hidden();
}

// Configuration
void http_application::enable_cors(bool enabled) {
    cors_enabled_ = enabled;
}

void http_application::set_max_body_size(size_t size) {
    max_body_size_ = size;
}

// Request processing
// The connection cannot be reused after this request (unread or broken body, failed
// handler...): close it once the response is sent. The response, unless already
// prepared, also tells the client.
static void close_after_response(request& req) {
    if (auto stream = req.get_http_stream()) stream->set_keep_alive(false);
    if (auto http_request = req.get_http_request()) http_request->set_keep_alive(false);
}

awaitable<void> http_application::handle(std::shared_ptr<request> req, std::shared_ptr<response_sink> sink) {
    response res(std::move(sink), req->get_http_request(), cors_enabled_);
    auto http_request = req->get_http_request();

    // Server settings the request and its response depend on
    res.set_error_formatter(error_formatter_);
    req->set_trusted_proxies(trusted_proxies_);
    req->set_max_body_size(max_body_size_);

    // Ambiguous body framing (invalid Content-Length, unsupported Transfer-Encoding, or
    // both): where this request ends, and so where the next one starts, is unknown
    if (!http_request->has_valid_framing()) {
        LOG_WARNING("Rejecting request with invalid body framing: {} {}",
                    get_method(http_request->get_method()), http_request->get_path());
        close_after_response(*req);
        res.error(http_response::status::bad_request, "Invalid request framing");
        co_return;
    }

    // Route matching, middlewares, body reading and handler; any exception is answered
    // with 500 (or, if the response was already started, the connection is closed)
    std::optional<std::string> failure;
    try {
        co_await handle_request(req, res);
    } catch (const std::exception& e) {
        failure = e.what();
    } catch (...) {
        failure = "unknown exception";
    }
    if (failure) {
        LOG_ERROR("Exception handling {} {}: {}", get_method(http_request->get_method()),
                  http_request->get_path(), *failure);
        if (res.has_responded()) {
            // The response may be incomplete (e.g. a chunked response): do not reuse the connection
            close_after_response(*req);
        } else {
            res.error(http_response::status::internal_server_error);
        }
    }

    // A body that could not be read (too large, malformed, truncated) fails the request,
    // and leaves the connection at an unknown position
    if (auto error = req->get_body_error(); error != body_error::none) {
        close_after_response(*req);
        if (!res.has_responded()) {
            if (error == body_error::too_large) {
                res.error(http_response::status::payload_too_large, "Payload Too Large");
            } else {
                res.error(http_response::status::bad_request, "Invalid request body");
            }
        }
    }

    // Drop any body left unread (unmatched route, or a handler that did not read it all)
    // so it is not parsed as the next request
    if (req->has_pending_body()) co_await discard_unread_body(*req);
}

awaitable<void> http_application::handle_request(std::shared_ptr<request> req, response& res) {
    // 1. Match the virtual host (by the Host header), then the route among its routes
    auto& host = resolve_host(*req);
    auto* matched_route = host.router().find_route(req);

    // 2. Run middlewares (before reading the body), sharing the handler response
    if (!middlewares_.empty() && !co_await run_middlewares(*req, res)) co_return;

    // 3. Three-way dispatch
    if (!matched_route) {
        // No route matched → fallback / 404
        co_await host.router().handle_unmatched(req, res);
    } else if (matched_route->is_deferred_body() || !req->has_pending_body()) {
        // DEFERRED: handler reads body at its discretion (bounded by the maximum body size
        // in read_body()); or NO BODY: dispatch directly
        co_await matched_route->handle_request_coro(*req, res);
    } else if (co_await req->read_body()) {
        // PENDING BODY: read (up to the maximum body size, or failing the request), then dispatch
        co_await matched_route->handle_request_coro(*req, res);
    }
}

// In-memory dispatch
awaitable<std::shared_ptr<http_response>> http_application::dispatch(std::shared_ptr<http_request> http_request,
                                                                     dispatch_options options) {
    if (!http_request) co_return make_error_response(http_response::status::bad_request, "Missing request");
    if (http_request->is_chunked_transfer()) {
        co_return make_error_response(http_response::status::bad_request, "Chunked requests cannot be dispatched in memory");
    }

    // Serve the body from memory, as if just received from the client: handlers read it
    // like on a connection (read_body(), or read()/read_some() on deferred routes)
    std::string body = std::move(http_request->get_body());
    http_request->get_body().clear();
    http_request->remove_headers(header::content_length);
    http_request->process_header(header::content_length, std::to_string(body.size()));

    auto req = std::make_shared<request>(nullptr, nullptr, http_request);
    req->set_peer_ip(options.remote_ip);
    if (!body.empty()) {
        req->set_read_ahead(reinterpret_cast<const uint8_t*>(body.data()), body.size());
    }

    auto executor = co_await boost::asio::this_coro::executor;
    auto memory = std::make_shared<memory_response>(executor);

    // Run the request on its own coroutine (in a strand, so it can be cancelled safely),
    // and wait for the response from the start: the timeout also covers a handler that
    // never finishes. Handlers may also answer later, from a copy of the response.
    // handle() answers whatever the middlewares, body reading or handler throw.
    auto strand = boost::asio::make_strand(executor);
    auto cancel = std::make_shared<boost::asio::cancellation_signal>();
    // (the coroutine keeps the cancellation signal alive while its slot is in use)
    co_spawn(strand,
        [this, req, memory, cancel]() -> awaitable<void> {
            co_await handle(req, memory);
        },
        boost::asio::bind_cancellation_slot(cancel->slot(), boost::asio::detached));

    auto result = co_await memory->wait(options.timeout);
    if (!result) {
        // Abort what the handler is waiting for (timers, sockets...)
        boost::asio::post(strand, [cancel]() { cancel->emit(boost::asio::cancellation_type::terminal); });
        co_return make_error_response(http_response::status::gateway_timeout, "No response within the timeout");
    }
    co_return result;
}

void http_application::dispatch(const boost::asio::any_io_executor& executor,
                                std::shared_ptr<http_request> request,
                                std::function<void(std::shared_ptr<http_response>)> callback,
                                dispatch_options options) {
    co_spawn(executor, dispatch(std::move(request), std::move(options)),
        [this, callback = std::move(callback)](std::exception_ptr error, std::shared_ptr<http_response> result) {
            if (error || !result) {
                result = make_error_response(http_response::status::internal_server_error, "");
            }
            callback(std::move(result));
        });
}

awaitable<bool> http_application::run_middlewares(request& req, response& res) {
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

awaitable<void> http_application::discard_unread_body(request& req) {
    // A connection taken over belongs to its new owner, unread body included
    if (auto stream = req.get_http_stream(); stream && stream->takes_over()) {
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

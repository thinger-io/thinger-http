#include "http_application.hpp"
#include "request.hpp"
#include "response.hpp"
#include "memory_response.hpp"
#include "../../asio/async_event.hpp"
#include "../../util/logger.hpp"
#include "../../util/base64.hpp"
#include "../../util/ascii.hpp"
#include <boost/algorithm/string.hpp>
#include <mutex>
#include <optional>

namespace thinger::http {

// What serving a request reads: the hosts and their routes, the middlewares and the settings of
// the application. Its connections keep it (see connection_handler()), so the requests still
// running when a server is destroyed find it. It never refers to the application
class http_application::state : public std::enable_shared_from_this<state> {
public:
    explicit state(std::unique_ptr<virtual_host> default_host) : default_host_(std::move(default_host)) {}

    // handle(), telling `created` the response once created (for in-memory dispatch, which
    // answers its timeout)
    awaitable<void> handle(std::shared_ptr<request> req, std::shared_ptr<response_sink> sink,
                           std::function<void(const response&)> created);
    virtual_host& resolve_host(request& req);
    std::shared_ptr<http_response> make_error_response(http_response::status status, const std::string& message) const;
    // Middlewares, body reading and handler of a matched request, within the cancellation scope
    // of its stream (on a coroutine of their own if none is bound to it, see
    // http_stream::bind_handler()), then what is left of its body
    awaitable<void> serve(std::shared_ptr<request> req, response& res, virtual_host& host, const route* matched_route);
    // The same, within the timeout of the route (or of the host)
    awaitable<void> serve_with_timeout(std::shared_ptr<request> req, response res, virtual_host& host,
                                       const route* matched_route, std::chrono::milliseconds timeout,
                                       http_response::status status);
    awaitable<bool> run_middlewares(request& req, response& res);
    awaitable<void> discard_unread_body(request& req);

    // The application as the default host: its settings, shared with the application
    std::unique_ptr<virtual_host> default_host_;

    // Body decoders, accepted types and limits (before openapi_, which documents them)
    body_settings body_settings_;
    openapi_generator openapi_{default_host_->router(), &body_settings_};
    std::vector<async_middleware_function> middlewares_;
    bool cors_enabled_{false};

    // Maximum allowed request body size
    size_t max_body_size_{default_max_body_size};

    // Virtual hosts by exact name, and by pattern (in registration order)
    std::map<std::string, std::unique_ptr<virtual_host>> exact_hosts_;
    std::vector<std::unique_ptr<virtual_host>> pattern_hosts_;

    // Body of error responses (default format if null)
    std::shared_ptr<const error_formatter> error_formatter_;

    // Proxies allowed to report the client address (none if null)
    std::shared_ptr<const trusted_proxies> trusted_proxies_;

    // Called with every response (none if null)
    std::shared_ptr<const response_hook> response_hook_;
};

http_application::http_application()
    : state_(std::make_shared<state>(std::unique_ptr<virtual_host>(new virtual_host(settings_)))) {}

std::function<awaitable<void>(std::shared_ptr<request>)> http_application::connection_handler() const {
    return [state = state_](std::shared_ptr<request> req) {
        auto sink = std::make_shared<connection_sink>(*req);
        return state->handle(std::move(req), std::move(sink), nullptr);
    };
}

// Middleware
void http_application::use(async_middleware_function middleware) {
    state_->middlewares_.push_back(std::move(middleware));
}

void http_application::use(middleware_function middleware) {
    use([middleware = std::move(middleware)](request& req, response& res) -> awaitable<bool> {
        // shared flag: a misbehaving middleware may still call next() after returning
        auto passed = std::make_shared<bool>(false);
        middleware(req, res, [passed]() { *passed = true; });
        co_return *passed;
    });
}

void http_application::use(coroutine_middleware_function middleware) {
    use([middleware = std::move(middleware)](request& req, response& res) -> awaitable<bool> {
        auto passed = std::make_shared<bool>(false);
        co_await middleware(req, res, [passed]() { *passed = true; });
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
        auto& host = state_->exact_hosts_[key];
        if (!host) host = std::make_unique<virtual_host>(key);
        return *host;
    }

    // Registering the same pattern again returns the existing host
    auto pattern = name;
    ::thinger::util::ascii::to_lower(pattern);
    for (const auto& host : state_->pattern_hosts_) {
        if (host->name() == pattern) return *host;
    }
    return *state_->pattern_hosts_.emplace_back(std::make_unique<virtual_host>(pattern));
}

virtual_host& http_application::host_regex(const std::string& pattern) {
    return *state_->pattern_hosts_.emplace_back(
        std::make_unique<virtual_host>(pattern, std::regex(pattern, std::regex::icase)));
}

virtual_host& http_application::state::resolve_host(request& req) {
    // Without virtual hosts, every request is served by the application
    if (exact_hosts_.empty() && pattern_hosts_.empty()) {
        req.set_virtual_host(default_host_.get());
        return *default_host_;
    }

    auto http_request = req.get_http_request();
    const auto& host_header = http_request->get_header(header::host);
    auto name = virtual_host::normalize_host(host_header.empty() ? http_request->get_host() : host_header);

    virtual_host* host = default_host_.get();
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
    state_->error_formatter_ = formatter ? std::make_shared<const error_formatter>(std::move(formatter)) : nullptr;
}

std::shared_ptr<http_response> http_application::state::make_error_response(http_response::status status,
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
            state_->trusted_proxies_.reset();
            return false;
        }
    }
    state_->trusted_proxies_ = trusted->empty() ? nullptr : std::move(trusted);
    return true;
}

// Response hook
void http_application::on_response(response_hook hook) {
    state_->response_hook_ = hook ? std::make_shared<const response_hook>(std::move(hook)) : nullptr;
}

// OpenAPI
route& http_application::serve_openapi(const std::string& path) {
    // The generator outlives the route: both are the state's
    return get(path, [openapi = &state_->openapi_](response& res) {
        res.json(openapi->generate());
    }).hidden();
}

openapi_generator& http_application::openapi() {
    return state_->openapi_;
}

// Configuration
void http_application::enable_cors(bool enabled) {
    state_->cors_enabled_ = enabled;
}

void http_application::set_max_body_size(size_t size) {
    state_->max_body_size_ = size;
}

size_t http_application::get_max_body_size() const {
    return state_->max_body_size_;
}

body_decoder_registry& http_application::body_decoders() {
    return state_->body_settings_.decoders;
}

const std::vector<std::string>& http_application::get_body_types() const {
    return state_->body_settings_.accepted;
}

void http_application::set_body_limits(body_limits limits) {
    state_->body_settings_.limits = limits;
}

const body_limits& http_application::get_body_limits() const {
    return state_->body_settings_.limits;
}

const body_settings& http_application::get_body_settings() const {
    return state_->body_settings_;
}

void http_application::accept_body_types(std::vector<std::string> types) {
    for (auto& type : types) ::thinger::util::ascii::to_lower(type);
    state_->body_settings_.accepted = std::move(types);
}

// Request processing
// The connection cannot be reused after this request (unread or broken body, failed
// handler...): close it once the response is sent, for `reason`. The head of the response,
// unless already written, also tells the client (see http_stream::must_close())
static void must_close(request& req, detail::close_reason reason) {
    if (auto stream = req.get_http_stream()) stream->must_close(reason);
}

// An expectation other than 100-continue cannot be met (RFC 9110 section 10.1.1): 417, as
// Node and Go answer. A Host its TLS handshake (SNI) was not made for: 421, as nginx answers,
// before its host runs anything (see request::misdirected()). Their body, sent or not, is
// not read: the connection is not reused then. True if the request was answered so
// (`head` is its http_request, which the caller holds already)
static bool refused_before_routing(request& req, const http_request& head, response& res) {
    bool unmet = head.get_expectation() == http_request::expectation::unsupported;
    if (!unmet && !req.misdirected()) [[likely]] return false;
    if (req.has_pending_body()) must_close(req, detail::close_reason::body_unread);
    if (unmet) res.error(http_response::status::expectation_failed, "Expectation Failed");
    else res.error(http_response::status::misdirected_request, "Misdirected Request");
    return true;
}

// The limits of the matched route apply to its request: its body size, else that of its host
// (which also applies to its not-found handler), else the application's; and the reasons that
// interrupt its handler (none without a route)
static void apply_route_limits(request& req, const virtual_host& host, const route* matched_route) {
    const auto& limit = matched_route && matched_route->get_max_body_size() ? matched_route->get_max_body_size()
                                                                           : host.get_max_body_size();
    if (limit) req.set_max_body_size(*limit);
    if (auto stream = req.get_http_stream(); stream && matched_route) {
        stream->set_cancel_on(matched_route->get_cancel_on());
    }
}

// The request deadline of the matched route (or its group), else that of its host: none if
// neither sets one (that of the connection applies)
static const std::optional<std::chrono::milliseconds>& request_deadline_of(const virtual_host& host,
                                                                          const route* matched_route) {
    return matched_route && matched_route->get_request_timeout() ? matched_route->get_request_timeout()
                                                                 : host.get_request_timeout();
}

// After the handler: a failed one gets a 500 unless its response was started (then the
// connection is not reused, see serve()); an unreadable body fails the request, at an
// unknown position
static void answer_failures(request& req, response& res, bool handler_failed) {
    if (handler_failed && !res.has_responded()) res.error(http_response::status::internal_server_error);
    if (auto error = req.get_body_error(); error != body_error::none) {
        must_close(req, detail::close_reason::body_error);
        if (!res.has_responded()) {
            if (error == body_error::too_large) {
                res.error(http_response::status::payload_too_large, "Payload Too Large");
            } else {
                res.error(http_response::status::bad_request, "Invalid request body");
            }
        }
    }
}

// How serve() dispatches a request once the middlewares let it through
enum class dispatch_kind { none, unmatched, handle, read_then_handle };

static dispatch_kind dispatch_of(request& req, response& res, const route* matched_route) {
    // No route matched → fallback / 404
    if (!matched_route) return dispatch_kind::unmatched;
    // UNSUPPORTED TYPE: answered 415 before reading the body (dropped afterwards)
    if (req.has_pending_body() && !matched_route->accepts_body(req, res)) return dispatch_kind::none;
    // DEFERRED: handler reads body at its discretion (bounded by the maximum body size in every
    // read); or NO BODY: dispatch directly
    if (matched_route->is_deferred_body() || !req.has_pending_body()) return dispatch_kind::handle;
    // PENDING BODY: read (up to the maximum body size, or failing the request), then dispatch
    return dispatch_kind::read_then_handle;
}

// What a handler failed with, for the log
static std::string describe(const std::exception_ptr& error) {
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "unknown exception";
    }
}

// A failed handler may have left the response it started incomplete (unless the request timeout
// answered it, `timed_out`): the connection is not reused then, closed as aborted if the scope
// interrupted it (see serve())
static void close_if_incomplete(request& req, const response& res, const http_stream* stream, bool failed,
                                bool timed_out) {
    if (failed && res.has_responded() && !timed_out) {
        must_close(req, stream && stream->handler_interrupted() ? detail::close_reason::aborted
                                                                : detail::close_reason::handler_failed);
    }
}

// Once the handler ended (see serve()): what is logged and answered for it, and whether the body
// left unread is to be discarded. `timed_out`: the request timeout answered it
static bool settle(request& req, response& res, const http_stream* stream,
                   const std::optional<std::string>& failure, bool timed_out) {
    auto http_request = req.get_http_request();

    // Interrupted by the cancellation of its scope (see route::cancel_on): not a failure, and
    // nothing more is awaited for it. No 500: the timeout already answered, the client is
    // gone, or the connection closes. A response it started may be incomplete
    if (stream && stream->handler_interrupted()) [[unlikely]] {
        LOG_DEBUG("Handler interrupted: {} {}{}", get_method(http_request->get_method()), http_request->get_path(),
                  failure ? " (" + *failure + ")" : "");
        if (res.has_responded() && !timed_out) must_close(req, detail::close_reason::aborted);
        if (req.has_pending_body()) must_close(req, detail::close_reason::body_unread);
        return false;
    }

    if (failure) {
        LOG_ERROR("Exception handling {} {}: {}", get_method(http_request->get_method()),
                  http_request->get_path(), *failure);
    }

    // Answered by the request timeout, which already decided what happens to the connection:
    // the handler is just late
    if (timed_out) return false;
    answer_failures(req, res, failure.has_value());
    return req.has_pending_body();
}

// An in-memory dispatch has no coroutine bound to the handler scope of its stream yet: `served`
// runs on one of its own, bound to the handler signal of the stream, which is emitted from now
// on. What it failed with if interrupted before its handler started, nothing once it ran
static awaitable<std::optional<std::string>> serve_on_own_coroutine(http_stream& stream,
                                                                    awaitable<void> served) {
    stream.bind_handler();
    stream.handler_started();
    std::optional<std::string> failure;
    try {
        co_await co_spawn(co_await boost::asio::this_coro::executor, std::move(served),
                          boost::asio::bind_cancellation_slot(stream.handler_slot(), boost::asio::use_awaitable));
        co_return std::nullopt;
    } catch (const std::exception& e) {
        // Thrown once its handler ended: as if from serve()
        if (!stream.handler_running()) throw;
        // Interrupted before it started
        failure = e.what();
    }
    stream.handler_ended();
    co_return failure;
}

namespace {

    // Tells the observer of a response (if any) that the application is done with its request,
    // however handle() ends
    struct handled_guard {
        std::shared_ptr<response_observer> observer;
        ~handled_guard() {
            if (observer) observer->handled();
        }
    };

}

awaitable<void> http_application::handle(std::shared_ptr<request> req, std::shared_ptr<response_sink> sink) {
    // Its coroutine, not one more around it
    return state_->handle(std::move(req), std::move(sink), nullptr);
}

awaitable<void> http_application::state::handle(std::shared_ptr<request> req, std::shared_ptr<response_sink> sink,
                                                std::function<void(const response&)> created) {
    // Told when the response ends, while a response hook is set
    handled_guard handled;
    if (response_hook_) [[unlikely]] {
        handled.observer = std::make_shared<response_observer>(req, response_hook_);
        sink->observe(handled.observer);
    }

    response res(std::move(sink), req->get_http_request(), cors_enabled_);
    auto http_request = req->get_http_request();

    // Server settings the request and its response depend on
    res.set_error_formatter(error_formatter_);
    if (created) created(res);
    req->set_trusted_proxies(trusted_proxies_);
    req->set_max_body_size(max_body_size_);
    req->set_body_settings(&body_settings_);

    // Ambiguous body framing (invalid Content-Length, unsupported Transfer-Encoding, or
    // both): where this request ends, and so where the next one starts, is unknown
    if (!http_request->has_valid_framing()) {
        LOG_WARNING("Rejecting request with invalid body framing: {} {}",
                    get_method(http_request->get_method()), http_request->get_path());
        must_close(*req, detail::close_reason::body_error);
        res.error(http_response::status::bad_request, "Invalid request framing");
        co_return;
    }

    if (refused_before_routing(*req, *http_request, res)) [[unlikely]] co_return;

    // Match the virtual host (by the Host header), then the route among its routes, whose
    // limits apply from here
    virtual_host* host = default_host_.get();
    const route* matched_route = nullptr;
    try {
        host = &resolve_host(*req);
        matched_route = host->router().find_route(req);
    } catch (const std::exception& e) {
        LOG_ERROR("Exception matching {} {}: {}", get_method(http_request->get_method()), http_request->get_path(), e.what());
        must_close(*req, detail::close_reason::handler_failed);
        res.error(http_response::status::internal_server_error);
        co_return;
    }
    apply_route_limits(*req, *host, matched_route);
    // Measured by the connection, from the start of the request
    if (const auto& deadline = request_deadline_of(*host, matched_route); deadline && req->source_) {
        req->source_->set_request_deadline(*deadline);
    }
    bool route_timeout = matched_route && matched_route->get_timeout();
    const auto& timeout = route_timeout ? matched_route->get_timeout() : host->get_timeout();
    if (timeout && timeout->count() > 0) {
        auto status = route_timeout ? matched_route->get_timeout_status() : host->get_timeout_status();
        co_await serve_with_timeout(std::move(req), std::move(res), *host, matched_route, *timeout, status);
        co_return;
    }
    co_await serve(std::move(req), res, *host, matched_route);
}

awaitable<void> http_application::state::serve(std::shared_ptr<request> req, response& res, virtual_host& host,
                                               const route* matched_route) {
    // The handler runs within the cancellation scope of its stream, on a coroutine bound to it
    // (the only binding of the scope, see route::cancel_on): this one, bound already (the read
    // loop of the connection, or the coroutine of a route timeout), with no coroutine more per
    // request. Without a stream there is no scope
    auto stream = req->get_http_stream();
    std::optional<std::string> failure;
    if (stream && !stream->handler_bound()) [[unlikely]] {
        failure = co_await serve_on_own_coroutine(*stream, serve(req, res, host, matched_route));
        if (!failure) co_return;
    } else {
        // The signals of the scope are emitted only while the handler runs (on this executor)
        if (stream) stream->handler_started();
        // Any exception is answered below with 500 unless the scope interrupted the handler
        try {
            if (!middlewares_.empty() && !co_await run_middlewares(*req, res)) {
                // STOPPED by a middleware, which answered (before reading the body)
            } else if (auto how = dispatch_of(*req, res, matched_route); how == dispatch_kind::unmatched) {
                co_await host.router().handle_unmatched(req, res);
            } else if (how == dispatch_kind::handle ||
                       (how == dispatch_kind::read_then_handle && co_await req->read_body())) {
                co_await matched_route->handle_request_coro(*req, res);
            }
        } catch (...) {
            failure = describe(std::current_exception());
        }

        // A response it started may be incomplete: the connection closes after it, decided now,
        // before its head may be written (this coroutine ends after a post)
        close_if_incomplete(*req, res, stream.get(), failure.has_value(), res.timed_out());
        if (stream) {
            stream->handler_ended();
            // What the scope or the handler left in the cancellation state of this coroutine goes
            // with the handler (a cancellation, its own state or filter): the next request starts anew
            co_await boost::asio::this_coro::reset_cancellation_state();
            co_await boost::asio::this_coro::throw_if_cancelled(true);
        }
    }

    // Drop any body left unread (unmatched route, or a handler that did not read it all)
    // so it is not parsed as the next request
    if (settle(*req, res, stream.get(), failure, res.timed_out())) co_await discard_unread_body(*req);
}

namespace {

    // Request timeout of a route (see route::timeout), shared by the request, its timer and
    // its response. Everything happens on the executor of the request.
    struct request_deadline {
        explicit request_deadline(const boost::asio::any_io_executor& executor)
            : timer(executor), wake(executor) {}

        // Expires at the deadline, unless cancelled once the response starts
        boost::asio::steady_timer timer;
        // Wakes the request once its handler ends, or it times out
        asio::async_event wake;
        bool served = false;
        bool expired = false;
    };

}

awaitable<void> http_application::state::serve_with_timeout(std::shared_ptr<request> req, response res,
                                                     virtual_host& host, const route* matched_route,
                                                     std::chrono::milliseconds timeout,
                                                     http_response::status status) {
    auto executor = co_await boost::asio::this_coro::executor;
    auto deadline = std::make_shared<request_deadline>(executor);

    // The timer stops once the response starts, whatever thread starts it
    res.on_start([weak = std::weak_ptr<request_deadline>(deadline)] {
        auto state = weak.lock();
        if (!state) return;
        boost::asio::dispatch(state->timer.get_executor(), [state] { state->timer.cancel(); });
    });

    // The connection timeout does not close the connection while the request waits for
    // its response within its own timeout
    if (req->source_) req->source_->hold_until(std::chrono::steady_clock::now() + timeout);

    // On expiry, the request is answered with an error. A body still unread may be being
    // read by the handler, which goes on: the connection closes after the response
    deadline->timer.expires_after(timeout);
    deadline->timer.async_wait([deadline, req, res, status](const boost::system::error_code& ec) mutable {
        if (ec) return;
        if (!res.has_responded()) {
            bool pending_body = req->has_pending_body();
            if (pending_body) must_close(*req, detail::close_reason::route_timeout);
            if (res.answer_timeout(status)) {
                auto http_request = req->get_http_request();
                LOG_WARNING("Request timed out: {} {}{}", get_method(http_request->get_method()),
                            http_request->get_path(), pending_body ? " (body not read, closing the connection)" : "");
                deadline->expired = true;
                deadline->wake.set();
            }
        }
        // Then the scope of the request is cancelled, after the answer, also when the response
        // started (on another thread) as the timer expired: its handler goes on unless its
        // route declared the timeout (see route::cancel_on)
        if (auto stream = req->get_http_stream()) stream->cancel(cancel_reason::timeout);
    });

    // The handler runs on its own coroutine, so the request can end before it does: it
    // keeps a copy of the response, and the request (with its cancellation scope, to which
    // the coroutine is bound)
    auto stream = req->get_http_stream();
    if (stream) stream->bind_handler();
    co_spawn(executor,
        [self = shared_from_this(), req, res, &host, matched_route]() mutable -> awaitable<void> {
            co_await self->serve(req, res, host, matched_route);
        },
        // Its end wakes the request (serve() answers what the handler throws)
        boost::asio::bind_cancellation_slot(stream ? stream->handler_slot() : boost::asio::cancellation_slot{},
                                            [deadline](const std::exception_ptr&) {
                                                deadline->served = true;
                                                deadline->wake.set();
                                            }));

    // Wait until the handler ends (a response it sends later is still awaited by the
    // connection, and the timer is still armed), or the request times out
    while (!deadline->served && !deadline->expired) {
        if (!co_await deadline->wake.wait()) co_return;
    }
}

// In-memory dispatch
awaitable<std::shared_ptr<http_response>> http_application::dispatch(std::shared_ptr<http_request> http_request,
                                                                     dispatch_options options) {
    if (!http_request) co_return state_->make_error_response(http_response::status::bad_request, "Missing request");
    if (http_request->is_chunked_transfer()) {
        co_return state_->make_error_response(http_response::status::bad_request, "Chunked requests cannot be dispatched in memory");
    }

    // The body comes from memory (see detail::memory_source): handlers read it like on a
    // connection (read_body(), or read()/read_some() on deferred routes)
    std::string body = std::move(http_request->get_body());
    http_request->get_body().clear();
    http_request->remove_headers(header::content_length);
    http_request->process_header(header::content_length, std::to_string(body.size()));

    auto executor = co_await boost::asio::this_coro::executor;
    auto memory = std::make_shared<memory_response>(executor);

    // Run the request on its own coroutine (in a strand, where its stream, the cancellation
    // scope of the request, acts), and wait for the response from the start: the timeout
    // also covers a handler that never finishes. Handlers may also answer later, from a copy
    // of the response. handle() answers whatever the middlewares, body reading or handler throw.
    auto strand = boost::asio::make_strand(executor);
    auto stream = std::make_shared<http_stream>(1, *http_request, strand);
    auto req = std::make_shared<request>(
        std::make_unique<detail::memory_source>(std::move(body), std::move(options.remote_ip)), stream, http_request);
    req->context() = std::move(options.context);
    // The response, once created on the strand, to answer the timeout through it
    struct created_response {
        std::mutex mutex;
        std::optional<response> res;
    };
    auto created = std::make_shared<created_response>();
    co_spawn(strand,
        [state = state_, req, memory, created]() -> awaitable<void> {
            co_await state->handle(req, memory, [created](const response& res) {
                std::scoped_lock lock(created->mutex);
                created->res.emplace(res);
            });
        },
        // What handle() itself throws is a 500, at once (unless the response is complete)
        [state = state_, memory](const std::exception_ptr& error) {
            if (error) memory->send(state->make_error_response(http_response::status::internal_server_error, ""));
        });

    auto result = co_await memory->wait(options.timeout);
    if (!result && (co_await boost::asio::this_coro::cancellation_state).cancelled() !=
                       boost::asio::cancellation_type::none) {
        // The caller cancelled the dispatch: for the request, its client is gone (its handler
        // goes on unless its route declared it, see route::cancel_on)
        stream->cancel(cancel_reason::client_gone);
        throw boost::system::system_error(boost::asio::error::operation_aborted);
    }
    if (!result) {
        // Answered with the timeout as a route timeout does (a later answer is dropped, and
        // logged as late), without the headers the handler may still be setting on the strand;
        // unless an answer started first. Before the response exists, the 504 is collected alone
        std::optional<response> res;
        {
            std::scoped_lock lock(created->mutex);
            res = created->res;
        }
        if (res) {
            res->answer_timeout(http_response::status::gateway_timeout, false);
        } else {
            memory->send(state_->make_error_response(http_response::status::gateway_timeout, "No response within the timeout"));
        }
        // Then the scope of the request is cancelled, on the strand: its handler goes on unless
        // its route declared the timeout (see route::cancel_on)
        stream->cancel(cancel_reason::timeout);
        result = co_await memory->wait(std::chrono::milliseconds(0));
        // An answer that started first and is not complete
        if (!result) result = state_->make_error_response(http_response::status::gateway_timeout, "No response within the timeout");
    }
    co_return result;
}

void http_application::dispatch(const boost::asio::any_io_executor& executor,
                                std::shared_ptr<http_request> request,
                                sync_callback<void(std::shared_ptr<http_response>)> callback,
                                dispatch_options options) {
    co_spawn(executor, dispatch(std::move(request), std::move(options)),
        [state = state_, callback = std::move(callback)](std::exception_ptr error, std::shared_ptr<http_response> result) {
            if (error || !result) {
                result = state->make_error_response(http_response::status::internal_server_error, "");
            }
            callback(std::move(result));
        });
}

awaitable<bool> http_application::state::run_middlewares(request& req, response& res) {
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

awaitable<void> http_application::state::discard_unread_body(request& req) {
    auto stream = req.get_http_stream();
    // A connection taken over belongs to its new owner, unread body included
    if (stream && stream->takes_over()) {
        co_return;
    }

    // Not asked for (no 100 Continue sent), the body may never come: the connection is closed
    // after the response instead of waiting for it
    if (stream && (stream->get_continue() == http_stream::continue_state::pending ||
                   stream->get_continue() == http_stream::continue_state::skipped)) {
        must_close(req, detail::close_reason::continue_skipped);
        co_return;
    }

    // Consume a body nobody read so the connection can be reused for the next request,
    // or close it after the response if the body is too large or cannot be read.
    if (!co_await req.discard_body(max_body_size_)) {
        if (stream) stream->must_close(detail::close_reason::body_unread);
    }
}

} // namespace thinger::http

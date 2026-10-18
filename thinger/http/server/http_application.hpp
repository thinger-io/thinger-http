#ifndef THINGER_HTTP_APPLICATION_HPP
#define THINGER_HTTP_APPLICATION_HPP

#include "virtual_host.hpp"
#include "error_format.hpp"
#include "trusted_proxies.hpp"
#include "openapi.hpp"
#include "body_decoders.hpp"
#include "response_sink.hpp"
#include "response_observer.hpp"
#include "request_context.hpp"
#include "../../util/types.hpp"
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace thinger::http {

class request;
class response;

// Options for requests dispatched in memory (see http_application::dispatch)
struct dispatch_options {
    // Address of the peer, reported by request::get_peer_ip() and, unless it is a trusted
    // proxy (see set_trusted_proxies), by request::get_request_ip(). Empty by default:
    // never assume a trusted address such as 127.0.0.1 for internal calls. The scheme
    // reported by request::get_request_scheme() is the one of the request (https if its URL
    // is https://, see http_request::set_ssl()), unless this address is a trusted proxy.
    std::string remote_ip;

    // Maximum time to wait for the response, handler execution included; 504 Gateway
    // Timeout if exceeded (the handler is then cancelled: its pending awaits are aborted)
    std::chrono::milliseconds timeout{30000};

    // Initial values of the request context (see request::context()), seen by the
    // middlewares and the handler, e.g. a trace id; they may replace them. Empty by default:
    // nothing is carried over from the request being served by the caller.
    request_context context;
};

// Middlewares run after route matching (request::get_matched_route() is set, or null
// if no route matched) and before the request body is read, so they cannot see it.
// They share the response object with the route handler.

// Asynchronous middleware: co_return true to continue the chain, or respond through
// the response and co_return false to stop it (the route handler is not called).
using async_middleware_function = std::function<thinger::awaitable<bool>(request&, response&)>;

// Synchronous middleware: call next() before returning to continue the chain, or
// respond through the response to stop it. next() must not be called asynchronously.
using middleware_function = sync_callback<void(request&, response&, std::function<void()>)>;

// The same, as a coroutine (as an async Express middleware): call next() before it
// completes to continue the chain
using coroutine_middleware_function = std::function<thinger::awaitable<void>(request&, response&, std::function<void()>)>;

// What a server serves: the virtual hosts and their routes, the middlewares, and how
// requests are processed (error format, trusted proxies, limits). The application is the
// default virtual host: the routes registered on it (see virtual_host for the registration
// methods) answer every host without a virtual host of its own. A server (see
// http_server_base) is an application served on a listener; an application can also be
// used on its own, to dispatch requests in memory, without a connection.
class http_application : public virtual_host {
public:
    http_application() = default;

    // Virtual hosts: the routes registered on the application itself answer any host,
    // unless the Host header (case-insensitive, port ignored) matches a host registered
    // here: exact names first, then patterns in registration order. Register before serving
    // (listen()): hosts are matched without locks.
    // name: exact ("api.example.com"), "*" (this default host), or a pattern where "*" is
    // one label and ":name" a label available as a request parameter (see virtual_host)
    virtual_host& host(const std::string& name);

    // Host matched by a regular expression on the whole host name (case-insensitive);
    // its capture groups are available through request::get_host_matches()
    virtual_host& host_regex(const std::string& pattern);

    // OpenAPI document generated from the routes of the default host (configure title,
    // hooks... here). For another host: openapi_generator(app.host(name).router()).
    openapi_generator& openapi() { return openapi_; }

    // Serve the OpenAPI document as JSON at `path` (not served unless called)
    route& serve_openapi(const std::string& path = "/openapi.json");

    // Dispatch a request in memory, without a connection, through the same steps as a
    // network request: route matching, middlewares, body reading and validation, handler,
    // and the same 404/405 handling. Useful for tests, internal calls and gateways.
    // The body is taken from the request content (set_content); chunked requests are not
    // supported. WebSocket, SSE and connection takeover answer 501.
    awaitable<std::shared_ptr<http_response>> dispatch(std::shared_ptr<http_request> request,
                                                       dispatch_options options = {});

    // Same, calling `callback` with the response; runs on `executor`
    void dispatch(const boost::asio::any_io_executor& executor,
                  std::shared_ptr<http_request> request,
                  sync_callback<void(std::shared_ptr<http_response>)> callback,
                  dispatch_options options = {});

    // Handle a request received by a server, answering through `sink`
    awaitable<void> handle(std::shared_ptr<request> req, std::shared_ptr<response_sink> sink);

    // Middleware (register before serving, executed in registration order)
    void use(async_middleware_function middleware);
    void use(middleware_function middleware);
    void use(coroutine_middleware_function middleware);

    // Basic Auth helpers
    using auth_verify_function = std::function<bool(const std::string& username, const std::string& password)>;

    // Add basic auth for a specific path prefix
    void set_basic_auth(const std::string& path_prefix,
                        const std::string& realm,
                        auth_verify_function verify);

    // Add basic auth with simple username/password
    void set_basic_auth(const std::string& path_prefix,
                        const std::string& realm,
                        const std::string& username,
                        const std::string& password);

    // Add basic auth with multiple users
    void set_basic_auth(const std::string& path_prefix,
                        const std::string& realm,
                        const std::map<std::string, std::string>& users);

    // Add CORS headers to every response
    void enable_cors(bool enabled = true);

    // Maximum request body size (8 MB by default), for the routes that do not set their own
    // (route::max_body_size) and the requests matching no route: every read of a body is
    // bounded by it (413 if larger), and an unread body larger than this is not discarded
    // after the response: the connection is closed instead
    void set_max_body_size(size_t size);
    size_t get_max_body_size() const { return max_body_size_; }

    // Decoders of request bodies by media type, for the routes taking the body decoded:
    // JSON, MessagePack, CBOR, UBJSON, urlencoded forms, text and binary are built in (see
    // body_decoder_registry); remove or add decoders here. Set before serving.
    body_decoder_registry& body_decoders() { return body_settings_.decoders; }

    // Body types accepted by the routes taking the body decoded that do not set their own
    // (route::accepts): names of decoders or media types; {"json"} by default. Others are
    // answered with 415 Unsupported Media Type. Set before serving.
    void accept_body_types(std::vector<std::string> types);
    const std::vector<std::string>& get_body_types() const { return body_settings_.accepted; }

    // Limits of decoded bodies (nesting depth), for the routes that do not set their own
    // (route::decode_limits); set before serving
    void set_body_limits(body_limits limits) { body_settings_.limits = limits; }
    const body_limits& get_body_limits() const { return body_settings_.limits; }

    // Decoders, accepted types and limits together, e.g. for the openapi_generator of a
    // virtual host: openapi_generator(app.host(name).router(), &app.get_body_settings())
    const body_settings& get_body_settings() const { return body_settings_; }

    // Format of the error responses: those sent with response::error() and the ones the
    // server generates (400 invalid body or schema validation, 404, 405, 413, 415, 500...).
    // By default the message is sent as text/plain, and errors with details (schema
    // validation) as JSON {"error": {"message": ..., "context": [...]}}. Set it before
    // serving (listen()): it is read without locks, and may be called concurrently from the
    // worker threads.
    void set_error_formatter(error_formatter formatter);

    // Proxies (IPs or CIDR ranges: "10.0.0.1", "10.0.0.0/8", "fd00::/8") whose forwarding
    // header gives the client address reported by request::get_request_ip(), and the scheme
    // reported by request::get_request_scheme() (the X-Forwarded-Proto value aligned with the
    // X-Forwarded-For entry of the client, or its last value; the proto of the Forwarded
    // element of the client). The headers are ignored for any other peer, so only list proxies
    // that overwrite or append to them. None by default; set before serving (listen()): they
    // are read without locks. Returns false if an entry is invalid (then no proxy is trusted).
    bool set_trusted_proxies(const std::vector<std::string>& proxies,
                             forwarded_header header = forwarded_header::x_forwarded_for);

    // Called once per request, with its response (see response_info: status, bytes, duration,
    // and whether it completed, handed the connection over, or was aborted), once both the
    // response ended and the application is done with the request (its handler returned, or
    // its route timed out), e.g. for access logs, metrics or audit records. Every request the
    // application receives is reported, whoever answered it: the handler, a middleware, or the
    // server itself (400 invalid framing or body, 404, 405, 413, 415, 417, 500, a route
    // timeout...), on a connection or dispatched in memory. Not the requests a connection
    // cannot parse (400, 431), which never reach the application.
    //
    // Streamed responses (start_chunked(), start_stream()) are reported once their end is
    // written. Upgrades (WebSocket, SSE, take_over()) are reported once the response handing
    // the connection over is written, as taken_over: what happens on the connection afterwards
    // is not (see websocket_connection::on_close(), sse_connection::on_close()).
    //
    // It runs on the thread of the connection (for in-memory dispatch, the one ending the
    // request or its response), so it must not block: hand anything slow to another thread.
    // It may run synchronously within the call that ends the response, when the application
    // was already done (e.g. a late end_chunked() or abort_stream()), and, for a response that
    // never ended, from the destructor of what held it, as the connection being released (also
    // at the teardown of the io_context). The request is read-only there, and its connection
    // may be gone (its peer address is kept). What it throws is logged. Without a hook nothing
    // is allocated or measured. Set it before serving (listen()): it is read without locks.
    void on_response(response_hook hook);

private:
    // handle(), telling `created` the response once created (for in-memory dispatch, which
    // answers its timeout)
    awaitable<void> handle(std::shared_ptr<request> req, std::shared_ptr<response_sink> sink,
                           std::function<void(const response&)> created);
    virtual_host& resolve_host(request& req);
    std::shared_ptr<http_response> make_error_response(http_response::status status, const std::string& message) const;
    // Middlewares, body reading and handler of a matched request, then what is left of its body
    awaitable<void> serve(std::shared_ptr<request> req, response& res, virtual_host& host, const route* matched_route);
    // The same, within the timeout of the route (or of the host)
    awaitable<void> serve_with_timeout(std::shared_ptr<request> req, response res, virtual_host& host,
                                       const route* matched_route, std::chrono::milliseconds timeout,
                                       http_response::status status);
    awaitable<bool> run_middlewares(request& req, response& res);
    awaitable<void> discard_unread_body(request& req);

    // Body decoders, accepted types and limits (before openapi_, which documents them)
    body_settings body_settings_;
    openapi_generator openapi_{router_, &body_settings_};
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

} // namespace thinger::http

#endif // THINGER_HTTP_APPLICATION_HPP

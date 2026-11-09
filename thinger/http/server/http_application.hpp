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
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
    // Timeout if exceeded. The scope of the request is then cancelled with
    // cancel_reason::timeout: its handler goes on, and sees it in request::cancelled(), unless
    // its route declared the timeout (see route::cancel_on), which interrupts it. A caller
    // cancelling the dispatch cancels the scope with cancel_reason::client_gone instead
    std::chrono::milliseconds timeout{30000};

    // Initial values of the request context (see request::context()), seen by the
    // middlewares and the handler, e.g. a trace id; they may replace them. Empty by default:
    // nothing is carried over from the request being served by the caller.
    request_context context;
};

// Cross-origin requests (see http_application::enable_cors). The defaults are those of cors()
// in Express: any origin, without credentials, reflecting the headers a preflight asks for.
struct cors_options {
    // Origins allowed. {"*"} alone (the default) is any origin: every response says
    // "Access-Control-Allow-Origin: *", also to a request without Origin. Otherwise, the exact
    // origins allowed (ASCII case-insensitive; empty for none, "null" for that origin): the one
    // of a request listed is reflected as it came, and every response says "Vary: Origin",
    // allowed or not. "*" next to other origins is invalid.
    std::vector<std::string> origins{"*"};

    // Whether an origin is allowed, instead of the list, which is ignored then: reflected as
    // above if it returns true. Called once for each request with an Origin (repeated ones come
    // joined by ", "), on the thread of the request and before its middlewares, so it must not
    // block. If it throws, the request is answered 500, with no origin allowed.
    std::function<bool(std::string_view origin, const http_request& request)> origin_filter{};

    // Access-Control-Allow-Methods of a preflight
    std::vector<std::string> methods{"GET", "HEAD", "PUT", "PATCH", "POST", "DELETE"};

    // Access-Control-Allow-Headers of a preflight: without a value (the default), what its
    // Access-Control-Request-Headers asks for, with "Vary: Access-Control-Request-Headers";
    // with a value, that list, whatever it asks for (not sent if empty). Being optional, it is
    // set as std::vector<std::string>{"Content-Type", "Authorization"}.
    std::optional<std::vector<std::string>> allowed_headers{};

    // Access-Control-Expose-Headers, on the responses to an origin allowed (not sent if empty)
    std::vector<std::string> exposed_headers{};

    // Access-Control-Max-Age of a preflight: not sent without a value (the default), and sent
    // when 0. Not negative.
    std::optional<std::chrono::seconds> max_age{};

    // Send "Access-Control-Allow-Credentials: true" to an origin allowed (cookies or
    // Authorization in a browser). Invalid with any origin ("*"), which browsers reject: use a
    // list or a filter.
    bool credentials = false;

    // Let a preflight go on as any request (route matching, middlewares and the OPTIONS route of
    // the application, or 404/405), with the preflight headers already on its response. By
    // default the application answers it with 204, before route matching and the middlewares.
    bool preflight_continue = false;
};

namespace detail {
    // What http_application and response share of CORS (cors.cpp): the options once checked and
    // computed (null, and logged, if invalid); what they mean for a request, which runs the
    // filter of the options and throws what it throws; and its headers, added to a message about
    // to be sent, unless the message has them
    struct cors_policy;
    struct cors_decision {
        std::string origin;             // granted: "*", as it came, or empty for none
        bool preflight = false;         // its response says what a preflight is told
        bool answer_preflight = false;  // the application answers it, not its routes
        std::string requested_headers;  // Access-Control-Request-Headers of a preflight, to reflect
    };
    std::shared_ptr<const cors_policy> make_cors_policy(cors_options options);
    cors_decision decide_cors(const cors_policy& policy, const http_request& request);
    void apply_cors(const cors_policy& policy, const cors_decision& decision, http_response& message);
}

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
// server_base) is an application served on listeners; an application can also be
// used on its own, to dispatch requests in memory, without a connection.
class http_application : public virtual_host {
public:
    http_application();

    // Ends the requests dispatched in memory that are still in flight (see dispatch()), without
    // waiting for their handlers. It may be called from any thread, or from one of those handlers
    ~http_application();

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
    openapi_generator& openapi();

    // Serve the OpenAPI document as JSON at `path` (not served unless called)
    route& serve_openapi(const std::string& path = "/openapi.json");

    // Dispatch a request in memory, without a connection, through the same steps as a
    // network request: route matching, middlewares, body reading and validation, handler,
    // and the same 404/405 handling. Useful for tests, internal calls and gateways.
    // The body is taken from the request content (set_content); chunked requests are not
    // supported. WebSocket, SSE and connection takeover answer 501.
    //
    // A dispatch shares only the serving state with the application, never the application
    // itself, so destroying it (from any thread, or from a handler) ends the dispatches in
    // flight without waiting for them. One without a complete response ends at once, and this
    // throws boost::system::system_error(operation_aborted); a complete one is delivered. One
    // not started yet (this awaitable, awaited later) runs no middleware or handler. A handler
    // running is interrupted as at the shutdown of a server: its next cancellable await ends
    // with operation_aborted, whatever its route declared and even if its scope was cancelled
    // already (request::cancelled() keeps the first reason, cancel_reason::shutdown if none).
    // Its current step, and the awaits it isolated from cancellation, end on their own, and what
    // it sends afterwards is dropped. What it captured itself is the caller's to keep alive.
    //
    // With a response hook (see on_response()), the dispatch ends once the hook has run, so what
    // the hook captures can be destroyed as soon as this returns. The wait is limited by what is
    // left of dispatch_options::timeout: a handler still running then, even one that answered
    // already, gets its response returned without waiting more, and the hook runs when the
    // handler ends, as on a connection: what the hook captures must live as long as the handler.
    // Neither a caller cancelling the dispatch, nor destroying the application, waits for the
    // hook. Without a hook nothing is waited for.
    awaitable<std::shared_ptr<http_response>> dispatch(std::shared_ptr<http_request> request,
                                                       dispatch_options options = {});

    // Same, calling `callback` once with the response; runs on `executor`. With no error to
    // report, a dispatch ended by destroying the application gets 503 Service Unavailable
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

    // Serve cross-origin requests from browsers (see cors_options; off by default). Its headers
    // go on every response of the application, whoever answers it (the handler, a middleware,
    // or the server itself: 400, 404, 405, 413, 415, 500, a route timeout...), on a connection or
    // dispatched in memory; a header set by the handler or a middleware stays instead. It is not
    // access control: a request from an origin not allowed still runs, only without the headers
    // a browser needs to read its response. A preflight (OPTIONS with a non-empty
    // Access-Control-Request-Method) is answered 204 by the application, without running the
    // middlewares, unless cors_options::preflight_continue. Returns `invalid_argument` for
    // options that cannot be applied, the reason in the log: nothing changes then. Set it before
    // serving (listen()): it is read without locks.
    boost::system::error_code enable_cors(cors_options options = {});

    // Back to no CORS headers, and OPTIONS routed as any other method
    void disable_cors();

    // Maximum request body size (8 MB by default), for the routes that do not set their own
    // (route::max_body_size) and the requests matching no route: every read of a body is
    // bounded by it (413 if larger), and an unread body larger than this is not discarded
    // after the response: the connection is closed instead
    void set_max_body_size(std::uint64_t size);
    std::uint64_t get_max_body_size() const;

    // Decoders of request bodies by media type, for the routes taking the body decoded:
    // JSON, MessagePack, CBOR, UBJSON, urlencoded forms, text and binary are built in (see
    // body_decoder_registry); remove or add decoders here. Set before serving.
    body_decoder_registry& body_decoders();

    // Body types accepted by the routes taking the body decoded that do not set their own
    // (route::accepts): names of decoders or media types; {"json"} by default. Others are
    // answered with 415 Unsupported Media Type. Set before serving.
    void accept_body_types(std::vector<std::string> types);
    const std::vector<std::string>& get_body_types() const;

    // Limits of decoded bodies (nesting depth), for the routes that do not set their own
    // (route::decode_limits); set before serving
    void set_body_limits(body_limits limits);
    const body_limits& get_body_limits() const;

    // Decoders, accepted types and limits together, e.g. for the openapi_generator of a
    // virtual host: openapi_generator(app.host(name).router(), &app.get_body_settings())
    const body_settings& get_body_settings() const;

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
    // A request dispatched in memory returns after the hook has run, unless the timeout of the
    // dispatch ends first, or the dispatch is cancelled (see dispatch()).
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

protected:
    // What serves the requests of a connection: it keeps what they read (the hosts and their
    // routes, the middlewares and the settings) alive, not the application, so the requests
    // still running when a server is destroyed find it
    std::function<awaitable<void>(std::shared_ptr<request>)> connection_handler() const;

private:
    class state;
    std::shared_ptr<state> state_;
};

} // namespace thinger::http

#endif // THINGER_HTTP_APPLICATION_HPP

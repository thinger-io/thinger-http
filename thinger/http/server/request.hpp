#ifndef API_REQUEST_HPP
#define API_REQUEST_HPP

#include <string>
#include <memory>
#include <optional>
#include <set>
#include <vector>
#include <nlohmann/json.hpp>
#include <boost/algorithm/string.hpp>

#include "http_stream.hpp"
#include "body_reader.hpp"
#include "body_decoders.hpp"
#include "multipart.hpp"
#include "request_context.hpp"
#include "../common/http_request.hpp"
#include "../common/http_response.hpp"
#include "../../util/types.hpp"

namespace thinger::http{

    // Maximum request body size unless the application or the route set theirs
    inline constexpr size_t default_max_body_size = 8 * 1024 * 1024;

    // Why the scope of a request was cancelled (see request::cancelled()):
    // - client_gone: its client reset the connection, or a read or write on it failed, or it
    //   ended before the request was complete (a client that only closes its sending side
    //   after a complete request still gets its response: that is not client_gone);
    // - timeout: the timeout of its route (or of its in-memory dispatch) expired;
    // - shutdown: the server reached the deadline of its shutdown with the request in progress.
    // The library interrupts a handler for a reason only if its route declared it (see
    // route::cancel_on), and always for shutdown
    enum class cancel_reason : uint8_t { none, client_gone, timeout, shutdown };

    // Forward declarations
    class route;
    class virtual_host;
    class trusted_proxies;
    struct body_settings;
    class http_application;
    namespace proxy_detail { class proxy_exchange; }

    /**
     * Class that represents a single HTTP request over the API. It means, that the HTTP request
     * was matched to some of the registered endpoints in the API. It holds where its body is
     * read from (its connection, or memory), the stream associated to the HTTP request, and the
     * HTTP request itself.
     */
    class request{
    public:
        /// A request whose body comes from `source` (none: no body beyond what it was given,
        /// and no connection), answered on `stream` (none in memory)
        request(std::unique_ptr<detail::request_source> source,
                std::shared_ptr<http_stream> http_stream,
                std::shared_ptr<http_request> http_request);

        virtual ~request();

        // Not copyable: its body reader refers to it
        request(const request&) = delete;
        request& operator=(const request&) = delete;

    public:
        /// get parameter
        const std::string& operator[](const std::string& param) const;

        /// has parameter
        bool has(const std::string& param) const;

        /// erase parameter
        bool erase(const std::string& param);

        std::string debug_parameters() const;

        std::shared_ptr<http_request> get_http_request();
        std::shared_ptr<const http_request> get_http_request() const;

        /// Client IP: the peer address or, if the peer is a trusted proxy (see
        /// http_application::set_trusted_proxies), the client address it forwarded
        std::string get_request_ip() const;

        /// Address of the direct peer (the connection's remote IP), whatever it forwards
        std::string get_peer_ip() const;

        /// The connection the request came from: its peer and local addresses, and whether it
        /// is TLS (for requests dispatched in memory, the peer address of the dispatch). Empty
        /// once the connection is gone
        connection_info connection() const;

        /// TLS session of the connection (e.g. the client certificate on an mTLS route): none
        /// without TLS, in memory, or once the connection is gone
        std::optional<asio::tls_session> tls() const;

        /// Whether the request came on a TLS connection whose handshake (SNI) chose another
        /// client certificate mode or certificate than its host name (Host) gets: it must not be
        /// served there (421, see asio::tls_handshake_record::misdirected()). False at once without
        /// TLS, in memory, or once the connection is gone
        bool misdirected() const;

        /// Client scheme, "http" or "https": the one of the connection (TLS or not; for
        /// requests dispatched in memory, the one of the request, see http_request::is_ssl())
        /// or, if the peer is a trusted proxy (see http_application::set_trusted_proxies),
        /// the one it forwarded for the client of get_request_ip(): the X-Forwarded-Proto value
        /// aligned with X-Forwarded-For (or its last one), or the proto of the Forwarded element
        /// giving the client address (see trusted_proxies::resolve())
        std::string get_request_scheme() const;

        /// Peer address of requests without a connection (in-memory dispatch)
        void set_peer_ip(std::string ip) { peer_ip_ = std::move(ip); }

        /// Proxies whose forwarding header get_request_ip() accepts (set by the server)
        void set_trusted_proxies(std::shared_ptr<const trusted_proxies> proxies) { trusted_proxies_ = std::move(proxies); }

        /// Trusted proxies set by the server (null if none is trusted)
        const std::shared_ptr<const trusted_proxies>& get_trusted_proxies() const { return trusted_proxies_; }

        /// Virtual host serving the request (the application itself for the default host)
        const virtual_host* get_virtual_host() const { return virtual_host_; }
        void set_virtual_host(const virtual_host* host) { virtual_host_ = host; }

        /// Captures of the host pattern that matched: [0] is the host name and [1..] the
        /// groups, e.g. the subdomain matched by "*" in "*.example.com" (the labels named
        /// with ":name" are also request parameters). Empty for exact and default hosts.
        const std::vector<std::string>& get_host_matches() const { return host_matches_; }
        void set_host_matches(std::vector<std::string> matches) { host_matches_ = std::move(matches); }

        // Convenience methods for accessing request data

        /// Get query parameter by key
        std::string query(const std::string& key) const;

        /// Get query parameter by key with default value
        std::string query(const std::string& key, const std::string& default_value) const;

        /// Get request body as string
        std::string body() const;

        /// Get request body parsed as JSON
        nlohmann::json json() const;

        /// Get request header by key
        std::string header(const std::string& key) const;

        std::shared_ptr<http_stream> get_http_stream() const;

        void set_uri_parameter(const std::string& param, const std::string& value);

        void add_uri_parameter(const std::string& param, const std::string& value);

        const std::string& get_uri_parameter(const std::string& param) const;

        const std::multimap<std::string, std::string>& get_uri_parameters() const;

        void set_auth_groups(const std::set<std::string>& groups);

        const std::set<std::string>& get_auth_groups() const;

        void set_auth_user(const std::string& auth_user);

        const std::string& get_auth_user() const;

        /// Typed values of this request (see request_context), e.g. the principal an
        /// authentication middleware stores for the route handler. Empty at first (or with the
        /// values of dispatch_options::context), they are destroyed with the request.
        request_context& context() noexcept { return context_; }
        const request_context& context() const noexcept { return context_; }

        void set_matched_route(const route* route);
        
        const route* get_matched_route() const;

        // Whether the client asked to keep the connection (its Connection header and HTTP
        // version), not whether the server keeps it after the response
        bool keep_alive() const;

        // --- Cancellation (see cancel_reason, route::cancel_on) ---

        /// Why the scope of the request was cancelled: none until then (or for a request
        /// without a stream). Its handler goes on unless its route declared the reason (or
        /// the server shuts down): it looks here where it chooses
        cancel_reason cancelled() const;

        /// Completes once the scope of the request is cancelled, whatever the reason, and
        /// returns it; none if this wait is cancelled itself (e.g. by `||`). For a request
        /// without a stream it never completes. Await it on the executor of the handler
        thinger::awaitable<cancel_reason> client_gone();

        /// Slot cancelled (terminal) with the scope of the request while the handler runs,
        /// for the awaits the handler chooses to abandon then: bind it with
        /// boost::asio::bind_cancellation_slot, to one pending await at a time (asio's rule).
        /// Not connected for a request without a stream
        boost::asio::cancellation_slot cancellation_slot();

        // --- Body reading (see body_reader) ---

        /// Store read-ahead data: the bytes read past the request head (called by the
        /// connection before dispatch)
        void set_read_ahead(const uint8_t* data, size_t size);

        /// Read exactly `size` bytes (read-ahead first, then the source). TCP backpressure.
        /// Returns less at the end of the body, or if it fails (see get_body_error()), e.g.
        /// for a body over the maximum body size (a Content-Length one before reading it).
        thinger::awaitable<size_t> read(uint8_t* buffer, size_t size);

        /// Read up to `max_size` bytes (read-ahead first, then the source); 0 at the end of the
        /// body, or if it fails (see get_body_error()), as read(). A read cancelled (e.g. by
        /// `read_some() || timer`) returns 0 too, without failing: the body is still pending.
        thinger::awaitable<size_t> read_some(uint8_t* buffer, size_t max_size);

        /// Read full body into http_request content (for non-deferred dispatch), decoding
        /// its Content-Encoding (gzip, deflate, or a list of them; see util::decode_content).
        /// Fails if the body exceeds the maximum body size (see get_max_body_size()), a
        /// Content-Length one without reading it.
        /// Only reads what is still pending: do not mix it with read()/read_some() on
        /// the same request, or the stored body will be incomplete (and fail to
        /// decompress if Content-Encoding is set).
        thinger::awaitable<bool> read_body();

        /// Read a multipart/form-data body (see multipart_form): its fields in memory and its
        /// files in temporary files, within the limits of the options and the maximum body
        /// size. Check the form for errors (400 malformed, 413 over a limit, 415 not a form).
        /// To stream the parts instead, read them with a multipart_reader.
        thinger::awaitable<multipart_form> read_multipart(multipart_options options = {});

        /// Read and drop an unread body so the connection can be reused.
        /// Returns false if the body exceeds `max_size` or the read fails.
        thinger::awaitable<bool> discard_body(size_t max_size);

        /// Whether part of the body is still unread by read(), read_some() or read_body().
        bool has_pending_body() const;

        /// Why reading the body failed (body_error::none if it did not): too large, invalid
        /// chunked framing, connection closed before its end, or undecodable Content-Encoding
        body_error get_body_error() const;

        /// Content-Length convenience (0 for chunked requests)
        size_t content_length() const;

        /// Whether the request uses chunked transfer encoding
        bool is_chunked() const;

        /// Remove and return the unconsumed read-ahead bytes (data following this request)
        std::vector<uint8_t> take_read_ahead();

        /// Maximum body size of every read (read_body(), read(), read_some()), also once
        /// decompressed: the one of the matched route, or of the application (set by the server)
        void set_max_body_size(size_t size) { body_reader_.set_limit(size); }
        size_t get_max_body_size() const { return body_reader_.limit(); }

        /// How the matched route decodes the body (set by the server; the defaults if not)
        void set_body_settings(const body_settings* settings) { body_settings_ = settings; }
        const body_settings& get_body_settings() const;

    private:

        /// After reading the body: once read to its end, none of it is left on the connection
        /// (see http_stream::input_pending())
        void note_body_read();

        /**
         * A stream is a channel inside the HTTP connection. i.e, while receiving multiple requests
         * and responses are generated asynchronously, it is required to know which response
         * belong to a given query, and provide an ordered response (to support HTTP pipelining).
         * With HTTP2.0 it is possible to provide responses without pipelining.
         */
        std::shared_ptr<http_stream> http_stream_;

        /**
         * Where the body is read from, and what is known of the connection (see
         * detail::request_source). After the stream it refers to, so destroyed before it
         */
        std::unique_ptr<detail::request_source> source_;

        /**
         * This is the request that originated the API Request, and required to know the body, the
         * content type, etc.
         */
        std::shared_ptr<http_request> http_request_;

        /**
         * Vector for storing the matched parameters found in the URL, like username, device, etc.
         */
        std::multimap<std::string, std::string> params_;

        std::string auth_user_;

        std::string peer_ip_;

        std::shared_ptr<const trusted_proxies> trusted_proxies_;

        const virtual_host* virtual_host_ = nullptr;

        std::vector<std::string> host_matches_;

        std::set<std::string> groups_;

        /// Values stored by the middlewares (or the in-memory dispatch) for the handler
        request_context context_;

        const route* matched_route_ = nullptr;

        /// Body framing and reading: read-ahead bytes, then the connection
        body_reader body_reader_;

        /// Body decoders, accepted types and limits of the application (null: defaults)
        const body_settings* body_settings_ = nullptr;

        /// The sink of the response, the deadline of a route timeout and the proxy watching the
        /// client reach the connection through the source
        friend class connection_sink;
        friend class http_application;
        friend class proxy_detail::proxy_exchange;

        /// Body parsed as JSON by the matched route, for callbacks taking it
        friend class route;
        nlohmann::json json_body_;

        /// Type of the body the matched route accepted (matched before the body is read):
        /// its media type (null until matched) and decoder (null if none). They point to the
        /// decoder registry for the media types its decoders list, else to body_type_.
        const media_type* body_media_type_ = nullptr;
        const body_format* body_format_ = nullptr;
        std::optional<body_type_match> body_type_;
    };

}


#endif
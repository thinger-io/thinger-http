#ifndef API_REQUEST_HPP
#define API_REQUEST_HPP

#include <string>
#include <memory>
#include <set>
#include <vector>
#include <nlohmann/json.hpp>
#include <boost/algorithm/string.hpp>

#include "http_stream.hpp"
#include "server_connection.hpp"
#include "body_reader.hpp"
#include "../common/http_response.hpp"
#include "../../util/types.hpp"

namespace thinger::http{

    // Forward declarations
    class route;
    class virtual_host;
    class trusted_proxies;

    /**
     * Class that represents a single HTTP request over the API. It means, that the HTTP request
     * was matched to some of the registered endpoints in the API. It holds the original HTTP
     * connection, the stream associated to the HTTP request, and the HTTP request itself.
     */
    class request{
    public:
        request(const std::shared_ptr<server_connection>& http_connection,
                    const std::shared_ptr<http_stream>& http_stream,
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

        /// Client IP: the peer address or, if the peer is a trusted proxy (see
        /// http_application::set_trusted_proxies), the client address it forwarded
        std::string get_request_ip() const;

        /// Address of the direct peer (the connection's remote IP), whatever it forwards
        std::string get_peer_ip() const;

        /// Peer address of requests without a connection (in-memory dispatch)
        void set_peer_ip(std::string ip) { peer_ip_ = std::move(ip); }

        /// Proxies whose forwarding header get_request_ip() accepts (set by the server)
        void set_trusted_proxies(std::shared_ptr<const trusted_proxies> proxies) { trusted_proxies_ = std::move(proxies); }

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

        std::shared_ptr<server_connection> get_http_connection() const;
        
        std::shared_ptr<http_stream> get_http_stream() const;

        void set_uri_parameter(const std::string& param, const std::string& value);

        void add_uri_parameter(const std::string& param, const std::string& value);

        const std::string& get_uri_parameter(const std::string& param) const;

        const std::multimap<std::string, std::string>& get_uri_parameters() const;

        void set_auth_groups(const std::set<std::string>& groups);

        const std::set<std::string>& get_auth_groups() const;

        void set_auth_user(const std::string& auth_user);

        const std::string& get_auth_user() const;
        
        void set_matched_route(const route* route);
        
        const route* get_matched_route() const;


        bool keep_alive() const;

        // --- Body reading (see body_reader) ---

        /// Store read-ahead data (called by server_connection before dispatch)
        void set_read_ahead(const uint8_t* data, size_t size);

        /// Read exactly `size` bytes (read-ahead first, then socket). TCP backpressure.
        /// Returns less at the end of the body, or if it fails (see get_body_error()).
        thinger::awaitable<size_t> read(uint8_t* buffer, size_t size);

        /// Read up to `max_size` bytes (read-ahead first, then socket); 0 at the end of the
        /// body, or if it fails (see get_body_error()).
        thinger::awaitable<size_t> read_some(uint8_t* buffer, size_t max_size);

        /// Read full body into http_request content (for non-deferred dispatch), decoding
        /// its Content-Encoding (gzip, deflate). Fails if the body exceeds the maximum body
        /// size (set_max_body_size, the server limit), without reading it.
        /// Only reads what is still pending: do not mix it with read()/read_some() on
        /// the same request, or the stored body will be incomplete (and fail to
        /// decompress if Content-Encoding is set).
        thinger::awaitable<bool> read_body();

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

        /// Bytes remaining in read-ahead buffer
        size_t read_ahead_available() const;

        /// Remove and return the unconsumed read-ahead bytes (data following this request)
        std::vector<uint8_t> take_read_ahead();

        /// Maximum body size accepted by read_body() (set by the server)
        void set_max_body_size(size_t size) { max_body_size_ = size; }

        /// Underlying socket. Do not read or write it while the server owns the connection:
        /// read the body with read()/read_some(), or own the socket with response::take_over().
        std::shared_ptr<asio::socket> get_socket() const;

    private:

        /**
         * HTTP connection is the wrapper for a raw socket, being able to parse request and to
         * pipeline HTTP responses
         */
        std::weak_ptr<server_connection> http_connection_;

        /**
         * A stream is a channel inside the HTTP connection. i.e, while receiving multiple requests
         * and responses are generated asynchronously, it is required to know which response
         * belong to a given query, and provide an ordered response (to support HTTP pipelining).
         * With HTTP2.0 it is possible to provide responses without pipelining.
         */
        std::weak_ptr<http_stream> http_stream_;

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
        
        const route* matched_route_ = nullptr;

        /// Body framing and reading: read-ahead bytes, then the connection
        body_reader body_reader_;

        /// Max body size for read_body()
        size_t max_body_size_ = 8 * 1024 * 1024;

        /// Body parsed as JSON by the matched route, for callbacks taking it
        friend class route;
        nlohmann::json json_body_;
    };

}


#endif
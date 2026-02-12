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
#include "../common/http_response.hpp"
#include "../../util/types.hpp"

namespace thinger::http{

    // Forward declarations
    class route;
    enum class auth_level;

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

    public:
        /// get parameter
        const std::string& operator[](const std::string& param) const;

        /// has parameter
        bool has(const std::string& param) const;

        /// erase parameter
        bool erase(const std::string& param);

        std::string debug_parameters() const;

        std::shared_ptr<http_request> get_http_request();

        std::string get_request_ip() const;

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

        //void add_matched_param(const std::string& param);

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
        
        auth_level get_required_auth_level() const;


        bool keep_alive() const;

        // --- Deferred body reading support ---

        /// Store read-ahead data (called by server_connection before dispatch)
        void set_read_ahead(const uint8_t* data, size_t size);

        /// Read exactly `size` bytes (read-ahead first, then socket). TCP backpressure.
        thinger::awaitable<size_t> read(uint8_t* buffer, size_t size);

        /// Read up to `max_size` bytes (read-ahead first, then socket).
        thinger::awaitable<size_t> read_some(uint8_t* buffer, size_t max_size);

        /// Read full body into http_request content (for non-deferred dispatch).
        thinger::awaitable<bool> read_body();

        /// Content-Length convenience (0 for chunked requests)
        size_t content_length() const;

        /// Whether the request uses chunked transfer encoding
        bool is_chunked() const;

        /// Bytes remaining in read-ahead buffer
        size_t read_ahead_available() const;

        /// Direct socket access (for pipe-style forwarding)
        std::shared_ptr<asio::socket> get_socket() const;

        //exec_result get_request_data() const;

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

        std::set<std::string> groups_;
        
        const route* matched_route_ = nullptr;

        /// Leftover data from header parsing buffer (for deferred body reading)
        std::vector<uint8_t> read_ahead_;
        size_t read_ahead_offset_ = 0;

        /// Raw read (bypasses chunked decoding) — reads from read-ahead, then socket
        thinger::awaitable<size_t> raw_read_some(uint8_t* buffer, size_t max_size);

        /// Chunked transfer encoding decoder state
        enum class chunk_state { size, size_lf, data, data_cr, data_lf, trailer_lf, done };
        chunk_state chunk_state_ = chunk_state::size;
        size_t chunk_remaining_ = 0;
        size_t chunk_size_accum_ = 0;

        /// Read with chunked decoding (transparent to caller)
        thinger::awaitable<size_t> read_some_chunked(uint8_t* buffer, size_t max_size);

        /// Max body size for non-deferred chunked read_body()
        size_t max_body_size_ = 8 * 1024 * 1024;

    public:
        void set_max_body_size(size_t size) { max_body_size_ = size; }
    };

}


#endif
#ifndef THINGER_HTTP_CLIENT_HTTP_CLIENT_BASE_HPP
#define THINGER_HTTP_CLIENT_HTTP_CLIENT_BASE_HPP

#include "../common/http_request.hpp"
#include "../common/http_response.hpp"
#include "client_connection.hpp"
#include "client_response.hpp"
#include "connection_pool.hpp"
#include "proxy.hpp"
#include "websocket_client.hpp"
#include "stream_types.hpp"
#include "../../util/types.hpp"
#include "form.hpp"
#include <memory>
#include <string>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <vector>
#include <boost/asio/ssl/context.hpp>

namespace thinger::http {

using headers_map = std::map<std::string, std::string>;

class http_client_base {
protected:
    // Configuration
    std::chrono::seconds timeout_{30};
    unsigned int max_redirects_{5};
    bool follow_redirects_{true};
    std::string user_agent_{"ThingerHTTP/2.0"};
    bool auto_decompress_{true};
    bool verify_ssl_{true};
    std::string unix_socket_;
    size_t max_content_size_{8 * 1048576};  // 8 MB default; buffered-mode responses above this are rejected

    // Proxy set with proxy() (null if none), and the one of the environment (null if not
    // used): the first one set applies
    std::shared_ptr<const proxy_resolver> proxy_;
    std::shared_ptr<const proxy_resolver> env_proxy_;

    // CAs trusted besides (or instead of) the system ones
    std::vector<std::string> ca_files_;
    std::vector<std::string> ca_pems_;
    bool system_ca_{true};

    // Connections kept alive between requests
    connection_pool pool_;

    // Abstract method - each derived class provides its own io_context
    virtual boost::asio::io_context& get_io_context() = 0;

    // Apply default headers
    void apply_default_headers(const std::shared_ptr<http_request>& request);

    // A connection for a request: an idle one of the pool if `reuse` and there is one, or a
    // new one (empty, and why in `ec`, if its proxy or CAs cannot be used)
    connection_pool::lease get_connection(const std::shared_ptr<http_request>& request,
                                          boost::system::error_code& ec, bool reuse = true);

public:
    http_client_base();
    virtual ~http_client_base();

    // Configuration setters (fluent API)
    http_client_base& timeout(std::chrono::seconds t) { timeout_ = t; return *this; }
    http_client_base& max_redirects(unsigned int max) { max_redirects_ = max; return *this; }
    http_client_base& follow_redirects(bool follow) { follow_redirects_ = follow; return *this; }
    http_client_base& user_agent(const std::string& agent) { user_agent_ = agent; return *this; }
    // Ask for gzip and deflate bodies, and decode them (the default; also a list of codings,
    // see util::decode_content); otherwise bodies are kept as received, with their
    // Content-Encoding
    http_client_base& auto_decompress(bool decompress) { auto_decompress_ = decompress; return *this; }
    http_client_base& verify_ssl(bool verify) { verify_ssl_ = verify; return *this; }
    http_client_base& unix_socket(const std::string& path) { unix_socket_ = path; return *this; }
    http_client_base& max_content_size(size_t size) { max_content_size_ = size; return *this; }

    // Proxy for every request (see proxy_config), instead of the one of the environment: one
    // URL for every scheme ("" to connect directly), and the hosts reached directly
    http_client_base& proxy(const std::string& url, const std::string& no_proxy = "");
    http_client_base& proxy(const proxy_config& config);
    // Use the proxy of the environment variables (read now) when none is set with proxy();
    // on by default (read when the client is created)
    http_client_base& proxy_from_environment(bool enabled);

    // CAs trusted to verify servers, besides the system ones: a PEM file (one or more
    // certificates), or PEM text. Loaded when first needed, again if the file changes
    http_client_base& add_ca_file(const std::string& path) { ca_files_.push_back(path); return *this; }
    http_client_base& add_ca_pem(const std::string& pem) { ca_pems_.push_back(pem); return *this; }
    // Trust the system CAs (the default); false trusts only those added
    http_client_base& use_system_ca(bool enabled) { system_ca_ = enabled; return *this; }

    // Keep-alive pool (see connection_pool): idle connections kept per origin (0 disables
    // it: a new connection for each request), in total, and for how long
    http_client_base& max_idle_connections_per_host(size_t max);
    http_client_base& max_idle_connections(size_t max);
    http_client_base& idle_timeout(std::chrono::milliseconds timeout);

    // Configuration getters
    std::chrono::seconds get_timeout() const { return timeout_; }
    unsigned int get_max_redirects() const { return max_redirects_; }
    bool get_follow_redirects() const { return follow_redirects_; }
    const std::string& get_user_agent() const { return user_agent_; }
    bool get_auto_decompress() const { return auto_decompress_; }
    bool get_verify_ssl() const { return verify_ssl_; }
    size_t get_max_content_size() const { return max_content_size_; }
    size_t get_max_idle_connections_per_host() const { return pool_.get_config().max_idle_per_host; }
    size_t get_max_idle_connections() const { return pool_.get_config().max_idle; }
    std::chrono::milliseconds get_idle_timeout() const { return pool_.get_config().idle_timeout; }

    // The proxy a request to `url` goes through ("http://host:port"), empty if it connects
    // directly (or if the proxy cannot be used)
    std::string proxy_for(const std::string& url) const;
    std::string proxy_for(const http_request& request) const;

    // Request creation
    std::shared_ptr<http_request> create_request(method m, const std::string& url);

    // Main HTTP methods - all return awaitable
    awaitable<client_response> get(const std::string& url, headers_map headers = {});
    awaitable<client_response> post(const std::string& url, std::string body = {},
                                    std::string content_type = "application/json", headers_map headers = {});
    awaitable<client_response> post(const std::string& url, const form& form, headers_map headers = {});
    awaitable<client_response> put(const std::string& url, std::string body = {},
                                   std::string content_type = "application/json", headers_map headers = {});
    awaitable<client_response> patch(const std::string& url, std::string body = {},
                                     std::string content_type = "application/json", headers_map headers = {});
    awaitable<client_response> del(const std::string& url, headers_map headers = {});
    awaitable<client_response> head(const std::string& url, headers_map headers = {});
    awaitable<client_response> options(const std::string& url, headers_map headers = {});

    // Generic send with custom request
    awaitable<client_response> send(std::shared_ptr<http_request> request);

    // Streaming send - streams response through callback without loading into memory
    awaitable<stream_result> send_streaming(std::shared_ptr<http_request> request,
                                            stream_callback callback);

    // Send and get the connection of the (last) response back: for upgrades like WebSocket,
    // as one that switched protocols is never reused. Any other may be reused by the client
    awaitable<std::pair<client_response, std::shared_ptr<client_connection>>>
        send_with_connection(std::shared_ptr<http_request> request);

    // WebSocket upgrade (simple API)
    awaitable<std::optional<websocket_client>> upgrade_websocket(
        const std::string& url,
        const std::string& subprotocol = "");

    // WebSocket upgrade with custom request/headers (for builder pattern)
    awaitable<std::optional<websocket_client>> upgrade_websocket(
        std::shared_ptr<http_request> request,
        const std::string& subprotocol = "");

    // Connection pool management: close the idle connections, count them, and the
    // connections by origin, idle and in use
    void clear_connections() { pool_.clear(); }
    size_t pool_size() const { return pool_.size(); }
    connection_pool::stats pool_stats() const { return pool_.get_stats(); }

private:
    // How a connection reaches the origin of a request: through a proxy or not, with which
    // TLS context, and the key of connections that can be shared
    struct connection_plan {
        proxy_route route;
        std::shared_ptr<boost::asio::ssl::context> tls;
        std::string key;
    };

    // TLS contexts by what they trust, shared by the connections (and their id)
    struct tls_entry {
        uint64_t id;
        std::shared_ptr<boost::asio::ssl::context> context;
    };
    static constexpr size_t MAX_TLS_CONTEXTS = 32;
    std::mutex tls_mutex_;
    std::map<std::string, tls_entry> tls_contexts_;
    uint64_t next_tls_id_{0};

    // The proxy of a request: none, with an error if it is set but cannot be used
    proxy_resolver::choice select_proxy(const http_request& request) const;

    // The TLS context of a request, from the CAs of the client and the request
    std::shared_ptr<boost::asio::ssl::context> tls_context(const http_request& request, uint64_t& id,
                                                           boost::system::error_code& ec);

    connection_plan plan_connection(const http_request& request, boost::system::error_code& ec);
    std::shared_ptr<client_connection> make_connection(const connection_plan& plan);

    // Send following redirects; the connection of the last response in `used`, if given
    awaitable<client_response> send_with_redirects(std::shared_ptr<http_request> request,
                                                   std::shared_ptr<client_connection>* used = nullptr);

    // Send on the connection of `lease`, and again on a new one (then in `lease`) if a reused
    // one failed and the request can be sent twice (see can_retry)
    awaitable<std::shared_ptr<http_response>> send_request(const std::shared_ptr<http_request>& request,
                                                           connection_pool::lease& lease);
    static bool can_retry(const client_connection& connection, const http_request& request);

    // Check if URLs have same origin (for security when forwarding headers)
    static bool is_same_origin(const std::string& url1, const std::string& url2);
};

} // namespace thinger::http

#endif

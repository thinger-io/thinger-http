#ifndef THINGER_HTTP_SERVER_REVERSE_PROXY_EXCHANGE_HPP
#define THINGER_HTTP_SERVER_REVERSE_PROXY_EXCHANGE_HPP

// Internal to the reverse proxy (see reverse_proxy.hpp): shared by its source files only

#include "../reverse_proxy.hpp"
#include "../request.hpp"
#include "../response.hpp"
#include "../../client/response_factory.hpp"
#include "../../../asio/sockets/tcp_socket.hpp"
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace thinger::http::proxy_detail {

    using clock = std::chrono::steady_clock;

    // Bytes read or written at once on each side
    inline constexpr size_t buffer_size = 16 * 1024;

    // --- Headers (proxy_request.cpp) ---

    // Path and query sent in the request line: an absolute path, without whitespace,
    // control characters or a fragment
    bool is_request_target(std::string_view target);

    // Headers that can be sent as they are: valid names and values
    bool has_valid_headers(const headers& message);

    // Names listed by the Connection headers: hop-by-hop headers of this message
    std::vector<std::string> connection_options(const headers& message);

    bool is_listed(const std::vector<std::string>& options, std::string_view name);

    // Whether a header goes from one hop to the next: not hop-by-hop, nor listed in Connection
    bool is_end_to_end(std::string_view name, const std::vector<std::string>& connection);

    // Remove the headers of the framing and the connection, set by the proxy
    void remove_framing_headers(headers& message);

    // Copy the status and end-to-end headers of a response of the target, but its
    // Content-Length (the framing is set by the response to the client)
    void copy_end_to_end(const http_response& head, http_response& out);

    bool is_ip_address(const std::string& value);

    // Tunnel between a client and a target after a protocol upgrade (proxy_tunnel.cpp)
    awaitable<void> run_tunnel(std::shared_ptr<asio::socket> client, std::shared_ptr<asio::socket> target,
                               std::string to_client, std::string to_target);

    // A request forwarded to a target, and its response. Its parts are defined by concern:
    // the request (proxy_request.cpp), its body (proxy_body.cpp), the response
    // (proxy_response.cpp), the tunnel of an upgrade (proxy_tunnel.cpp), and the connection
    // and timeouts (reverse_proxy.cpp).
    class proxy_exchange {
    public:
        proxy_exchange(reverse_proxy& proxy, request& req, response& res, const proxy_target& target,
                       const proxy_options& options)
            : proxy_(proxy), req_(req), res_(res), target_(target), options_(options),
              incoming_(req.get_http_request()) {}

        awaitable<proxy_result> run();

    private:
        enum class body_mode { none, length, chunked };

        // Build the request for the target; false if it cannot be forwarded (answered)
        bool prepare();
        bool check_method();
        bool check_upgrade(const std::vector<std::string>& connection);
        std::string target_path() const;
        std::string target_host() const;
        void copy_request_headers(http_request& outgoing, const std::vector<std::string>& connection) const;
        bool check_outgoing(const http_request& outgoing);
        bool frame_body(http_request& outgoing);
        void add_connection_headers(http_request& outgoing) const;

        // Forwarding headers: those received from a trusted proxy, and this hop's
        void add_forwarding_headers(http_request& outgoing) const;
        void add_x_forwarded(http_request& outgoing, const std::string& peer, bool keep_received) const;
        void add_forwarded(http_request& outgoing, const std::string& peer, bool keep_received) const;

        // Connect to the target within the connect timeout; false if it failed (answered)
        awaitable<bool> connect();
        awaitable<boost::system::error_code> connect_target();
        awaitable<boost::system::error_code> start_tls(std::shared_ptr<asio::tcp_socket> tcp);

        // Send the request, read the response head, then relay the response or tunnel
        awaitable<void> exchange();
        awaitable<bool> send_head();
        awaitable<void> stream_body();
        void frame_part(std::vector<boost::asio::const_buffer>& buffers, std::string& size_line,
                        const uint8_t* data, size_t size) const;
        awaitable<bool> read_head();
        void request_done();
        awaitable<void> relay_response();
        void upgrade();

        // Relay the response body, as framed by the target
        std::optional<size_t> content_length() const;
        std::optional<size_t> advertised_length() const;
        awaitable<bool> relay_body();
        awaitable<bool> relay_length(size_t length);
        awaitable<bool> relay_chunked();
        awaitable<bool> relay_until_close();

        // End the exchange once the idle or total timeout passes, or once the client closed
        // its connection (after sending the whole request)
        awaitable<void> watchdog();
        awaitable<void> watch_client();
        awaitable<void> wait_forever();
        void finish_stopped();

        // I/O with the target and the client, tracking activity for the idle timeout
        awaitable<io_result> read_target(uint8_t* buffer, size_t size);
        awaitable<bool> write_target(const std::vector<boost::asio::const_buffer>& buffers);
        awaitable<bool> send_to_client(std::string data);
        void touch() { last_activity_ = clock::now(); }

        // Answer an error to the client, or abort the response if it already started
        void fail(http_response::status status, const std::string& message, const std::string& detail);
        void reject_large_body();
        // The client connection is closed after the response: the rest of its request is not read
        void close_client_after_response();

        void close_target();
        boost::asio::io_context* io_context();
        std::string describe() const;

        reverse_proxy& proxy_;
        request& req_;
        response& res_;
        const proxy_target& target_;
        const proxy_options& options_;
        std::shared_ptr<http_request> incoming_;
        boost::asio::any_io_executor executor_;

        // Why the exchange was stopped before it ended: its failures are a consequence
        enum class stop_reason { none, timeout, client_left, request_failed };

        // Request for the target
        std::string request_head_;
        method outgoing_method_ = method::UNKNOWN;
        body_mode body_mode_ = body_mode::none;
        bool body_buffered_ = false;
        bool upgrade_requested_ = false;
        bool request_sent_ = false;

        std::shared_ptr<asio::socket> target_socket_;
        bool tls_config_error_ = false;

        // Bytes read from the target and not consumed yet
        std::string pending_;

        // Response head of the target
        std::shared_ptr<http_response> head_;
        int status_code_ = 0;
        response_factory::body_framing body_framing_ = response_factory::body_framing::none;

        bool response_started_ = false;
        // The whole response is queued: the connection may close after it (not the client leaving)
        bool response_ended_ = false;
        stop_reason stopped_ = stop_reason::none;
        std::optional<boost::asio::steady_timer> request_sent_signal_;
        clock::time_point start_;
        clock::time_point last_activity_;

        proxy_result result_;
    };

}

#endif // THINGER_HTTP_SERVER_REVERSE_PROXY_EXCHANGE_HPP

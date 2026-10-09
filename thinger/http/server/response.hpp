#ifndef THINGER_HTTP_SERVER_RESPONSE_HPP
#define THINGER_HTTP_SERVER_RESPONSE_HPP

#include "../common/http_response.hpp"
#include "../../util/logger.hpp"
#include "server_connection.hpp"
#include "http_stream.hpp"
#include "websocket_connection.hpp"
#include "sse_connection.hpp"
#include "response_sink.hpp"
#include "error_format.hpp"
#include "../../util/compression.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <functional>
#include <filesystem>
#include <set>
#include <string>

namespace thinger::http {

// Forward declarations
class websocket_connection;
class sse_connection;

// Response to a request. It is a handle: copies share the response, so a handler may keep
// a copy and answer later, from any thread. Only the first answer is sent.
class response {
public:
    response(const std::shared_ptr<server_connection>& connection,
             const std::shared_ptr<http_stream>& stream,
             const std::shared_ptr<http::http_request>& http_request,
             bool cors_enabled = false);

    // Response written to `sink`, e.g. a memory_response for requests dispatched in memory
    response(std::shared_ptr<response_sink> sink,
             const std::shared_ptr<http::http_request>& http_request,
             bool cors_enabled = false);

    // JSON response
    void json(const nlohmann::json& data, http::http_response::status status = http::http_response::status::ok);

    // Text response
    void send(const std::string& text, const std::string& content_type = "text/plain");

    // HTML response
    void html(const std::string& html) {
        send(html, "text/html");
    }

    // Error response, with the body produced by the error formatter (see
    // http_application::set_error_formatter): by default the message as text/plain, or a
    // JSON object for errors with details
    void error(http::http_response::status status, const std::string& message = "",
               const nlohmann::json& details = nullptr);

    // Formatter used by error() (set by the server)
    void set_error_formatter(std::shared_ptr<const error_formatter> formatter);

    // Set status code (for building custom responses)
    void status(http::http_response::status s);

    // Set header (for building custom responses)
    void header(const std::string& key, const std::string& value);

    // Send raw http_response object (for advanced use cases)
    void send_response(const std::shared_ptr<http_response>& response);

    // WebSocket upgrade
    void upgrade_websocket(std::function<void(std::shared_ptr<websocket_connection>)> handler,
                          const std::set<std::string>& supported_protocols = {});

    // Server-Sent Events
    void start_sse(std::function<void(std::shared_ptr<sse_connection>)> handler);

    // Take over the connection (e.g. for a custom protocol upgrade or a tunnel): sends
    // this response (status and headers set so far, 200 by default), then the server
    // stops handling the connection and passes the socket to `handler`, together with
    // any bytes already read past this request. The handler fully owns the socket.
    void take_over(takeover_handler handler);

    // File sending
    void send_file(const std::filesystem::path& path, bool force_download = false);
    
    // Redirect response
    void redirect(const std::string& url, http::http_response::status redirect_type = http::http_response::status::moved_temporarily);
    
    // Chunked response support
    bool start_chunked(const std::string& content_type, http::http_response::status status = http::http_response::status::ok);
    bool write_chunk(const std::string& data);
    bool end_chunked();

    // Check if response has been sent
    bool has_responded() const;

    // Get the underlying connection (for advanced use cases); null for requests
    // dispatched in memory
    std::shared_ptr<server_connection> get_connection() const;

private:
    struct state;

    bool ensure_not_responded() const;
    // Response being built, created on first use
    http_response& prepare_response();
    void compress_response_if_needed();
    // Mark the response as sent; false (and nothing must be sent) if it already was
    bool mark_responded();
    void send_prepared_response();
    // Answers 501 if the connection cannot be taken over (in-memory requests)
    bool reject_takeover(const char* feature);

    std::shared_ptr<state> state_;
};

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_RESPONSE_HPP
#ifndef THINGER_HTTP_SERVER_RESPONSE_HPP
#define THINGER_HTTP_SERVER_RESPONSE_HPP

#include "../common/http_response.hpp"
#include "server_connection.hpp"
#include "http_stream.hpp"
#include "websocket_connection.hpp"
#include "sse_connection.hpp"
#include "../../util/compression.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <functional>
#include <filesystem>
#include <set>

namespace thinger::http {

// Forward declarations
class websocket_connection;
class sse_connection;

class response {
private:
    std::weak_ptr<server_connection> connection_;
    std::weak_ptr<http_stream> stream_;
    std::shared_ptr<http::http_request> http_request_;
    std::shared_ptr<http_response> response_;
    bool responded_ = false;
    bool cors_enabled_ = false;

    void ensure_not_responded() const {
        if (responded_) {
            throw std::runtime_error("Response already sent");
        }
    }
    
    void prepare_response() {
        if (!response_) {
            response_ = std::make_shared<http_response>();
            response_->set_keep_alive(http_request_->keep_alive());
            
            // Add CORS headers if enabled
            if (cors_enabled_) {
                response_->add_header("Access-Control-Allow-Origin", "*");
                response_->add_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS, HEAD, PATCH");
                response_->add_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
                response_->add_header("Access-Control-Allow-Credentials", "true");
            }
        }
    }
    
    static bool is_compressible_content_type(const std::string& content_type) {
        // Only compress text-based content types
        return content_type.starts_with("text/")
            || content_type.starts_with("application/json")
            || content_type.starts_with("application/xml")
            || content_type.starts_with("application/javascript")
            || content_type.starts_with("application/x-javascript")
            || content_type.starts_with("image/svg+xml");
    }

    void compress_response_if_needed() {
        // Only compress if there's a body worth compressing
        const auto& content = response_->get_content();
        if (content.size() < 200) return;

        // Don't compress if already compressed
        if (response_->has_header("Content-Encoding")) return;

        // Only compress text-based content types
        const auto& ct = response_->get_content_type();
        if (ct.empty() || !is_compressible_content_type(ct)) return;

        // Check what the client accepts
        std::string accept_encoding = http_request_->get_header("Accept-Encoding");
        if (accept_encoding.empty()) return;

        try {
            if (accept_encoding.find("gzip") != std::string::npos) {
                response_->set_content(::thinger::util::gzip::compress(content));
                response_->add_header("Content-Encoding", "gzip");
            } else if (accept_encoding.find("deflate") != std::string::npos) {
                response_->set_content(::thinger::util::deflate::compress(content));
                response_->add_header("Content-Encoding", "deflate");
            }
        } catch (...) {
            // Compression failed — send uncompressed (content unchanged)
        }
    }

    void send_prepared_response() {
        ensure_not_responded();
        prepare_response();
        compress_response_if_needed();

        if (auto conn = connection_.lock()) {
            if (auto str = stream_.lock()) {
                conn->handle_stream(str, response_);
            }
        }
        responded_ = true;
    }

public:
    response(const std::shared_ptr<server_connection>& connection,
             const std::shared_ptr<http_stream>& stream, 
             const std::shared_ptr<http::http_request>& http_request,
             bool cors_enabled = false)
        : connection_(connection), stream_(stream), http_request_(http_request), cors_enabled_(cors_enabled) {}

    // JSON response
    void json(const nlohmann::json& data, http::http_response::status status = http::http_response::status::ok) {
        prepare_response();
        response_->set_status(status);
        response_->set_content(data.dump(), "application/json");
        send_prepared_response();
    }

    // Text response
    void send(const std::string& text, const std::string& content_type = "text/plain") {
        prepare_response();
        response_->set_content(text, content_type);
        send_prepared_response();
    }

    // HTML response
    void html(const std::string& html) {
        send(html, "text/html");
    }

    // Error response
    void error(http::http_response::status status, const std::string& message = "") {
        prepare_response();
        response_->set_status(status);
        if (!message.empty()) {
            response_->set_content(message, "text/plain");
        }
        send_prepared_response();
    }

    // Set status code (for building custom responses)
    void status(http::http_response::status s) {
        ensure_not_responded();
        prepare_response();
        response_->set_status(s);
    }

    // Set header (for building custom responses)
    void header(const std::string& key, const std::string& value) {
        ensure_not_responded();
        prepare_response();
        response_->add_header(key, value);
    }

    // Send raw http_response object (for advanced use cases)
    void send_response(const std::shared_ptr<http_response>& response) {
        ensure_not_responded();
        response_ = response;

        // Ensure keep-alive is set properly
        response_->set_keep_alive(http_request_->keep_alive());

        // Add CORS headers if enabled
        if (cors_enabled_) {
            response_->add_header("Access-Control-Allow-Origin", "*");
            response_->add_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS, HEAD, PATCH");
            response_->add_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
            response_->add_header("Access-Control-Allow-Credentials", "true");
        }

        compress_response_if_needed();

        if (auto conn = connection_.lock()) {
            if (auto str = stream_.lock()) {
                conn->handle_stream(str, response_);
            }
        }
        responded_ = true;
    }

    // WebSocket upgrade
    void upgrade_websocket(std::function<void(std::shared_ptr<websocket_connection>)> handler,
                          const std::set<std::string>& supported_protocols = {});

    // Server-Sent Events
    void start_sse(std::function<void(std::shared_ptr<sse_connection>)> handler);

    // File sending
    void send_file(const std::filesystem::path& path, bool force_download = false);
    
    // Redirect response
    void redirect(const std::string& url, http::http_response::status redirect_type = http::http_response::status::moved_temporarily);
    
    // Chunked response support
    void start_chunked(const std::string& content_type, http::http_response::status status = http::http_response::status::ok);
    void write_chunk(const std::string& data);
    void end_chunked();

    // Check if response has been sent
    bool has_responded() const {
        return responded_;
    }

    // Get the underlying connection (for advanced use cases)
    std::shared_ptr<server_connection> get_connection() const {
        return connection_.lock();
    }
};

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_RESPONSE_HPP
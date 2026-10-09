#include "response.hpp"
#include "request.hpp"
#include "mime_types.hpp"
#include "../../util/logger.hpp"
#include "../../util/base64.hpp"
#include "../../util/sha1.hpp"
#include "../../asio/sockets/websocket.hpp"
#include <boost/algorithm/string.hpp>
#include <fstream>
#include <sstream>
#include <atomic>

namespace thinger::http {

// State shared by the copies of a response. Each answer builds its own message and sends it
// only if it is the first one (responded); the others drop theirs
struct response::state {
    std::shared_ptr<response_sink> sink;
    std::shared_ptr<http_request> request;
    std::shared_ptr<http_response> draft;                       // status and headers set so far
    std::shared_ptr<const error_formatter> formatter;           // default format if null
    std::atomic<bool> responded{false};
    bool cors_enabled = false;
};

namespace {

    void add_cors_headers(http_response& response) {
        response.add_header("Access-Control-Allow-Origin", "*");
        response.add_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS, HEAD, PATCH");
        response.add_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
        response.add_header("Access-Control-Allow-Credentials", "true");
    }

    bool is_compressible_content_type(const std::string& content_type) {
        // Only compress text-based content types
        return content_type.starts_with("text/")
            || content_type.starts_with("application/json")
            || content_type.starts_with("application/xml")
            || content_type.starts_with("application/javascript")
            || content_type.starts_with("application/x-javascript")
            || content_type.starts_with("image/svg+xml");
    }

}

response::response(const std::shared_ptr<server_connection>& connection,
                   const std::shared_ptr<http_stream>& stream,
                   const std::shared_ptr<http::http_request>& http_request,
                   bool cors_enabled)
    : response(std::make_shared<connection_sink>(connection, stream), http_request, cors_enabled) {}

response::response(std::shared_ptr<response_sink> sink,
                   const std::shared_ptr<http::http_request>& http_request,
                   bool cors_enabled)
    : state_(std::make_shared<state>()) {
    state_->sink = std::move(sink);
    state_->request = http_request;
    state_->cors_enabled = cors_enabled;
}

bool response::ensure_not_responded() const {
    if (state_->responded) {
        LOG_ERROR("Response already sent");
        return false;
    }
    return true;
}

bool response::mark_responded() {
    if (state_->responded.exchange(true)) {
        LOG_ERROR("Response already sent");
        return false;
    }
    return true;
}

std::shared_ptr<http_response> response::new_response() const {
    auto message = state_->draft ? std::make_shared<http_response>(*state_->draft)
                                 : std::make_shared<http_response>();
    message->set_keep_alive(state_->request->keep_alive());
    if (!state_->draft && state_->cors_enabled) add_cors_headers(*message);
    return message;
}

http_response& response::draft() {
    if (!state_->draft) {
        state_->draft = std::make_shared<http_response>();
        if (state_->cors_enabled) add_cors_headers(*state_->draft);
    }
    return *state_->draft;
}

void response::compress_if_needed(http_response& response) const {

    // Only compress if there's a body worth compressing
    const auto& content = response.get_content();
    if (content.size() < 200) return;

    // Don't compress if already compressed
    if (response.has_header("Content-Encoding")) return;

    // Only compress text-based content types
    const auto& ct = response.get_content_type();
    if (ct.empty() || !is_compressible_content_type(ct)) return;

    // Check what the client accepts
    std::string accept_encoding = state_->request->get_header("Accept-Encoding");
    if (accept_encoding.empty()) return;

    if (accept_encoding.find("gzip") != std::string::npos) {
        auto compressed = ::thinger::util::gzip::compress(content);
        if (compressed) {
            response.set_content(std::move(*compressed));
            response.add_header("Content-Encoding", "gzip");
        }
    } else if (accept_encoding.find("deflate") != std::string::npos) {
        auto compressed = ::thinger::util::deflate::compress(content);
        if (compressed) {
            response.set_content(std::move(*compressed));
            response.add_header("Content-Encoding", "deflate");
        }
    }
}

void response::send_message(std::shared_ptr<http_response> message) {
    compress_if_needed(*message);
    if (!mark_responded()) return;
    state_->sink->send(std::move(message));
}

bool response::reject_takeover(const char* feature) {
    if (state_->sink->can_take_over()) return false;
    error(http::http_response::status::not_implemented,
          std::string(feature) + " is not available for in-memory requests");
    return true;
}

void response::json(const nlohmann::json& data, http::http_response::status status) {
    if (!ensure_not_responded()) return;
    auto message = new_response();
    message->set_status(status);
    message->set_content(data.dump(), "application/json");
    send_message(std::move(message));
}

void response::send(const std::string& text, const std::string& content_type) {
    if (!ensure_not_responded()) return;
    auto message = new_response();
    message->set_content(text, content_type);
    send_message(std::move(message));
}

void response::error(http::http_response::status status, const std::string& message,
                     const nlohmann::json& details) {
    if (!ensure_not_responded()) return;
    auto response = new_response();
    response->set_status(status);
    format_error(state_->formatter.get(), {status, message, details}, *response);
    send_message(std::move(response));
}

void response::set_error_formatter(std::shared_ptr<const error_formatter> formatter) {
    state_->formatter = std::move(formatter);
}

void response::status(http::http_response::status s) {
    if (!ensure_not_responded()) return;
    draft().set_status(s);
}

void response::header(const std::string& key, const std::string& value) {
    if (!ensure_not_responded()) return;
    draft().add_header(key, value);
}

void response::send_response(const std::shared_ptr<http_response>& response) {
    if (!ensure_not_responded()) return;
    response->set_keep_alive(state_->request->keep_alive());
    if (state_->cors_enabled) add_cors_headers(*response);
    send_message(response);
}

bool response::has_responded() const {
    return state_->responded;
}

std::shared_ptr<server_connection> response::get_connection() const {
    return state_->sink->connection();
}

// Redirect implementation
void response::redirect(const std::string& url, http::http_response::status redirect_type) {
    if (!ensure_not_responded()) return;
    auto response = new_response();
    response->set_status(redirect_type);
    response->add_header(header::location, url);
    send_message(std::move(response));
}

// File sending implementation
void response::send_file(const std::filesystem::path& path, bool force_download) {
    if (!ensure_not_responded()) return;
    
    // For now, we'll implement basic file sending here
    // TODO: Integrate with simple_file_handler when it's updated to work with response
    
    if (!std::filesystem::exists(path)) {
        error(http_response::status::not_found, "File not found");
        return;
    }
    
    if (!std::filesystem::is_regular_file(path)) {
        error(http_response::status::forbidden, "Not a regular file");
        return;
    }
    
    // Read file content
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error(http_response::status::internal_server_error, "Failed to open file");
        return;
    }
    
    // Get file size
    file.seekg(0, std::ios::end);
    auto file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    // Read file content
    std::string content(file_size, '\0');
    file.read(&content[0], file_size);
    
    // Determine content type using mime_types
    std::string content_type = mime_types::extension_to_type(path.extension().string());
    
    // Create response
    auto response = new_response();
    response->set_status(http_response::status::ok);
    response->set_content(std::move(content), content_type);
    
    // Add Content-Disposition header if force_download is true
    if (force_download) {
        response->add_header("Content-Disposition", "attachment; filename=\"" + path.filename().string() + "\"");
    }
    
    send_message(std::move(response));
}

// WebSocket upgrade implementation
void response::upgrade_websocket(std::function<void(std::shared_ptr<websocket_connection>)> handler,
                                const std::set<std::string>& supported_protocols) {
    if (!ensure_not_responded()) return;
    if (reject_takeover("WebSocket upgrade")) return;
    const auto& request = *state_->request;

    // Check if this is a WebSocket upgrade request
    if (!boost::iequals(request.get_header(http::header::upgrade), "websocket")) {
        error(http_response::status::upgrade_required, "This service requires use of WebSockets");
        return;
    }
    
    // Check WebSocket protocol if specified
    auto protocol = request.get_header("Sec-WebSocket-Protocol");
    if (!protocol.empty()) {
        LOG_DEBUG("Received WebSocket protocol: {}", protocol);
        if (!supported_protocols.contains(protocol)) {
            error(http_response::status::bad_request, "Unsupported WebSocket protocol");
            return;
        }
    } else if (!supported_protocols.empty()) {
        error(http_response::status::bad_request, "This method requires specifying a WebSocket protocol");
        return;
    }
    
    // Get WebSocket key
    auto ws_key = request.get_header("Sec-WebSocket-Key");
    if (ws_key.empty()) {
        error(http_response::status::bad_request, "Missing Sec-WebSocket-Key header");
        return;
    }
    
    // Calculate WebSocket accept key
    std::string accept_key = ws_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string hash = ::thinger::util::sha1::hash(accept_key);
    accept_key = ::thinger::util::base64::encode(hash);
    
    // Create upgrade response
    auto response = new_response();
    response->set_status(http_response::status::switching_protocols);
    response->add_header(header::upgrade, "websocket");
    response->add_header(header::connection, "Upgrade");
    response->add_header("Sec-WebSocket-Accept", accept_key);
    
    if (!protocol.empty()) {
        response->add_header("Sec-WebSocket-Protocol", protocol);
    }
    
    // Send the upgrade response and take over the connection
    take_over(std::move(response), [handler = std::move(handler)](std::shared_ptr<asio::socket> socket, std::string buffered) {
        // A client must wait for the handshake response before sending frames (RFC 6455)
        if (!buffered.empty()) {
            LOG_WARNING("WebSocket client sent data before the handshake completed, closing");
            socket->close();
            return;
        }

        // Create WebSocket from the socket
        auto websocket = std::make_shared<asio::websocket>(socket, true, true); // binary=true, server=true
        auto ws_connection = std::make_shared<websocket_connection>(websocket);

        // Call user handler
        handler(ws_connection);

        // Start the WebSocket connection
        ws_connection->start();
    });
}

void response::take_over(takeover_handler handler) {
    if (!ensure_not_responded()) return;
    if (reject_takeover("Connection takeover")) return;
    take_over(new_response(), std::move(handler));
}

void response::take_over(std::shared_ptr<http_response> message, takeover_handler handler) {
    compress_if_needed(*message);
    if (!mark_responded()) return;
    state_->sink->take_over(std::move(message), std::move(handler));
}

// Server-Sent Events implementation
void response::start_sse(std::function<void(std::shared_ptr<sse_connection>)> handler) {
    if (!ensure_not_responded()) return;
    if (reject_takeover("Server-Sent Events")) return;

    // Create SSE response headers
    auto response = new_response();
    response->set_status(http_response::status::ok);
    response->set_content_type("text/event-stream");
    response->add_header("Cache-Control", "no-cache");
    response->add_header("Connection", "keep-alive");
    response->add_header("X-Accel-Buffering", "no"); // Disable nginx buffering
    
    // Send the SSE headers and take over the connection (anything the client sends
    // afterwards is not part of the event stream)
    take_over(std::move(response), [handler = std::move(handler)](std::shared_ptr<asio::socket> socket, std::string) {
        // Create SSE connection
        auto sse_conn = std::make_shared<sse_connection>(socket);

        // Start the SSE connection
        sse_conn->start();

        // Call user handler
        handler(sse_conn);
    });
}

// Chunked response support: the sink writes the headers, then each chunk
bool response::start_chunked(const std::string& content_type, http::http_response::status status) {
    if (!ensure_not_responded()) return false;
    auto response = new_response();
    response->set_status(status);
    response->set_content_type(content_type);
    if (!mark_responded()) return false;
    return state_->sink->begin(std::move(response));
}

bool response::write_chunk(const std::string& data) {
    if (!state_->responded) {
        LOG_ERROR("Must call start_chunked() before writing chunks");
        return false;
    }
    return state_->sink->append(data);
}

bool response::end_chunked() {
    if (!state_->responded) {
        LOG_ERROR("Must call start_chunked() before ending chunks");
        return false;
    }
    return state_->sink->finish();
}

} // namespace thinger::http

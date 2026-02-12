#include "response.hpp"
#include "request.hpp"
#include "mime_types.hpp"
#include "../../util/logger.hpp"
#include "../../util/base64.hpp"
#include "../../util/sha1.hpp"
#include "../../asio/sockets/websocket.hpp"
#include <boost/algorithm/string.hpp>
#include "../common/http_data.hpp"
#include "../data/out_chunk.hpp"
#include <fstream>
#include <sstream>

namespace thinger::http {

// Redirect implementation
void response::redirect(const std::string& url, http::http_response::status redirect_type) {
    prepare_response();
    response_->set_status(redirect_type);
    response_->add_header(header::location, url);
    send_prepared_response();
}

// File sending implementation
void response::send_file(const std::filesystem::path& path, bool force_download) {
    ensure_not_responded();
    
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
    prepare_response();
    response_->set_status(http_response::status::ok);
    response_->set_content(content, content_type);
    
    // Add Content-Disposition header if force_download is true
    if (force_download) {
        response_->add_header("Content-Disposition", "attachment; filename=\"" + path.filename().string() + "\"");
    }
    
    send_prepared_response();
}

// WebSocket upgrade implementation
void response::upgrade_websocket(std::function<void(std::shared_ptr<websocket_connection>)> handler,
                                const std::set<std::string>& supported_protocols) {
    ensure_not_responded();
    
    auto conn = connection_.lock();
    auto str = stream_.lock();
    if (!conn || !str) {
        error(http_response::status::internal_server_error, "Connection lost");
        return;
    }
    
    // Check if this is a WebSocket upgrade request
    if (!boost::iequals(http_request_->get_header(http::header::upgrade), "websocket")) {
        error(http_response::status::upgrade_required, "This service requires use of WebSockets");
        return;
    }
    
    // Check WebSocket protocol if specified
    auto protocol = http_request_->get_header("Sec-WebSocket-Protocol");
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
    auto ws_key = http_request_->get_header("Sec-WebSocket-Key");
    if (ws_key.empty()) {
        error(http_response::status::bad_request, "Missing Sec-WebSocket-Key header");
        return;
    }
    
    // Calculate WebSocket accept key
    std::string accept_key = ws_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string hash = ::thinger::util::sha1::hash(accept_key);
    accept_key = ::thinger::util::base64::encode(hash);
    
    // Create upgrade response
    prepare_response();
    response_->set_status(http_response::status::switching_protocols);
    response_->add_header(header::upgrade, "websocket");
    response_->add_header(header::connection, "Upgrade");
    response_->add_header("Sec-WebSocket-Accept", accept_key);
    
    if (!protocol.empty()) {
        response_->add_header("Sec-WebSocket-Protocol", protocol);
    }
    
    // Register callback to be executed after response is sent
    str->on_completed([handler = std::move(handler), conn, str]() {
        // Release the socket from HTTP connection
        auto socket = conn->release_socket();
        if (!socket) {
            LOG_ERROR("Failed to release socket for WebSocket upgrade");
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
    
    // Send the upgrade response
    conn->handle_stream(str, response_);
    
    responded_ = true;
}

// Server-Sent Events implementation
void response::start_sse(std::function<void(std::shared_ptr<sse_connection>)> handler) {
    ensure_not_responded();
    
    auto conn = connection_.lock();
    auto str = stream_.lock();
    if (!conn || !str) {
        error(http_response::status::internal_server_error, "Connection lost");
        return;
    }
    
    // Create SSE response headers
    prepare_response();
    response_->set_status(http_response::status::ok);
    response_->set_content_type("text/event-stream");
    response_->add_header("Cache-Control", "no-cache");
    response_->add_header("Connection", "keep-alive");
    response_->add_header("X-Accel-Buffering", "no"); // Disable nginx buffering
    
    // Register callback to be executed after headers are sent
    str->on_completed([handler = std::move(handler), conn]() {
        // Release the socket from HTTP connection
        auto socket = conn->release_socket();
        if (!socket) {
            LOG_ERROR("Failed to release socket for SSE");
            return;
        }
        
        // Create SSE connection
        auto sse_conn = std::make_shared<sse_connection>(socket);
        
        // Start the SSE connection
        sse_conn->start();
        
        // Call user handler
        handler(sse_conn);
    });
    
    // Send the SSE headers
    conn->handle_stream(str, response_);
    
    responded_ = true;
}

// Chunked response support
void response::start_chunked(const std::string& content_type, http::http_response::status status) {
    ensure_not_responded();

    auto conn = connection_.lock();
    auto str = stream_.lock();
    if (!conn || !str) {
        throw std::runtime_error("Connection lost");
    }

    // Create chunked response headers
    prepare_response();
    response_->set_status(status);
    response_->set_content_type(content_type);
    response_->add_header("Transfer-Encoding", "chunked");
    response_->add_header("X-Content-Type-Options", "nosniff");
    response_->set_last_frame(false);

    // Send headers (stream stays open for subsequent chunks)
    conn->handle_stream(str, response_);

    responded_ = true;
}

void response::write_chunk(const std::string& data) {
    if (!responded_) {
        throw std::runtime_error("Must call start_chunked() before writing chunks");
    }

    auto conn = connection_.lock();
    auto str = stream_.lock();
    if (!conn || !str) {
        throw std::runtime_error("Connection lost");
    }

    auto chunk = std::make_shared<http_data>(std::make_shared<data::out_chunk>(data));
    chunk->set_last_frame(false);
    conn->handle_stream(str, chunk);
}

void response::end_chunked() {
    if (!responded_) {
        throw std::runtime_error("Must call start_chunked() before ending chunks");
    }

    auto conn = connection_.lock();
    auto str = stream_.lock();
    if (!conn || !str) {
        return;
    }

    // Send final zero-length chunk to terminate the response
    auto chunk = std::make_shared<http_data>(std::make_shared<data::out_chunk>());
    chunk->set_last_frame(true);
    conn->handle_stream(str, chunk);
}

} // namespace thinger::http
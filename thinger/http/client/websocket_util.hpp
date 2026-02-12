#ifndef THINGER_HTTP_WEBSOCKET_UTIL_HPP
#define THINGER_HTTP_WEBSOCKET_UTIL_HPP

#include <string>
#include <optional>

namespace thinger::http::websocket_util {

// WebSocket GUID for Sec-WebSocket-Accept calculation
inline constexpr const char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// URL components for WebSocket connections
struct url_components {
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
    bool secure;
};

/**
 * Parse a WebSocket URL (ws:// or wss://)
 * @return parsed components or nullopt if invalid
 */
std::optional<url_components> parse_websocket_url(const std::string& url);

/**
 * Generate a random WebSocket key for the handshake.
 */
std::string generate_websocket_key();

/**
 * Validate the Sec-WebSocket-Accept key from server response.
 */
bool validate_accept_key(const std::string& accept_key, const std::string& sent_key);

} // namespace thinger::http::websocket_util

#endif // THINGER_HTTP_WEBSOCKET_UTIL_HPP

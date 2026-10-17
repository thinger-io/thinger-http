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
#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace thinger::http {

// Forward declarations
class websocket_connection;
class sse_connection;
class http_application;
class virtual_host;
class response;
struct file_info;

namespace file_server {
    struct representation;
    void send(response& res, const representation& file);
}

// WebSocket upgrade that hands out the WebSocket as a byte stream socket
// (see response::upgrade_websocket_stream)
struct websocket_options {
    // Subprotocols the handler speaks, alternatives included (e.g. "mqtt", "mqttv3.1",
    // "mqttv3.1.1"). The first one the client offers (Sec-WebSocket-Protocol, in its order of
    // preference) that is in this list is accepted.
    std::vector<std::string> protocols;
    // With protocols: a client offering none of them is rejected with 400. Otherwise it is
    // accepted without a subprotocol (and the handler gets an empty one)
    bool protocol_required = true;
    // Messages written: binary, or text
    bool binary = true;
    // Ping the client once nothing was received from it for this long (0: never)
    std::chrono::milliseconds ping_interval{0};
    // Close the connection once nothing was received for this long, pongs included (0: never)
    std::chrono::milliseconds idle_timeout{0};
};

// Receives the WebSocket and the subprotocol negotiated (empty if none)
using websocket_stream_handler = sync_callback<void(std::shared_ptr<asio::websocket> socket, std::string protocol)>;
using websocket_stream_coroutine = std::function<awaitable<void>(std::shared_ptr<asio::websocket> socket, std::string protocol)>;

// Receive a WebSocket with a message interface, and an event stream
using websocket_handler = sync_callback<void(std::shared_ptr<websocket_connection>)>;
using websocket_coroutine = std::function<awaitable<void>(std::shared_ptr<websocket_connection>)>;
using sse_handler = sync_callback<void(std::shared_ptr<sse_connection>)>;
using sse_coroutine = std::function<awaitable<void>(std::shared_ptr<sse_connection>)>;

// Response to a request. It is a handle: copies share the response, so a handler may keep
// a copy and answer later, from any thread. Only the first answer is sent: the others are
// dropped, even when they run at the same time from several threads. Setting the status or
// headers before answering (status(), header()) is not thread-safe: do it from the thread
// running the handler, before any answer (a route timeout answers with the headers set so far).
// A response to a HEAD request is the one to GET without its body: every answer sends its
// headers, Content-Length included (of the body uncompressed: HEAD is never compressed), and
// drops the body; chunks and streamed data are dropped too, and SSE sends its headers alone.
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

    // The value as JSON, MessagePack, CBOR or UBJSON, as the Accept header of the request
    // prefers (by quality, then in that order); JSON if it accepts none of them or is missing.
    // The response has "Vary: Accept".
    void encode(const nlohmann::json& data, http::http_response::status status = http::http_response::status::ok);

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

    // Set status code (for building custom responses: used by the answers that do not set
    // their own, such as send() and take_over(); not thread-safe)
    void status(http::http_response::status s);

    // Add a header to the response sent, whatever answer sends it (not thread-safe)
    void header(const std::string& key, const std::string& value);

    // Send raw http_response object (for advanced use cases). Its content is sent whole,
    // framed by a Content-Length of its size (any Content-Length or Transfer-Encoding it
    // carries is replaced, e.g. when copied from another response); 1xx, 204 and 304 are
    // sent without body, and 1xx and 204 without Content-Length
    void send_response(const std::shared_ptr<http_response>& response);

    // WebSocket upgrade, with a message interface. With supported_protocols, the client must
    // offer one of them (400 otherwise), and the first one it offers is accepted; without, a
    // client offering a subprotocol is rejected with 400. The handler is called on the
    // io_context of the connection, before the WebSocket starts reading (set its callbacks
    // there). A coroutine handler runs on that io_context too: the WebSocket starts reading
    // once it completes.
    void upgrade_websocket(websocket_handler handler, const std::set<std::string>& supported_protocols = {});
    void upgrade_websocket(websocket_coroutine handler, const std::set<std::string>& supported_protocols = {});

    // WebSocket upgrade handing out the WebSocket as a byte stream socket, for a protocol
    // running on it (MQTT, a custom one...) or a tunnel (socket_pipe): binary messages by
    // default, read as one continuous stream (see asio::websocket). The handler is called on
    // the io_context of the socket once the handshake response is written, with the WebSocket
    // (holding any frames the client sent right after its request) and the subprotocol
    // negotiated (see websocket_options). Rejected with 426 if the request is not a
    // WebSocket upgrade, or 400 if its key is missing or no subprotocol matches. The handler
    // can be a coroutine, run on that io_context.
    void upgrade_websocket_stream(const websocket_options& options, websocket_stream_handler handler);
    void upgrade_websocket_stream(const websocket_options& options, websocket_stream_coroutine handler);
    void upgrade_websocket_stream(websocket_stream_handler handler) {
        upgrade_websocket_stream(websocket_options{}, std::move(handler));
    }
    void upgrade_websocket_stream(websocket_stream_coroutine handler) {
        upgrade_websocket_stream(websocket_options{}, std::move(handler));
    }

    // Server-Sent Events (for a HEAD request, the headers alone: the handler is not called).
    // The handler is called on the io_context of the connection once the event stream
    // started; a coroutine handler runs on that io_context.
    void start_sse(sse_handler handler);
    void start_sse(sse_coroutine handler);

    // Take over the connection (e.g. for a custom protocol upgrade or a tunnel): sends
    // this response (status and headers set so far, 200 by default), then the server
    // stops handling the connection and passes the socket to `handler`, together with
    // any bytes already read past this request. The handler fully owns the socket. If the
    // connection was closed before (e.g. the client closed or half-closed it while the
    // response was pending), the handler is not called, only released. A coroutine
    // handler runs on the io_context of the socket. Unless it switches protocols (101, or
    // 2xx to CONNECT) or has its own framing (Content-Length or Transfer-Encoding set with
    // header()), the response says "Connection: close": what the handler writes is read as
    // its body, until the connection closes.
    void take_over(takeover_handler handler);
    void take_over(takeover_coroutine handler);

    // File response, with the media type of its extension. It carries a weak ETag and a
    // Last-Modified from the size and modification time of the file, and Cache-Control:
    // no-cache unless one was set before (header()): caches keep the file, but check it on
    // every use. GET and HEAD requests with If-None-Match (or else If-Modified-Since) are
    // answered with 304 Not Modified while it does not change; a GET with a single byte range
    // (Range, and If-Range with the Last-Modified date) with 206 Partial Content, or 416 if
    // it starts past the end. 404 if the file does not exist, 403 if it is not a regular file.
    // With force_download, Content-Disposition: attachment.
    void send_file(const std::filesystem::path& path, bool force_download = false);

    // Redirect response
    void redirect(const std::string& url, http::http_response::status redirect_type = http::http_response::status::moved_temporarily);
    
    // Chunked response: start_chunked() sends the headers (false if another answer was sent
    // first, or the connection is gone), then write_chunk() each chunk and end_chunked() the
    // last one. Chunks are only written on a response started with start_chunked(), once it
    // returned true (from any copy, any thread) and until end_chunked(): otherwise they
    // return false and write nothing. For a HEAD request, start_chunked() sends the headers
    // (without Transfer-Encoding) as a whole response, and the chunks are dropped.
    bool start_chunked(const std::string& content_type, http::http_response::status status = http::http_response::status::ok);
    bool write_chunk(const std::string& data);
    bool end_chunked();

    // Streamed response, with flow control (e.g. to relay a body as it arrives from
    // elsewhere). start_stream() sends the head: the headers set so far (header()) and those
    // of `head`, with the status of `head`. Its framing is set here (Content-Length,
    // Transfer-Encoding and Connection headers in `head` are ignored): with `content_length`
    // the body has that length; without it, it is chunked, or, for HTTP/1.0 clients, ended
    // by closing the connection. Responses to HEAD requests, and with status 1xx, 204 or
    // 304, have no body (`content_length` is only advertised, except for 1xx and 204).
    // Then write_stream() each part of the body: it completes once the part is written to
    // the connection, so the body goes at the pace of the client. end_stream() completes
    // the response (false if the body is shorter than its Content-Length, then closing the
    // connection), or abort_stream() leaves it incomplete for the client (closing the
    // connection) if the body cannot be produced. They return false if the response cannot
    // be written: another answer was sent first, the connection is gone, more data than
    // its Content-Length or data for a response without body. Use one writer at a time.
    bool start_stream(const http_response& head, std::optional<size_t> content_length = std::nullopt);
    awaitable<bool> write_stream(std::string data);
    awaitable<bool> end_stream();
    void abort_stream();

    // Check if response has been sent
    bool has_responded() const;

    // Get the underlying connection (for advanced use cases); null for requests
    // dispatched in memory
    std::shared_ptr<server_connection> get_connection() const;

private:
    struct state;

    // The request timeout of the route (see route::timeout) is run by the application
    friend class http_application;
    // Files are answered by the file server, with the messages of this response
    friend void file_server::send(response& res, const file_server::representation& file);

    // The request answered, and the formatter of its errors (null for the default one)
    const http_request& request_message() const;
    const error_formatter* formatter() const;

    // Call `hook` once the response starts (the first answer), from the thread answering.
    // Set before the response is handed to the handler.
    void on_start(std::function<void()> hook);

    // Answer the request as timed out, with an error of `status`, unless another answer
    // started first (false then). Later answers are dropped quietly.
    bool answer_timeout(http::http_response::status status);

    // Whether the response was answered by answer_timeout()
    bool timed_out() const;

    // An answer arriving once the response was sent (or chunks out of place): logged as an
    // error, unless the request timed out (then the handler is just late)
    void log_late_answer(const char* message) const;

    bool ensure_not_responded() const;
    // Status and headers set before answering, created on first use
    http_response& draft();
    // New message to answer with: a copy of the draft, if any
    std::shared_ptr<http_response> new_response() const;
    // Whether the request is HEAD: its response has no body
    bool head_request() const;
    // Body of a message about to be sent: framed (see frame_message), dropped for HEAD
    // (keeping its Content-Length), else compressed if `compress` and the client accepts it
    // (see compression::compress)
    void prepare_body(http_response& message, bool compress) const;
    // Framing of a message sent whole: its content is its body, with its Content-Length
    [[gnu::noinline, gnu::cold]] void frame_message(http_response& message) const;
    // Framing of the response of a connection taken over (see take_over)
    void frame_takeover(http_response& message) const;
    // Mark the response as sent; false (and nothing must be sent) if it already was
    bool mark_responded();
    // Send a message (see prepare_body), unless another answer was sent first
    void send_message(std::shared_ptr<http_response> message, bool compress = true);
    // Send a message and take the connection over, unless another answer was sent first
    void take_over(std::shared_ptr<http_response> message, takeover_handler handler);
    // Answers 501 if the connection cannot be taken over (in-memory requests)
    bool reject_takeover(const char* feature);
    // Upgrade to a WebSocket with a message interface, and to an event stream: `deliver`
    // receives the connection, and the socket it runs on (whose executor runs `deliver`)
    void accept_websocket(const std::set<std::string>& supported_protocols,
                          std::function<void(std::shared_ptr<websocket_connection>, asio::websocket&)> deliver);
    void accept_sse(std::function<void(std::shared_ptr<sse_connection>, asio::socket&)> deliver);
    // Checks of a WebSocket upgrade request and the subprotocol negotiated: the 101 response
    // to send, or null if the request was answered with an error
    std::shared_ptr<http_response> websocket_handshake(const std::vector<std::string>& protocols,
                                                       bool protocol_required, bool reject_unknown_protocol,
                                                       std::string& protocol);

    std::shared_ptr<state> state_;
};

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_RESPONSE_HPP
#include "response.hpp"
#include "request.hpp"
#include "mime_types.hpp"
#include "body_decoders.hpp"
#include "file_server/file_info.hpp"
#include "../../util/logger.hpp"
#include "../../util/base64.hpp"
#include "../../util/sha1.hpp"
#include "../../asio/sockets/websocket.hpp"
#include <boost/algorithm/string.hpp>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <atomic>
#include <mutex>
#include <optional>

namespace thinger::http {

// State shared by the copies of a response. Each answer builds its own message and sends it
// only if it is the first one (responded); the others drop theirs
struct response::state {
    std::shared_ptr<response_sink> sink;
    std::shared_ptr<http_request> request;
    std::optional<http_response> draft;                         // status and headers set so far
    std::shared_ptr<const error_formatter> formatter;           // default format if null
    std::atomic<bool> responded{false};
    bool cors_enabled = false;

    // Request timeout: called once the response starts, and whether it was answered by it
    std::function<void()> on_start;
    std::atomic<bool> timed_out{false};

    // Chunked response: chunks are only written between its headers and its end
    enum class chunked_phase { none, open, ended };
    std::mutex chunked_mutex;
    chunked_phase chunked = chunked_phase::none;

    // Streamed response (under chunked_mutex too): its phase, framing, and the bytes left
    // of a body with Content-Length
    chunked_phase stream = chunked_phase::none;
    body_framing stream_framing = body_framing::none;
    size_t stream_remaining = 0;
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

void response::log_late_answer(const char* message) const {
    if (state_->timed_out) {
        LOG_DEBUG("{} (the request timed out)", message);
    } else {
        LOG_ERROR("{}", message);
    }
}

bool response::ensure_not_responded() const {
    if (state_->responded) {
        log_late_answer("Response already sent");
        return false;
    }
    return true;
}

bool response::mark_responded() {
    if (state_->responded.exchange(true)) {
        log_late_answer("Response already sent");
        return false;
    }
    if (state_->on_start) state_->on_start();
    return true;
}

void response::on_start(std::function<void()> hook) {
    state_->on_start = std::move(hook);
}

bool response::answer_timeout(http::http_response::status status) {
    if (state_->responded) return false;
    // (set before answering: whoever sees the response sent sees why)
    state_->timed_out = true;
    auto message = new_response();
    message->set_status(status);
    format_error(state_->formatter.get(), {status, "No response within the timeout", nullptr}, *message);
    if (!state_->responded.exchange(true)) {
        if (state_->on_start) state_->on_start();
        prepare_body(*message, false);
        state_->sink->send(std::move(message));
        return true;
    }
    // Another answer started meanwhile: it was not late
    state_->timed_out = false;
    return false;
}

bool response::timed_out() const {
    // (a handler may have moved the response away)
    return state_ && state_->timed_out;
}

std::shared_ptr<http_response> response::new_response() const {
    auto message = state_->draft ? std::make_shared<http_response>(*state_->draft)
                                 : std::make_shared<http_response>();
    // A protocol switch keeps the Connection header set for it (Upgrade)
    if (message->get_status() != http_response::status::switching_protocols) {
        message->set_keep_alive(state_->request->keep_alive());
    }
    if (!state_->draft && state_->cors_enabled) add_cors_headers(*message);
    return message;
}

http_response& response::draft() {
    if (!state_->draft) {
        state_->draft.emplace();
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

bool response::head_request() const {
    return state_->request->get_method() == method::HEAD;
}

namespace {

    // Whether a message sent whole is framed as response::frame_message() leaves it: with a
    // Content-Length (set with its content) and no Transfer-Encoding, and a status with body
    bool framed(const http_response& message) {
        int code = message.get_status_code();
        return message.has_content_length_header() && !message.has_transfer_encoding_header() &&
               code >= 200 && code != 204 && code != 304;
    }

}

void response::frame_message(http_response& message) const {
    // As Node does, a message sent whole tells where its body ends, else the client would
    // read it until the connection closes (RFC 9112, 6.3): its Content-Length, never
    // Transfer-Encoding (its body is not chunked). 1xx, 204 and 304 never have a body, nor
    // its length for 1xx and 204 (RFC 9110, 8.6); a 304 may tell the length of the
    // representation. A response to HEAD gets the length of the content it was given, if
    // any: it has no body anyway.
    int code = message.get_status_code();
    if (code < 200 || code == 204 || code == 304) {
        message.get_content().clear();
        if (message.has_transfer_encoding_header()) message.remove_headers(header::transfer_encoding);
        if (code != 304 && message.has_content_length_header()) message.remove_headers(header::content_length);
        return;
    }
    if (message.has_transfer_encoding_header()) message.remove_headers(header::transfer_encoding);
    if (!message.has_content_length_header() && (!head_request() || !message.get_content().empty())) {
        message.set_content_length(message.get_content().size());
    }
}

void response::frame_takeover(http_response& message) const {
    // What follows the response belongs to the handler. Switching protocols (101, or 2xx to
    // CONNECT: a tunnel), nothing tells a body (RFC 9110, 9.3.6). Otherwise, unless the
    // handler framed it, what the handler writes is the body, ending when the connection
    // closes: the response says so, as Node does for a body of unknown length not chunked
    int code = message.get_status_code();
    bool tunnel = code == 101 || (state_->request->get_method() == method::CONNECT && code >= 200 && code < 300);
    if (tunnel) {
        message.remove_headers(header::content_length);
        message.remove_headers(header::transfer_encoding);
    } else if (!message.has_content_length_header() && !message.has_transfer_encoding_header()) {
        message.set_keep_alive(false);
    }
}

void response::prepare_body(http_response& message, bool compress) const {
    // (the usual answer is framed already: its Content-Length was set with its content)
    if (!framed(message)) [[unlikely]] {
        frame_message(message);
    }
    if (head_request()) {
        // The headers of the answer to GET, Content-Length included, without its body: the
        // client reads none (RFC 9110, 9.3.2), so it would be taken as the next response.
        // Not compressed (like Express): its length is the one of the identity body.
        message.get_content().clear();
    } else if (compress) {
        compress_if_needed(message);
    }
}

void response::send_message(std::shared_ptr<http_response> message, bool compress) {
    prepare_body(*message, compress);
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

void response::encode(const nlohmann::json& data, http::http_response::status status) {
    if (!ensure_not_responded()) return;
    // Repeated Accept headers make one list
    std::string accept;
    if (state_->request) {
        for (const auto& value : state_->request->get_headers_with_key(header::accept)) {
            if (!accept.empty()) accept += ", ";
            accept += value;
        }
    }
    auto encoding = negotiate_response_encoding(accept);
    auto message = new_response();
    message->set_status(status);
    message->set_content(encode_body(data, encoding.format), encoding.media_type);
    message->add_header("Vary", "Accept");
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
    // A message built by hand may carry the framing of another one (e.g. copied from a
    // response whose body was decoded): its content is its body, sent whole (a response
    // to HEAD keeps the length it tells)
    int code = response->get_status_code();
    if (!head_request() && code != 304 && response->has_content_length_header()) {
        response->remove_headers(header::content_length);
    }
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
    // (without it, a keep-alive client would read the body until the connection closes)
    response->set_content_length(0);
    send_message(std::move(response));
}

// File sending: the whole file, a byte range of it, or 304 if the client has it already
void response::send_file(const std::filesystem::path& path, bool force_download) {
    if (!ensure_not_responded()) return;

    // One stat for the checks, the length and the validators
    auto file = file_info::of(path);
    if (!file) {
        error(http_response::status::not_found, "File not found");
        return;
    }
    if (!file->regular) {
        error(http_response::status::forbidden, "Not a regular file");
        return;
    }

    const auto& request = *state_->request;
    bool head = request.get_method() == method::HEAD;
    bool get = request.get_method() == method::GET;
    const auto& content_type = mime_types::extension_to_type(path.extension().string());

    // Headers of the 200 and 304 answers alike: the validators, and how to cache them.
    // Without a Cache-Control of the handler, caches store the file but revalidate it on
    // every use (else they could reuse it for a while, guessing from Last-Modified).
    auto message = new_response();
    if (!message->has_header(header::cache_control)) message->add_header(header::cache_control, "no-cache");
    message->set_header("ETag", file->etag);
    message->set_header("Last-Modified", file->last_modified);
    // a text file may be sent compressed (see compress_if_needed): another representation
    if (file->size >= 200 && is_compressible_content_type(content_type)) message->add_header("Vary", "Accept-Encoding");

    // Conditional request for a file the client has: no body, nor the length of the file
    if ((get || head) && file->not_modified(request)) {
        message->set_status(http_response::status::not_modified);
        send_message(std::move(message), false);
        return;
    }

    message->set_status(http_response::status::ok);
    message->set_content_type(content_type);
    message->add_header("Accept-Ranges", "bytes");
    if (force_download) {
        message->add_header("Content-Disposition", "attachment; filename=\"" + path.filename().string() + "\"");
    }

    // HEAD: the headers of the GET answer (with the length of the file, uncompressed), no body
    if (head) {
        message->set_content_length(file->size);
        send_message(std::move(message), false);
        return;
    }

    // A single byte range of a GET request (RFC 9110, 14.2), unless If-Range asks for the
    // whole file once it changed: only the date can match, as the entity tag is weak
    std::optional<byte_range> range;
    const auto& range_header = request.get_header("Range");
    if (get && !range_header.empty()) {
        const auto& if_range = request.get_header("If-Range");
        if (if_range.empty() || if_range == file->last_modified) range = parse_byte_range(range_header, file->size);
    }
    if (range && range->length == 0) {
        auto unsatisfiable = new_response();
        unsatisfiable->set_status(http_response::status::range_not_satisfiable);
        unsatisfiable->add_header("Content-Range", "bytes */" + std::to_string(file->size));
        format_error(state_->formatter.get(), {http_response::status::range_not_satisfiable,
                                               "Range not satisfiable", nullptr}, *unsatisfiable);
        send_message(std::move(unsatisfiable));
        return;
    }

    uint64_t first = range ? range->first : 0;
    uint64_t length = range ? range->length : file->size;
    std::ifstream stream(path, std::ios::binary);
    std::string content(length, '\0');
    if (!stream || !stream.seekg(static_cast<std::streamoff>(first)) ||
        !stream.read(content.data(), static_cast<std::streamsize>(length))) {
        error(http_response::status::internal_server_error, "Failed to read file");
        return;
    }
    message->set_content(std::move(content));

    // A range is sent as is: compressing it would change the bytes it refers to
    if (range) {
        message->set_status(http_response::status::partial_content);
        message->add_header("Content-Range", "bytes " + std::to_string(first) + "-" +
                                             std::to_string(first + length - 1) + "/" + std::to_string(file->size));
        send_message(std::move(message), false);
        return;
    }
    send_message(std::move(message));
}

namespace {

    // Subprotocols offered by the client, in its order of preference: the comma-separated
    // values of its Sec-WebSocket-Protocol headers
    std::vector<std::string> offered_protocols(const http_request& request) {
        std::vector<std::string> offered;
        for (const auto& value : request.get_headers_with_key("Sec-WebSocket-Protocol")) {
            std::vector<std::string> tokens;
            boost::split(tokens, value, boost::is_any_of(","));
            for (auto& token : tokens) {
                boost::trim(token);
                if (!token.empty()) offered.push_back(std::move(token));
            }
        }
        return offered;
    }

    // Whether a comma-separated header (any of its values) holds `token` (case-insensitive)
    bool has_token(const std::vector<std::string>& values, std::string_view token) {
        for (const auto& value : values) {
            std::vector<std::string> tokens;
            boost::split(tokens, value, boost::is_any_of(","));
            for (auto& candidate : tokens) {
                boost::trim(candidate);
                if (boost::iequals(candidate, token)) return true;
            }
        }
        return false;
    }

    // A Sec-WebSocket-Key: 16 bytes in base64 (RFC 6455, 4.1)
    bool valid_websocket_key(const std::string& key) {
        if (key.size() != 24 || !key.ends_with("==")) return false;
        for (size_t i = 0; i < 22; ++i) {
            char c = key[i];
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '/') return false;
        }
        return ::thinger::util::base64::decode(key).size() == 16;
    }

}

std::shared_ptr<http_response> response::websocket_handshake(const std::vector<std::string>& protocols,
                                                             bool protocol_required, bool reject_unknown_protocol,
                                                             std::string& protocol) {
    const auto& request = *state_->request;

    // Check if this is a WebSocket upgrade request (RFC 6455, 4.2.1)
    if (!boost::iequals(request.get_header(http::header::upgrade), "websocket")) {
        error(http_response::status::upgrade_required, "This service requires use of WebSockets");
        return nullptr;
    }
    if (request.get_method() != method::GET) {
        error(http_response::status::bad_request, "A WebSocket handshake must be a GET request");
        return nullptr;
    }
    if (!has_token(request.get_headers_with_key(http::header::connection), "upgrade")) {
        error(http_response::status::bad_request, "A WebSocket handshake requires Connection: Upgrade");
        return nullptr;
    }
    if (request.get_header("Sec-WebSocket-Version") != "13") {
        header("Sec-WebSocket-Version", "13");
        error(http_response::status::upgrade_required, "Unsupported WebSocket version");
        return nullptr;
    }

    // Subprotocol: the first one offered by the client that is supported
    auto offered = offered_protocols(request);
    protocol.clear();
    for (const auto& candidate : offered) {
        if (std::find(protocols.begin(), protocols.end(), candidate) != protocols.end()) {
            protocol = candidate;
            break;
        }
    }
    if (protocol.empty()) {
        if (!offered.empty() && (reject_unknown_protocol || (protocol_required && !protocols.empty()))) {
            LOG_DEBUG("Unsupported WebSocket protocol: {}", request.get_header("Sec-WebSocket-Protocol"));
            error(http_response::status::bad_request, "Unsupported WebSocket protocol");
            return nullptr;
        }
        if (offered.empty() && protocol_required && !protocols.empty()) {
            error(http_response::status::bad_request, "This method requires specifying a WebSocket protocol");
            return nullptr;
        }
    }

    // Get WebSocket key
    auto ws_key = request.get_header("Sec-WebSocket-Key");
    if (!valid_websocket_key(ws_key)) {
        error(http_response::status::bad_request, "Missing or invalid Sec-WebSocket-Key header");
        return nullptr;
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
    return response;
}

namespace {

    // Run a coroutine handler on `io_context`, detached: what it throws is logged
    template<typename F>
    void spawn_handler(boost::asio::io_context& io_context, const char* what, F&& run) {
        co_spawn(io_context, std::forward<F>(run), [what](const std::exception_ptr& error) {
            if (!error) return;
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& e) {
                LOG_ERROR("Exception in the {} handler: {}", what, e.what());
            } catch (...) {
                LOG_ERROR("Unknown exception in the {} handler", what);
            }
        });
    }

}

// WebSocket upgrade implementation
void response::upgrade_websocket(websocket_handler handler, const std::set<std::string>& supported_protocols) {
    accept_websocket(supported_protocols, [handler = std::move(handler)](std::shared_ptr<websocket_connection> connection,
                                                                        asio::socket&) {
        handler(connection);
        connection->start();
    });
}

void response::upgrade_websocket(websocket_coroutine handler, const std::set<std::string>& supported_protocols) {
    accept_websocket(supported_protocols, [handler = std::move(handler)](std::shared_ptr<websocket_connection> connection,
                                                                        asio::socket& socket) mutable {
        spawn_handler(socket.get_io_context(), "WebSocket",
                      [handler = std::move(handler), connection]() mutable -> awaitable<void> {
            co_await handler(connection);
            connection->start();
        });
    });
}

void response::accept_websocket(const std::set<std::string>& supported_protocols,
                                std::function<void(std::shared_ptr<websocket_connection>, asio::socket&)> deliver) {
    if (!ensure_not_responded()) return;
    if (reject_takeover("WebSocket upgrade")) return;

    std::string protocol;
    auto response = websocket_handshake({supported_protocols.begin(), supported_protocols.end()},
                                        true, true, protocol);
    if (!response) return;

    // Send the upgrade response and take over the connection
    take_over(std::move(response), [deliver = std::move(deliver), protocol](std::shared_ptr<asio::socket> socket, std::string buffered) {
        // A client must wait for the handshake response before sending frames (RFC 6455)
        if (!buffered.empty()) {
            LOG_WARNING("WebSocket client sent data before the handshake completed, closing");
            socket->close();
            return;
        }

        // Create WebSocket from the socket
        auto websocket = std::make_shared<asio::websocket>(socket, true, true); // binary=true, server=true
        websocket->set_protocol(protocol);
        wrap_connection(*socket, *websocket, connection_type::websocket);
        auto ws_connection = std::make_shared<websocket_connection>(websocket);

        // The handler sets its callbacks, then the WebSocket starts
        deliver(ws_connection, *socket);
    });
}

void response::upgrade_websocket_stream(const websocket_options& options, websocket_stream_coroutine handler) {
    upgrade_websocket_stream(options, websocket_stream_handler(
        [handler = std::move(handler)](std::shared_ptr<asio::websocket> websocket, std::string protocol) mutable {
            auto& io_context = websocket->get_io_context();
            spawn_handler(io_context, "WebSocket",
                          [handler = std::move(handler), websocket = std::move(websocket),
                           protocol = std::move(protocol)]() mutable {
                return handler(std::move(websocket), std::move(protocol));
            });
        }));
}

void response::upgrade_websocket_stream(const websocket_options& options, websocket_stream_handler handler) {
    if (!ensure_not_responded()) return;
    if (reject_takeover("WebSocket upgrade")) return;

    std::string protocol;
    auto response = websocket_handshake(options.protocols, options.protocol_required, false, protocol);
    if (!response) return;

    take_over(std::move(response), [handler = std::move(handler), options, protocol](std::shared_ptr<asio::socket> socket, std::string buffered) {
        auto websocket = std::make_shared<asio::websocket>(socket, options.binary, true);
        // (the handler may ask to be told of a shutdown on the websocket: see on_server_shutdown())
        wrap_connection(*socket, *websocket, connection_type::websocket);
        websocket->set_protocol(protocol);
        // Frames the client sent right after its request, without waiting for the response
        if (!buffered.empty()) websocket->set_read_ahead(std::move(buffered));
        websocket->set_keepalive(options.ping_interval, options.idle_timeout);
        handler(std::move(websocket), protocol);
    });
}

void response::take_over(takeover_handler handler) {
    if (!ensure_not_responded()) return;
    if (reject_takeover("Connection takeover")) return;
    take_over(new_response(), std::move(handler));
}

void response::take_over(takeover_coroutine handler) {
    take_over(takeover_handler([handler = std::move(handler)](std::shared_ptr<asio::socket> socket,
                                                             std::string buffered) mutable {
        auto& io_context = socket->get_io_context();
        spawn_handler(io_context, "takeover",
                      [handler = std::move(handler), socket = std::move(socket), buffered = std::move(buffered)]() mutable {
            return handler(std::move(socket), std::move(buffered));
        });
    }));
}

void response::take_over(std::shared_ptr<http_response> message, takeover_handler handler) {
    frame_takeover(*message);
    if (!mark_responded()) return;
    state_->sink->take_over(std::move(message), std::move(handler));
}

// Server-Sent Events implementation
void response::start_sse(sse_handler handler) {
    accept_sse([handler = std::move(handler)](std::shared_ptr<sse_connection> connection, asio::socket&) {
        handler(std::move(connection));
    });
}

void response::start_sse(sse_coroutine handler) {
    accept_sse([handler = std::move(handler)](std::shared_ptr<sse_connection> connection, asio::socket& socket) mutable {
        spawn_handler(socket.get_io_context(), "SSE", [handler = std::move(handler), connection = std::move(connection)]() mutable {
            return handler(std::move(connection));
        });
    });
}

void response::accept_sse(std::function<void(std::shared_ptr<sse_connection>, asio::socket&)> deliver) {
    if (!ensure_not_responded()) return;
    if (reject_takeover("Server-Sent Events")) return;

    // Create SSE response headers
    auto response = new_response();
    response->set_status(http_response::status::ok);
    response->set_content_type("text/event-stream");
    response->add_header("Cache-Control", "no-cache");
    response->add_header("X-Accel-Buffering", "no"); // Disable nginx buffering

    // HEAD: the headers of the event stream alone, as a whole response (no events, nor
    // handler), so the connection goes on with the next request
    if (head_request()) {
        send_message(std::move(response), false);
        return;
    }

    // Send the SSE headers and take over the connection (anything the client sends
    // afterwards is not part of the event stream)
    take_over(std::move(response), [deliver = std::move(deliver)](std::shared_ptr<asio::socket> socket, std::string) {
        // Create SSE connection
        auto sse_conn = std::make_shared<sse_connection>(socket);

        // Start the SSE connection
        sse_conn->start();

        // Call user handler
        deliver(std::move(sse_conn), *socket);
    });
}

// Chunked response support: the sink writes the headers, then each chunk. Chunks are
// written in order (the lock), only once the headers are queued and until the end. A
// response to HEAD is the headers alone, as a whole response: its chunks are dropped
bool response::start_chunked(const std::string& content_type, http::http_response::status status) {
    if (!ensure_not_responded()) return false;
    auto response = new_response();
    response->set_status(status);
    response->set_content_type(content_type);
    if (!mark_responded()) return false;

    std::lock_guard lock(state_->chunked_mutex);
    if (head_request()) {
        // (without Transfer-Encoding, as start_stream() does)
        state_->sink->send(std::move(response));
    } else if (!state_->sink->begin(std::move(response))) {
        return false;
    }
    state_->chunked = state::chunked_phase::open;
    return true;
}

bool response::write_chunk(const std::string& data) {
    std::lock_guard lock(state_->chunked_mutex);
    if (state_->chunked != state::chunked_phase::open) {
        log_late_answer("Chunks can only be written between start_chunked() and end_chunked()");
        return false;
    }
    if (head_request()) return true;
    return state_->sink->append(data);
}

bool response::end_chunked() {
    std::lock_guard lock(state_->chunked_mutex);
    if (state_->chunked != state::chunked_phase::open) {
        log_late_answer("Only a response started with start_chunked() can be ended, once");
        return false;
    }
    state_->chunked = state::chunked_phase::ended;
    if (head_request()) return true;
    return state_->sink->finish();
}


// Streamed response: the sink writes the head, then each part of the body once the previous
// one is written. The response checks the body against its framing, so a writer can never
// put more on the connection than the head announced (it would be read as what follows)
bool response::start_stream(const http_response& head, std::optional<size_t> content_length) {
    if (!ensure_not_responded()) return false;
    auto message = new_response();
    message->set_status(head.get_status());
    message->set_reason_phrase(head.get_reason());
    // the headers of `head` replace those with the same name set before (e.g. CORS)
    auto is_framing = [](const std::string& key) {
        return headers::is_header(key, header::content_length) || headers::is_header(key, header::transfer_encoding) ||
               headers::is_header(key, header::connection);
    };
    for (const auto& [key, value] : head.get_headers()) {
        if (!is_framing(key)) message->remove_headers(key);
    }
    for (const auto& [key, value] : head.get_headers()) {
        if (!is_framing(key)) message->add_header(key, value);
    }
    // headers set before (header()) may carry framing too
    message->remove_headers(header::content_length);
    message->remove_headers(header::transfer_encoding);

    const auto& request = *state_->request;
    int code = message->get_status_code();
    bool no_body = request.get_method() == method::HEAD || (code >= 100 && code < 200) || code == 204 || code == 304;
    bool http_1_1 = request.get_http_version_major() > 1 ||
                    (request.get_http_version_major() == 1 && request.get_http_version_minor() >= 1);

    body_framing framing;
    if (no_body) {
        framing = body_framing::none;
        // the length of the resource (HEAD, 304), but never for 1xx and 204
        if (content_length && code >= 200 && code != 204) message->set_content_length(*content_length);
    } else if (content_length) {
        framing = body_framing::length;
        message->set_content_length(*content_length);
    } else if (http_1_1) {
        framing = body_framing::chunked;
    } else {
        framing = body_framing::close;
    }

    if (!mark_responded()) return false;

    std::lock_guard lock(state_->chunked_mutex);
    if (!state_->sink->begin_stream(std::move(message), framing)) return false;
    state_->stream = state::chunked_phase::open;
    state_->stream_framing = framing;
    state_->stream_remaining = framing == body_framing::length ? *content_length : 0;
    return true;
}

awaitable<bool> response::write_stream(std::string data) {
    {
        std::lock_guard lock(state_->chunked_mutex);
        if (state_->stream != state::chunked_phase::open) {
            log_late_answer("Stream data can only be written between start_stream() and end_stream()");
            co_return false;
        }
        if (state_->stream_framing == body_framing::none) {
            if (!data.empty()) LOG_ERROR("Response without body: stream data dropped");
            co_return data.empty();
        }
        if (state_->stream_framing == body_framing::length) {
            if (data.size() > state_->stream_remaining) {
                LOG_ERROR("Stream data exceeds the Content-Length of the response: dropped");
                co_return false;
            }
            state_->stream_remaining -= data.size();
        }
    }
    // (an empty chunk would end a chunked body)
    if (data.empty()) co_return true;
    co_return co_await state_->sink->write_stream(std::move(data));
}

awaitable<bool> response::end_stream() {
    {
        std::lock_guard lock(state_->chunked_mutex);
        if (state_->stream != state::chunked_phase::open) {
            log_late_answer("Only a response started with start_stream() can be ended, once");
            co_return false;
        }
        state_->stream = state::chunked_phase::ended;
        if (state_->stream_framing == body_framing::length && state_->stream_remaining > 0) {
            LOG_ERROR("Stream ended {} bytes before its Content-Length: closing the connection",
                      state_->stream_remaining);
            state_->sink->abort_stream();
            co_return false;
        }
    }
    co_return co_await state_->sink->end_stream();
}

void response::abort_stream() {
    std::lock_guard lock(state_->chunked_mutex);
    if (state_->stream != state::chunked_phase::open) return;
    state_->stream = state::chunked_phase::ended;
    state_->sink->abort_stream();
}

} // namespace thinger::http


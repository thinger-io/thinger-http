#include "response.hpp"
#include "request.hpp"
#include "mime_types.hpp"
#include "body_decoders.hpp"
#include "response_compression.hpp"
#include "file_server/file_info.hpp"
#include "file_server/file_response.hpp"
#include "../common/syntax.hpp"
#include "../../util/ascii.hpp"
#include "../../util/logger.hpp"
#include "../../util/base64.hpp"
#include "../../util/sha1.hpp"
#include "../../asio/sockets/websocket.hpp"
#include <algorithm>
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
    // Set before answering: whoever sees the response sent sees why
    state_->timed_out = true;
    if (state_->responded.exchange(true)) {
        // Another answer started meanwhile: it was not late
        state_->timed_out = false;
        return false;
    }
    if (state_->on_start) state_->on_start();
    auto message = take_draft();
    message->set_status(status);
    format_error(state_->formatter.get(), {status, "No response within the timeout", nullptr}, *message);
    prepare_body(*message, false);
    state_->sink->send(std::move(message));
    return true;
}

bool response::timed_out() const {
    // A handler may have moved the response away
    return state_ && state_->timed_out;
}

std::shared_ptr<http_response> response::new_response() {
    if (!mark_responded()) return nullptr;
    return take_draft();
}

std::shared_ptr<http_response> response::take_draft() {
    // Moved, not copied: only the answer sent reads it, once it is the first one
    auto message = state_->draft ? std::make_shared<http_response>(std::move(*state_->draft))
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
    // The usual answer is framed already: its Content-Length was set with its content
    if (!framed(message)) [[unlikely]] {
        frame_message(message);
    }
    if (head_request()) {
        // The headers of the answer to GET, Content-Length included, without its body: the
        // client reads none (RFC 9110, 9.3.2), so it would be taken as the next response.
        // Not compressed (like Express): its length is the one of the identity body.
        if (compress && compression::compressible(message)) compression::add_vary(message, "Accept-Encoding");
        message.get_content().clear();
    } else if (compress && message.get_content().size() >= compression::threshold) {
        compression::compress(message, *state_->request);
    }
}

void response::send_message(std::shared_ptr<http_response> message, bool compress) {
    prepare_body(*message, compress);
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
    // Before taking the draft: dump() throws for strings that are not UTF-8, and the error
    // answered then keeps the headers set
    auto body = data.dump();
    auto message = new_response();
    if (!message) return;
    message->set_status(status);
    message->set_content(std::move(body), "application/json");
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
    // Before taking the draft, as json()
    auto body = encode_body(data, encoding.format);
    auto message = new_response();
    if (!message) return;
    message->set_status(status);
    message->set_content(std::move(body), encoding.media_type);
    message->add_header("Vary", "Accept");
    send_message(std::move(message));
}

void response::send(const std::string& text, const std::string& content_type) {
    if (!ensure_not_responded()) return;
    auto message = new_response();
    if (!message) return;
    message->set_content(text, content_type);
    send_message(std::move(message));
}

void response::error(http::http_response::status status, const std::string& message,
                     const nlohmann::json& details) {
    if (!ensure_not_responded()) return;
    auto response = new_response();
    if (!response) return;
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

void response::header(std::string key, std::string value) {
    if (!ensure_not_responded()) return;
    draft().add_header(std::move(key), std::move(value));
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
    if (!mark_responded()) return;
    send_message(response);
}

const http_request& response::request_message() const {
    return *state_->request;
}

const error_formatter* response::formatter() const {
    return state_->formatter.get();
}

bool response::has_responded() const {
    return state_->responded;
}

std::shared_ptr<server_connection> response::get_connection() const {
    return state_->sink->connection();
}

void response::redirect(const std::string& url, http::http_response::status redirect_type) {
    if (!ensure_not_responded()) return;
    auto response = new_response();
    if (!response) return;
    response->set_status(redirect_type);
    response->add_header(header::location, url);
    // Without it, a keep-alive client would read the body until the connection closes
    response->set_content_length(0);
    send_message(std::move(response));
}

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
    file_server::send(*this, {path, *file, mime_types::extension_to_type(path.extension().string()), {}, force_download});
}

namespace {

    using ::thinger::util::ascii::iequals;

    // Why a WebSocket handshake is rejected: the error answered, and whether the client is
    // told the version supported
    struct handshake_rejection {
        http_response::status status;
        const char* message;
        bool unsupported_version = false;
    };

    // Checks of a WebSocket handshake request (RFC 6455, 4.2.1) before its subprotocol
    std::optional<handshake_rejection> check_handshake(const http_request& request) {
        using status = http_response::status;
        if (!iequals(request.get_header(header::upgrade), "websocket")) {
            return handshake_rejection{status::upgrade_required, "This service requires use of WebSockets"};
        }
        if (request.get_method() != method::GET) {
            return handshake_rejection{status::bad_request, "A WebSocket handshake must be a GET request"};
        }
        bool upgrade = false;
        for (const auto& value : request.get_headers_with_key(header::connection)) {
            upgrade = upgrade || syntax::any_element(value, [](std::string_view option) { return iequals(option, "upgrade"); });
        }
        if (!upgrade) return handshake_rejection{status::bad_request, "A WebSocket handshake requires Connection: Upgrade"};
        if (request.get_header("Sec-WebSocket-Version") != "13") {
            return handshake_rejection{status::upgrade_required, "Unsupported WebSocket version", true};
        }
        return std::nullopt;
    }

    // Subprotocols offered by the client, in its order of preference: the comma-separated
    // values of its Sec-WebSocket-Protocol headers
    std::vector<std::string> offered_protocols(const http_request& request) {
        std::vector<std::string> offered;
        for (const auto& value : request.get_headers_with_key("Sec-WebSocket-Protocol")) {
            syntax::for_each_element(value, [&](std::string_view protocol) { offered.emplace_back(protocol); });
        }
        return offered;
    }

    // The first subprotocol offered that is supported, empty if none
    std::string select_protocol(const std::vector<std::string>& offered, const std::vector<std::string>& supported) {
        for (const auto& candidate : offered) {
            if (std::find(supported.begin(), supported.end(), candidate) != supported.end()) return candidate;
        }
        return {};
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

    std::string websocket_accept(const std::string& key) {
        return ::thinger::util::base64::encode(::thinger::util::sha1::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
    }

}

std::shared_ptr<http_response> response::websocket_handshake(const std::vector<std::string>& protocols,
                                                             bool protocol_required, bool reject_unknown_protocol,
                                                             std::string& protocol) {
    const auto& request = *state_->request;
    if (auto rejection = check_handshake(request)) {
        if (rejection->unsupported_version) header("Sec-WebSocket-Version", "13");
        error(rejection->status, rejection->message);
        return nullptr;
    }

    auto offered = offered_protocols(request);
    protocol = select_protocol(offered, protocols);
    bool required = protocol_required && !protocols.empty();
    if (protocol.empty() && !offered.empty() && (reject_unknown_protocol || required)) {
        LOG_DEBUG("Unsupported WebSocket protocol: {}", request.get_header("Sec-WebSocket-Protocol"));
        error(http_response::status::bad_request, "Unsupported WebSocket protocol");
        return nullptr;
    }
    if (offered.empty() && required) {
        error(http_response::status::bad_request, "This method requires specifying a WebSocket protocol");
        return nullptr;
    }

    const auto& key = request.get_header("Sec-WebSocket-Key");
    if (!valid_websocket_key(key)) {
        error(http_response::status::bad_request, "Missing or invalid Sec-WebSocket-Key header");
        return nullptr;
    }

    auto response = new_response();
    if (!response) return nullptr;
    response->set_status(http_response::status::switching_protocols);
    response->add_header(header::upgrade, "websocket");
    response->add_header(header::connection, "Upgrade");
    response->add_header("Sec-WebSocket-Accept", websocket_accept(key));
    if (!protocol.empty()) response->add_header("Sec-WebSocket-Protocol", protocol);
    return response;
}

namespace {

    // Run a coroutine handler on `executor` (or an io_context), detached: what it throws is logged
    template<typename Executor, typename F>
    void spawn_handler(Executor&& executor, const char* what, F&& run) {
        co_spawn(std::forward<Executor>(executor), std::forward<F>(run), [what](const std::exception_ptr& error) {
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

void response::upgrade_websocket(websocket_handler handler, const std::set<std::string>& supported_protocols) {
    accept_websocket(supported_protocols, [handler = std::move(handler)](std::shared_ptr<websocket_connection> connection,
                                                                        asio::websocket&) {
        handler(connection);
        connection->start();
    });
}

void response::upgrade_websocket(websocket_coroutine handler, const std::set<std::string>& supported_protocols) {
    accept_websocket(supported_protocols, [handler = std::move(handler)](std::shared_ptr<websocket_connection> connection,
                                                                        asio::websocket& websocket) mutable {
        spawn_handler(websocket.get_executor(), "WebSocket",
                      [handler = std::move(handler), connection]() mutable -> awaitable<void> {
            co_await handler(connection);
            connection->start();
        });
    });
}

void response::accept_websocket(const std::set<std::string>& supported_protocols,
                                std::function<void(std::shared_ptr<websocket_connection>, asio::websocket&)> deliver) {
    if (!ensure_not_responded()) return;
    if (reject_takeover("WebSocket upgrade")) return;

    std::string protocol;
    auto response = websocket_handshake({supported_protocols.begin(), supported_protocols.end()},
                                        true, true, protocol);
    if (!response) return;

    take_over(std::move(response), [deliver = std::move(deliver), protocol](std::shared_ptr<asio::socket> socket, std::string buffered) {
        // A client must wait for the handshake response before sending frames (RFC 6455)
        if (!buffered.empty()) {
            LOG_WARNING("WebSocket client sent data before the handshake completed, closing");
            socket->close();
            return;
        }

        // binary, server; on the io_context of the connection, where it is taken over (the
        // default executor)
        auto websocket = std::make_shared<asio::websocket>(socket, true, true);
        websocket->set_protocol(protocol);
        wrap_connection(*socket, *websocket, connection_type::websocket);
        auto ws_connection = std::make_shared<websocket_connection>(websocket);

        // The handler sets its callbacks, then the WebSocket starts
        deliver(ws_connection, *websocket);
    });
}

void response::upgrade_websocket_stream(const websocket_options& options, websocket_stream_coroutine handler) {
    upgrade_websocket_stream(options, websocket_stream_handler(
        [handler = std::move(handler)](std::shared_ptr<asio::websocket> websocket, std::string protocol) mutable {
            auto executor = websocket->get_executor();
            spawn_handler(std::move(executor), "WebSocket",
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
        // On the io_context of the connection, where it is taken over (the default executor)
        auto websocket = std::make_shared<asio::websocket>(socket, options.binary, true);
        // The handler may ask to be told of a shutdown on the websocket (on_server_shutdown())
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
    if (auto message = new_response()) take_over(std::move(message), std::move(handler));
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
    state_->sink->take_over(std::move(message), std::move(handler));
}

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

    auto response = new_response();
    if (!response) return;
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
        auto sse_conn = std::make_shared<sse_connection>(socket);
        sse_conn->start();
        deliver(std::move(sse_conn), *socket);
    });
}

// Chunked response support: the sink writes the headers, then each chunk. Chunks are
// written in order (the lock), only once the headers are queued and until the end. A
// response to HEAD is the headers alone, as a whole response: its chunks are dropped
bool response::start_chunked(const std::string& content_type, http::http_response::status status) {
    if (!ensure_not_responded()) return false;
    auto response = new_response();
    if (!response) return false;
    response->set_status(status);
    response->set_content_type(content_type);

    std::lock_guard lock(state_->chunked_mutex);
    if (head_request()) {
        // Without Transfer-Encoding, as start_stream() does
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


namespace {

    bool is_framing_header(std::string_view name) {
        return headers::is_header(name, header::content_length) || headers::is_header(name, header::transfer_encoding) ||
               headers::is_header(name, header::connection);
    }

    // Status and headers of the head of a streamed response: those of `head` replace the ones
    // with the same name set before (e.g. CORS); the framing is the response's own
    void apply_head(http_response& message, const http_response& head) {
        message.set_status(head.get_status());
        message.set_reason_phrase(head.get_reason());
        for (const auto& [key, value] : head.get_headers()) {
            if (!is_framing_header(key)) message.remove_headers(key);
        }
        for (const auto& [key, value] : head.get_headers()) {
            if (!is_framing_header(key)) message.add_header(key, value);
        }
        // headers set before (header()) may carry framing too
        message.remove_headers(header::content_length);
        message.remove_headers(header::transfer_encoding);
    }

    bool is_http_1_1(const http_request& request) {
        return request.get_http_version_major() > 1 ||
               (request.get_http_version_major() == 1 && request.get_http_version_minor() >= 1);
    }

    // Framing of a streamed response, with its Content-Length if it has one
    body_framing frame_stream(http_response& message, const http_request& request,
                              std::optional<size_t> content_length) {
        int code = message.get_status_code();
        bool no_body = request.get_method() == method::HEAD || (code >= 100 && code < 200) || code == 204 || code == 304;
        if (no_body) {
            // the length of the resource (HEAD, 304), but never for 1xx and 204
            if (content_length && code >= 200 && code != 204) message.set_content_length(*content_length);
            return body_framing::none;
        }
        if (content_length) {
            message.set_content_length(*content_length);
            return body_framing::length;
        }
        return is_http_1_1(request) ? body_framing::chunked : body_framing::close;
    }

}

// Streamed response: the sink writes the head, then each part of the body once the previous
// one is written. The response checks the body against its framing, so a writer can never
// put more on the connection than the head announced (it would be read as what follows)
bool response::start_stream(const http_response& head, std::optional<size_t> content_length) {
    if (!ensure_not_responded()) return false;
    auto message = new_response();
    if (!message) return false;
    apply_head(*message, head);
    auto framing = frame_stream(*message, *state_->request, content_length);

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
    // An empty chunk would end a chunked body
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


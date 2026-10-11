#ifndef THINGER_HTTP_SERVER_BODY_DECODERS_HPP
#define THINGER_HTTP_SERVER_BODY_DECODERS_HPP

#include "../common/http_response.hpp"
#include <nlohmann/json.hpp>
#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace thinger::http {

// Media type of a Content-Type (or Accept) value: "type/subtype" and its parameters
// (RFC 9110, 8.3.1), e.g. "Application/JSON; charset=\"UTF-8\"" -> "application/json",
// {"charset", "UTF-8"}
struct media_type {
    // "type/subtype", lowercase
    std::string type;
    // Parameters in order: names lowercase, values unquoted (case kept)
    std::vector<std::pair<std::string, std::string>> parameters;

    // Parse a media type; nullopt if it is malformed. Media ranges ("*/*", "text/*") are
    // accepted too, for Accept headers.
    static std::optional<media_type> parse(std::string_view value);

    // Value of a parameter (the first one with that name), empty if missing
    std::string parameter(std::string_view name) const;
};

// Limits applied while decoding a body, against inputs that are small but expensive to
// decode or to use. The body size itself is limited by the server (set_max_body_size).
// Besides these, a body never decodes to more values than it has bytes (every value takes
// at least one byte in JSON, MessagePack and CBOR; UBJSON can declare arrays of values
// without payload, such as a billion nulls in a few bytes, which are rejected).
struct body_limits {
    // Maximum nesting of arrays and objects (a scalar is at depth 0, [] or {} at depth 1).
    // Decoding is iterative, but the decoded value is often used recursively (schema
    // validation, dump(), copies, comparisons), so an unlimited depth could overflow the stack.
    size_t max_depth = 128;
};

// A body that cannot be decoded: 400 Bad Request if it is invalid, or 415 Unsupported Media
// Type if its type (or charset) is not supported. The message is sent to the client.
class body_decode_error : public std::runtime_error {
public:
    explicit body_decode_error(const std::string& message,
                               http_response::status status = http_response::status::bad_request)
        : std::runtime_error(message), status_(status) {}

    http_response::status status() const { return status_; }

private:
    http_response::status status_;
};

// Decodes a body of a media type into a JSON value, within the limits. Throws
// body_decode_error if it cannot; any other exception is answered as an invalid body (400).
using body_decoder = std::function<nlohmann::json(std::string_view body, const media_type& type,
                                                  const body_limits& limits)>;

// A body format: a decoder and the media types it decodes
struct body_format {
    // Name the routes accept it by, e.g. "msgpack"
    std::string name;
    // Media types it decodes (lowercase), e.g. "application/msgpack", "application/x-msgpack"
    std::vector<std::string> media_types;
    // Structured syntax suffix it also decodes, e.g. "+json" for "application/problem+json"
    // (empty if none)
    std::string suffix;
    body_decoder decode;
};

// Decoders of request bodies by media type, used for the routes taking a decoded body
// (see route::accepts). Built in, each one removable:
//
//   name      media types                                    decoded as
//   json      application/json (and any +json)               the JSON value
//   msgpack   application/msgpack, application/x-msgpack     the value (bin/ext as binary)
//   cbor      application/cbor (and any +cbor)               the value (byte strings as binary,
//                                                            tagged items rejected)
//   ubjson    application/ubjson                             the value
//   form      application/x-www-form-urlencoded              an object of strings (repeated
//                                                            names: an array of strings)
//   text      text/plain                                     a string (valid UTF-8)
//   binary    application/octet-stream                       a binary value (json::binary)
//
// JSON, form and text take no charset other than UTF-8 (415); strings in every format must
// be valid UTF-8 (400 otherwise), so a decoded value can always be dumped. Configure it
// before serving: it is read concurrently by the worker threads.
class body_decoder_registry {
public:
    // With the built-in decoders
    body_decoder_registry();

    // Add a decoder, replacing the one with the same name (if any); its media types (and
    // suffix) are removed from any other decoder
    body_decoder_registry& add(const std::string& name, std::vector<std::string> media_types,
                               body_decoder decoder, std::string suffix = {});

    // Remove a decoder (e.g. a built-in one): bodies of its media types are no longer
    // decoded, so routes accepting them answer 415. Returns false if there was none.
    bool remove(const std::string& name);

    // Decoder by name, null if none
    const body_format* find(std::string_view name) const;

    // Decoder for a media type ("type/subtype", lowercase): by its media types, or else by
    // its suffix; null if none
    const body_format* find_media_type(std::string_view type) const;

    // A media type exactly as a decoder lists it, without parameters (as most clients send
    // their Content-Type, e.g. "application/json"): that media type, parsed when the decoder
    // was added, and its decoder; {null, null} if it is not one. A Content-Type found here
    // needs no parsing on each request.
    std::pair<const media_type*, const body_format*> find_plain_media_type(std::string_view type) const;

    const std::vector<body_format>& formats() const { return formats_; }

    // The built-in decoders, to reuse in custom ones
    static nlohmann::json decode_json(std::string_view body, const media_type& type, const body_limits& limits);
    static nlohmann::json decode_msgpack(std::string_view body, const media_type& type, const body_limits& limits);
    static nlohmann::json decode_cbor(std::string_view body, const media_type& type, const body_limits& limits);
    static nlohmann::json decode_ubjson(std::string_view body, const media_type& type, const body_limits& limits);
    static nlohmann::json decode_form(std::string_view body, const media_type& type, const body_limits& limits);
    static nlohmann::json decode_text(std::string_view body, const media_type& type, const body_limits& limits);
    static nlohmann::json decode_binary(std::string_view body, const media_type& type, const body_limits& limits);

private:
    // Index the plain media types of the decoders (after adding or removing one)
    void index_plain_media_types();

    std::vector<body_format> formats_;
    // The media types of the decoders that are plain (lowercase "type/subtype"), parsed,
    // with the index of their decoder in formats_
    std::vector<std::pair<media_type, size_t>> plain_media_types_;
};

// How the bodies of the routes are decoded: the application settings (see
// http_application::body_decoders), shared by its virtual hosts
struct body_settings {
    body_decoder_registry decoders;
    // Types accepted by routes taking a decoded body that do not set their own (route::accepts)
    std::vector<std::string> accepted{"json"};
    body_limits limits;

    // Defaults, for routes handled without an application
    static const body_settings& defaults();
};

// Media type assumed for a body without Content-Type: JSON, as before body decoders existed
inline constexpr std::string_view default_body_media_type = "application/json";

// Result of checking the Content-Type of a body against the types a route accepts
struct body_type_match {
    // Whether the body is accepted; if not, `error` tells why (for a 415 answer)
    bool accepted = false;
    std::string error;
    // Media type of the body (default_body_media_type without Content-Type)
    media_type type;
    // Decoder for it, null if none is registered (a type accepted only by a route reading
    // the body itself)
    const body_format* format = nullptr;
};

// Check a Content-Type header value (empty if missing) against the accepted types: names of
// decoders in the registry, or media types. A type is accepted if its media type is listed,
// or the name of its decoder.
body_type_match match_body_type(const body_decoder_registry& decoders,
                                const std::vector<std::string>& accepted,
                                std::string_view content_type);

// Media types of a list of accepted types (the media types of each decoder named, and the
// media types listed), in order and without repetitions, e.g. for API documentation
std::vector<std::string> accepted_media_types(const body_decoder_registry& decoders,
                                              const std::vector<std::string>& accepted);

// Response encoding chosen by the Accept header of a request, among JSON, MessagePack, CBOR
// and UBJSON (see response::encode)
struct response_encoding {
    std::string media_type;     // Content-Type to send, e.g. "application/msgpack"
    std::string format;         // "json", "msgpack", "cbor" or "ubjson"
};

// The encoding the Accept header (empty if missing) prefers, by quality and then in the
// order JSON, MessagePack, CBOR, UBJSON; JSON if it accepts none of them
response_encoding negotiate_response_encoding(std::string_view accept);

// Encode a value in a format of negotiate_response_encoding()
std::string encode_body(const nlohmann::json& value, const std::string& format);

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_BODY_DECODERS_HPP

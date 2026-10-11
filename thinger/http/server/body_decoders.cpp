#include "body_decoders.hpp"
#include "../util/url.hpp"
#include "../util/utf8.hpp"
#include "../../util/ascii.hpp"
#include <algorithm>
#include <array>
#include <cstdint>

namespace thinger::http {

namespace {

    using json = nlohmann::json;

    // RFC 9110, 5.6.2: token characters (bit 1), in a table, as media types are parsed on
    // every request with a body. Bit 2: those of a plain media type (see is_plain_media_type),
    // all but uppercase letters and '*'.
    constexpr std::array<uint8_t, 256> token_table = [] {
        std::array<uint8_t, 256> table{};
        for (int c = '0'; c <= '9'; ++c) table[c] = 3;
        for (int c = 'a'; c <= 'z'; ++c) table[c] = 3;
        for (int c = 'A'; c <= 'Z'; ++c) table[c] = 1;
        for (unsigned char c : std::string_view("!#$%&'+-.^_`|~")) table[c] = 3;
        table['*'] = 1;
        return table;
    }();

    bool is_tchar(char c) {
        return token_table[static_cast<unsigned char>(c)] & 1;
    }

    // "type/subtype" of token characters in lowercase, without wildcards, whitespace nor
    // parameters (as most clients send it): media_type::parse() reads it as it is
    bool is_plain_media_type(std::string_view value) {
        size_t slash = 0;   // (none: a slash first is not valid either)
        for (size_t i = 0; i < value.size(); ++i) {
            if (token_table[static_cast<unsigned char>(value[i])] & 2) continue;
            if (value[i] != '/' || i == 0 || slash) return false;
            slash = i;
        }
        return slash > 0 && slash + 1 < value.size();
    }

    bool is_ows(char c) {
        return c == ' ' || c == '\t';
    }

    // RFC 9110, 5.6.4: qdtext, and the characters of a quoted-pair after the backslash
    bool is_qdtext(char c) {
        auto u = static_cast<unsigned char>(c);
        return u == '\t' || u == ' ' || u == 0x21 || (u >= 0x23 && u <= 0x5B) || (u >= 0x5D && u <= 0x7E) || u >= 0x80;
    }

    bool is_quoted_pair_char(char c) {
        auto u = static_cast<unsigned char>(c);
        return u == '\t' || u == ' ' || (u >= 0x21 && u <= 0x7E) || u >= 0x80;
    }

    std::string lowercase(std::string_view value) {
        std::string result(value);
        ::thinger::util::ascii::to_lower(result);
        return result;
    }

    std::string_view trim(std::string_view value) {
        while (!value.empty() && is_ows(value.front())) value.remove_prefix(1);
        while (!value.empty() && is_ows(value.back())) value.remove_suffix(1);
        return value;
    }

    bool valid_utf8(std::string_view value) {
        return utf8::is_valid(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }

    // Charsets other than UTF-8 are not converted: rejected as an unsupported media type
    void require_utf8(const media_type& type, const std::string& what, bool allow_ascii = false) {
        auto charset = type.parameter("charset");
        if (charset.empty() || ::thinger::util::ascii::iequals(charset, "utf-8")) return;
        if (allow_ascii && ::thinger::util::ascii::iequals(charset, "us-ascii")) return;
        throw body_decode_error("Unsupported charset for " + what + " (only UTF-8)",
                                http_response::status::unsupported_media_type);
    }

    // Builds the decoded value from the SAX events of nlohmann::json (for JSON and the binary
    // formats alike), within the limits: nesting depth, number of values (at most one per
    // byte of input) and, for the binary formats, UTF-8 strings. Containers are filled
    // through a stack, never recursively, and decoding stops at the first event over a limit.
    class limited_dom_builder {
    public:
        enum class failure { none, syntax, depth, values, utf8 };

        limited_dom_builder(json& root, size_t max_depth, size_t max_values, bool check_utf8)
            : root_(root), max_depth_(max_depth), max_values_(max_values), check_utf8_(check_utf8) {}

        bool null() { return add(nullptr); }
        bool boolean(bool value) { return add(value); }
        bool number_integer(json::number_integer_t value) { return add(value); }
        bool number_unsigned(json::number_unsigned_t value) { return add(value); }
        bool number_float(json::number_float_t value, const json::string_t&) { return add(value); }

        bool string(json::string_t& value) {
            if (check_utf8_ && !valid_utf8(value)) return fail(failure::utf8);
            return add(std::move(value));
        }

        bool binary(json::binary_t& value) {
            return add(std::move(value));
        }

        bool start_object(std::size_t) { return open(json::value_t::object); }

        bool key(json::string_t& value) {
            if (check_utf8_ && !valid_utf8(value)) return fail(failure::utf8);
            // a repeated key keeps the last value, as json::parse() does
            element_ = &stack_.back()->get_ref<json::object_t&>()[value];
            return true;
        }

        bool end_object() { stack_.pop_back(); return true; }
        bool start_array(std::size_t) { return open(json::value_t::array); }
        bool end_array() { stack_.pop_back(); return true; }

        bool parse_error(std::size_t, const std::string&, const json::exception&) {
            return fail(failure::syntax);
        }

        failure get_failure() const { return failure_; }

    private:
        bool fail(failure reason) {
            if (failure_ == failure::none) failure_ = reason;
            return false;
        }

        bool open(json::value_t type) {
            if (stack_.size() >= max_depth_) return fail(failure::depth);
            if (!add(type)) return false;
            stack_.push_back(last_);
            return true;
        }

        // Add a value where it goes (the root, the open array, or the last key of the open
        // object), built there from the parsed value; false if over the limit of values.
        // Pointers to open containers stay valid: their parents get no other element while
        // they are open.
        template<typename Value>
        bool add(Value&& value) {
            if (++values_ > max_values_) return fail(failure::values);
            if (stack_.empty()) {
                root_ = json(std::forward<Value>(value));
                last_ = &root_;
            } else if (auto* array = stack_.back()->template get_ptr<json::array_t*>()) {
                last_ = &array->emplace_back(std::forward<Value>(value));
            } else {
                *element_ = json(std::forward<Value>(value));
                last_ = element_;
            }
            return true;
        }

        json& root_;
        std::vector<json*> stack_;
        json* element_ = nullptr;
        // the value added last
        json* last_ = nullptr;
        size_t max_depth_;
        size_t max_values_;
        size_t values_ = 0;
        bool check_utf8_;
        failure failure_ = failure::none;
    };

    json decode_with_limits(std::string_view body, json::input_format_t format, const body_limits& limits,
                            const char* name) {
        json result;
        limited_dom_builder builder(result, limits.max_depth, body.size(), format != json::input_format_t::json);
        bool decoded = false;
        try {
            decoded = json::sax_parse(body.data(), body.data() + body.size(), &builder, format, true);
        } catch (const std::exception&) {
            // the parser reports errors through the builder, but fail safe on any exception
            decoded = false;
        }
        if (decoded) return result;

        switch (builder.get_failure()) {
            case limited_dom_builder::failure::depth:
                throw body_decode_error(std::string("Invalid ") + name + ": nested deeper than " +
                                        std::to_string(limits.max_depth) + " levels");
            case limited_dom_builder::failure::values:
                throw body_decode_error(std::string("Invalid ") + name + ": more values than bytes");
            case limited_dom_builder::failure::utf8:
                throw body_decode_error(std::string("Invalid ") + name + ": string not valid UTF-8");
            default:
                throw body_decode_error(std::string("Invalid ") + name);
        }
    }

    // Quality of an Accept media range (RFC 9110, 12.4.2) in thousandths; -1 if invalid
    int parse_quality(std::string_view value) {
        if (value.empty() || value.size() > 5) return -1;
        if (value[0] != '0' && value[0] != '1') return -1;
        int quality = (value[0] - '0') * 1000;
        if (value.size() == 1) return quality;
        if (value[1] != '.') return -1;
        int scale = 100;
        for (size_t i = 2; i < value.size(); ++i, scale /= 10) {
            if (value[i] < '0' || value[i] > '9') return -1;
            quality += (value[i] - '0') * scale;
        }
        return quality <= 1000 ? quality : -1;
    }

}

// --- media_type ---

std::optional<media_type> media_type::parse(std::string_view value) {
    // (parsed on every request with a body: the usual value, such as "application/json",
    // needs neither a copy in lowercase nor a parameter list)
    if (is_plain_media_type(value)) return media_type{std::string(value), {}};

    size_t i = 0;
    const size_t n = value.size();
    auto skip_ows = [&]() { while (i < n && is_ows(value[i])) ++i; };
    auto token = [&]() {
        size_t start = i;
        while (i < n && is_tchar(value[i])) ++i;
        return value.substr(start, i - start);
    };

    skip_ows();
    size_t start = i;
    auto type = token();
    if (type.empty() || i >= n || value[i] != '/') return std::nullopt;
    ++i;
    auto subtype = token();
    if (subtype.empty()) return std::nullopt;
    // "*/subtype" is not a media range
    if (type == "*" && subtype != "*") return std::nullopt;

    // "type/subtype" as sent, lowercase
    media_type result;
    result.type.assign(value.substr(start, i - start));
    ::thinger::util::ascii::to_lower(result.type);

    skip_ows();
    while (i < n) {
        if (value[i] != ';') return std::nullopt;
        ++i;
        skip_ows();
        if (i == n) break;
        if (value[i] == ';') continue;   // empty parameter

        auto name = token();
        if (name.empty() || i >= n || value[i] != '=') return std::nullopt;
        ++i;

        std::string parameter_value;
        if (i < n && value[i] == '"') {
            ++i;
            bool closed = false;
            while (i < n) {
                char c = value[i++];
                if (c == '"') {
                    closed = true;
                    break;
                }
                if (c == '\\') {
                    if (i >= n || !is_quoted_pair_char(value[i])) return std::nullopt;
                    c = value[i++];
                } else if (!is_qdtext(c)) {
                    return std::nullopt;
                }
                parameter_value += c;
            }
            if (!closed) return std::nullopt;
        } else {
            auto token_value = token();
            if (token_value.empty()) return std::nullopt;
            parameter_value = token_value;
        }
        result.parameters.emplace_back(lowercase(name), std::move(parameter_value));
        skip_ows();
    }
    return result;
}

std::string media_type::parameter(std::string_view name) const {
    for (const auto& [key, value] : parameters) {
        if (::thinger::util::ascii::iequals(key, name)) return value;
    }
    return {};
}

// --- body_decoder_registry ---

body_decoder_registry::body_decoder_registry() {
    add("json", {"application/json"}, decode_json, "+json");
    add("msgpack", {"application/msgpack", "application/x-msgpack"}, decode_msgpack);
    add("cbor", {"application/cbor"}, decode_cbor, "+cbor");
    add("ubjson", {"application/ubjson"}, decode_ubjson);
    add("form", {"application/x-www-form-urlencoded"}, decode_form);
    add("text", {"text/plain"}, decode_text);
    add("binary", {"application/octet-stream"}, decode_binary);
}

body_decoder_registry& body_decoder_registry::add(const std::string& name, std::vector<std::string> media_types,
                                                  body_decoder decoder, std::string suffix) {
    body_format format{lowercase(name), {}, lowercase(suffix), std::move(decoder)};
    for (auto& type : media_types) {
        auto lower = lowercase(type);
        if (std::find(format.media_types.begin(), format.media_types.end(), lower) == format.media_types.end()) {
            format.media_types.push_back(std::move(lower));
        }
    }

    // A media type (or suffix) is decoded by one decoder only: the last one added
    for (auto& other : formats_) {
        if (other.name == format.name) continue;
        std::erase_if(other.media_types, [&](const std::string& type) {
            return std::find(format.media_types.begin(), format.media_types.end(), type) != format.media_types.end();
        });
        if (!format.suffix.empty() && other.suffix == format.suffix) other.suffix.clear();
    }

    auto existing = std::find_if(formats_.begin(), formats_.end(),
                                 [&](const body_format& f) { return f.name == format.name; });
    if (existing != formats_.end()) {
        *existing = std::move(format);
    } else {
        formats_.push_back(std::move(format));
    }
    index_plain_media_types();
    return *this;
}

bool body_decoder_registry::remove(const std::string& name) {
    auto key = lowercase(name);
    if (std::erase_if(formats_, [&](const body_format& f) { return f.name == key; }) == 0) return false;
    index_plain_media_types();
    return true;
}

void body_decoder_registry::index_plain_media_types() {
    plain_media_types_.clear();
    for (size_t i = 0; i < formats_.size(); ++i) {
        for (const auto& type : formats_[i].media_types) {
            if (is_plain_media_type(type)) plain_media_types_.emplace_back(media_type{type, {}}, i);
        }
    }
}

std::pair<const media_type*, const body_format*> body_decoder_registry::find_plain_media_type(std::string_view type) const {
    for (const auto& [parsed, index] : plain_media_types_) {
        if (parsed.type == type) return {&parsed, &formats_[index]};
    }
    return {nullptr, nullptr};
}

const body_format* body_decoder_registry::find(std::string_view name) const {
    for (const auto& format : formats_) {
        if (::thinger::util::ascii::iequals(format.name, name)) return &format;
    }
    return nullptr;
}

const body_format* body_decoder_registry::find_media_type(std::string_view type) const {
    for (const auto& format : formats_) {
        for (const auto& media_type : format.media_types) {
            if (media_type == type) return &format;
        }
    }
    // Structured syntax suffix (RFC 6838, 4.2.8): "application/problem+json" is JSON
    auto plus = type.rfind('+');
    if (plus == std::string_view::npos || type.find('/') > plus) return nullptr;
    auto suffix = type.substr(plus);
    for (const auto& format : formats_) {
        if (!format.suffix.empty() && format.suffix == suffix) return &format;
    }
    return nullptr;
}

// --- built-in decoders ---

json body_decoder_registry::decode_json(std::string_view body, const media_type& type, const body_limits& limits) {
    require_utf8(type, "JSON");
    // The message of invalid JSON is kept as it was before the other formats
    return decode_with_limits(body, json::input_format_t::json, limits, "JSON");
}

json body_decoder_registry::decode_msgpack(std::string_view body, const media_type&, const body_limits& limits) {
    return decode_with_limits(body, json::input_format_t::msgpack, limits, "MessagePack");
}

json body_decoder_registry::decode_cbor(std::string_view body, const media_type&, const body_limits& limits) {
    return decode_with_limits(body, json::input_format_t::cbor, limits, "CBOR");
}

json body_decoder_registry::decode_ubjson(std::string_view body, const media_type&, const body_limits& limits) {
    return decode_with_limits(body, json::input_format_t::ubjson, limits, "UBJSON");
}

json body_decoder_registry::decode_form(std::string_view body, const media_type& type, const body_limits&) {
    require_utf8(type, "form data");

    // name=value pairs separated by '&': percent-decoded, '+' as a space. Each name is a
    // member of the object, with its value as a string; a repeated name, an array of its
    // values in order. Empty pairs and pairs without name are skipped; a pair without '='
    // has an empty value.
    json result = json::object();
    std::string name;
    std::string value;
    size_t position = 0;
    while (position <= body.size()) {
        size_t end = body.find('&', position);
        if (end == std::string_view::npos) end = body.size();
        auto pair = body.substr(position, end - position);
        position = end + 1;
        if (pair.empty()) continue;

        auto equals = pair.find('=');
        auto raw_name = pair.substr(0, equals);
        auto raw_value = equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);
        if (raw_name.empty()) continue;

        if (!util::url::url_decode(std::string(raw_name), name) ||
            !util::url::url_decode(std::string(raw_value), value)) {
            throw body_decode_error("Invalid form data: malformed percent-encoding");
        }
        if (!valid_utf8(name) || !valid_utf8(value)) {
            throw body_decode_error("Invalid form data: not valid UTF-8");
        }

        auto member = result.find(name);
        if (member == result.end()) {
            result.emplace(name, value);
        } else if (member->is_array()) {
            member->push_back(value);
        } else {
            *member = json::array({std::move(*member), value});
        }
    }
    return result;
}

json body_decoder_registry::decode_text(std::string_view body, const media_type& type, const body_limits&) {
    require_utf8(type, "text", true);
    if (!valid_utf8(body)) throw body_decode_error("Invalid text: not valid UTF-8");
    return json(std::string(body));
}

json body_decoder_registry::decode_binary(std::string_view body, const media_type&, const body_limits&) {
    return json::binary(std::vector<uint8_t>(body.begin(), body.end()));
}

// --- settings and matching ---

const body_settings& body_settings::defaults() {
    static const body_settings settings;
    return settings;
}

body_type_match match_body_type(const body_decoder_registry& decoders,
                                const std::vector<std::string>& accepted,
                                std::string_view content_type) {
    body_type_match match;
    content_type = trim(content_type);
    auto type = media_type::parse(content_type.empty() ? default_body_media_type : content_type);
    if (!type) {
        match.error = "Invalid Content-Type";
        return match;
    }
    match.type = std::move(*type);
    match.format = decoders.find_media_type(match.type.type);
    for (const auto& entry : accepted) {
        if (entry == match.type.type || (match.format && entry == match.format->name)) {
            match.accepted = true;
            return match;
        }
    }
    match.error = "Unsupported Media Type: " + match.type.type;
    return match;
}

std::vector<std::string> accepted_media_types(const body_decoder_registry& decoders,
                                              const std::vector<std::string>& accepted) {
    std::vector<std::string> types;
    auto add = [&types](const std::string& type) {
        if (std::find(types.begin(), types.end(), type) == types.end()) types.push_back(type);
    };
    for (const auto& entry : accepted) {
        if (entry.find('/') != std::string::npos) {
            add(entry);
        } else if (const auto* format = decoders.find(entry)) {
            for (const auto& type : format->media_types) add(type);
        }
    }
    return types;
}

// --- response encoding ---

response_encoding negotiate_response_encoding(std::string_view accept) {
    // In order of preference when the client accepts several with the same quality
    static const std::array<std::pair<std::string_view, std::string_view>, 5> candidates{{
        {"application/json", "json"},
        {"application/msgpack", "msgpack"},
        {"application/x-msgpack", "msgpack"},
        {"application/cbor", "cbor"},
        {"application/ubjson", "ubjson"},
    }};
    response_encoding result{"application/json", "json"};

    // Media ranges with their quality (1 unless given)
    std::vector<std::pair<media_type, int>> ranges;
    size_t position = 0;
    while (position < accept.size()) {
        size_t end = accept.find(',', position);
        if (end == std::string_view::npos) end = accept.size();
        auto element = trim(accept.substr(position, end - position));
        position = end + 1;
        if (element.empty()) continue;
        auto range = media_type::parse(element);
        if (!range) continue;
        int quality = 1000;
        for (const auto& [name, value] : range->parameters) {
            if (name == "q") {
                quality = parse_quality(value);
                break;
            }
        }
        if (quality >= 0) ranges.emplace_back(std::move(*range), quality);
    }
    if (ranges.empty()) return result;

    int best_quality = 0;
    for (const auto& [type, format] : candidates) {
        // The quality of the most specific range matching the type
        int specificity = -1;
        int quality = 0;
        for (const auto& [range, range_quality] : ranges) {
            int match = -1;
            if (range.type == type) {
                match = 2;
            } else if (range.type == "*/*") {
                match = 0;
            } else if (range.type.ends_with("/*") &&
                       type.starts_with(std::string_view(range.type).substr(0, range.type.size() - 1))) {
                match = 1;
            }
            if (match > specificity) {
                specificity = match;
                quality = range_quality;
            }
        }
        if (quality > best_quality) {
            best_quality = quality;
            result = {std::string(type), std::string(format)};
        }
    }
    return result;
}

std::string encode_body(const nlohmann::json& value, const std::string& format) {
    std::vector<uint8_t> encoded;
    if (format == "msgpack") {
        encoded = json::to_msgpack(value);
    } else if (format == "cbor") {
        encoded = json::to_cbor(value);
    } else if (format == "ubjson") {
        encoded = json::to_ubjson(value);
    } else {
        return value.dump();
    }
    return {encoded.begin(), encoded.end()};
}

} // namespace thinger::http

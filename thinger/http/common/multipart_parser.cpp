#include "multipart_parser.hpp"
#include "../util/utf8.hpp"
#include "../../util/ascii.hpp"
#include <optional>

namespace thinger::http {

namespace {

    bool is_tchar(char c) {
        auto u = static_cast<unsigned char>(c);
        if ((u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')) return true;
        return std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
    }

    bool is_ows(char c) {
        return c == ' ' || c == '\t';
    }

    // RFC 9110, 5.5: a field value has no control characters but HTAB (obs-text is allowed:
    // browsers send UTF-8 filenames as they are)
    bool is_field_char(char c) {
        auto u = static_cast<unsigned char>(c);
        return u == '\t' || (u >= 0x20 && u != 0x7F);
    }

    std::string_view trim(std::string_view value) {
        while (!value.empty() && is_ows(value.front())) value.remove_prefix(1);
        while (!value.empty() && is_ows(value.back())) value.remove_suffix(1);
        return value;
    }

    std::string_view take_token(std::string_view& input) {
        size_t size = 0;
        while (size < input.size() && is_tchar(input[size])) ++size;
        auto token = input.substr(0, size);
        input.remove_prefix(size);
        return token;
    }

    // Characters a backslash escapes in a quoted string (RFC 2045 tspecials): elsewhere a
    // backslash is kept, as old browsers send Windows paths unescaped ("C:\dev\a.txt"), the
    // way Go's mime.ParseMediaType reads them
    bool is_tspecial(char c) {
        return std::string_view("()<>@,;:\\\"/[]?=").find(c) != std::string_view::npos;
    }

    // A token or a quoted string; nullopt if there is neither
    std::optional<std::string> take_value(std::string_view& input) {
        if (input.empty() || input.front() != '"') {
            auto token = take_token(input);
            if (token.empty()) return std::nullopt;
            return std::string(token);
        }
        std::string value;
        for (size_t i = 1; i < input.size(); ++i) {
            char c = input[i];
            if (c == '"') {
                input.remove_prefix(i + 1);
                return value;
            }
            if (c == '\\' && i + 1 < input.size() && is_tspecial(input[i + 1])) c = input[++i];
            value += c;
        }
        return std::nullopt;    // unterminated
    }

    int hex_value(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    // RFC 5987 ext-value (charset'language'percent-encoded) in UTF-8; nullopt if it is
    // malformed or its charset is not UTF-8, ISO-8859-1 or US-ASCII
    std::optional<std::string> decode_ext_value(std::string_view value) {
        auto first = value.find('\'');
        if (first == std::string_view::npos) return std::nullopt;
        auto second = value.find('\'', first + 1);
        if (second == std::string_view::npos) return std::nullopt;
        auto charset = value.substr(0, first);
        auto encoded = value.substr(second + 1);

        std::string bytes;
        for (size_t i = 0; i < encoded.size(); ++i) {
            if (encoded[i] != '%') {
                bytes += encoded[i];
                continue;
            }
            if (i + 2 >= encoded.size()) return std::nullopt;
            int high = hex_value(encoded[i + 1]);
            int low = hex_value(encoded[i + 2]);
            if (high < 0 || low < 0) return std::nullopt;
            bytes += static_cast<char>(high * 16 + low);
            i += 2;
        }

        using ::thinger::util::ascii::iequals;
        if (iequals(charset, "utf-8")) {
            if (!utf8::is_valid(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())) return std::nullopt;
            return bytes;
        }
        if (iequals(charset, "iso-8859-1")) {
            std::string result;
            for (char c : bytes) {
                auto u = static_cast<unsigned char>(c);
                if (u < 0x80) {
                    result += c;
                } else {
                    result += static_cast<char>(0xC0 | (u >> 6));
                    result += static_cast<char>(0x80 | (u & 0x3F));
                }
            }
            return result;
        }
        if (iequals(charset, "us-ascii")) {
            for (char c : bytes) {
                if (static_cast<unsigned char>(c) >= 0x80) return std::nullopt;
            }
            return bytes;
        }
        return std::nullopt;
    }

    // Content-Disposition (RFC 6266, RFC 7578 4.2): a disposition type and parameters, with
    // whitespace around ';' and '=' and a trailing ';', as Go reads it. A repeated parameter
    // is ambiguous: malformed.
    bool parse_content_disposition(std::string_view value, multipart_part& part) {
        auto type = take_token(value);
        if (type.empty()) return false;
        part.disposition = std::string(type);
        ::thinger::util::ascii::to_lower(part.disposition);

        std::vector<std::string> seen;
        std::optional<std::string> filename;
        std::optional<std::string> extended_filename;
        for (;;) {
            value = trim(value);
            if (value.empty()) break;
            if (value.front() != ';') return false;
            value.remove_prefix(1);
            value = trim(value);
            if (value.empty()) break;   // trailing ';'

            std::string name(take_token(value));
            if (name.empty()) return false;
            ::thinger::util::ascii::to_lower(name);
            value = trim(value);
            if (value.empty() || value.front() != '=') return false;
            value.remove_prefix(1);
            value = trim(value);
            auto parameter = take_value(value);
            if (!parameter) return false;

            for (const auto& other : seen) {
                if (other == name) return false;
            }
            seen.push_back(name);

            if (name == "name") {
                part.name = std::move(*parameter);
            } else if (name == "filename") {
                filename = std::move(*parameter);
            } else if (name == "filename*") {
                // An unsupported or malformed one is ignored, falling back to filename
                extended_filename = decode_ext_value(*parameter);
            }
        }

        // filename* first (RFC 6266, 4.3), as busboy does
        if (extended_filename) {
            filename = std::move(extended_filename);
        }
        if (filename) {
            part.filename = std::move(*filename);
            part.has_filename = true;
        }
        return true;
    }

}

http_response::status multipart_error_status(multipart_error error) {
    using status = http_response::status;
    switch (error) {
        case multipart_error::none: return status::ok;
        case multipart_error::not_multipart: return status::unsupported_media_type;
        case multipart_error::invalid_boundary:
        case multipart_error::malformed:
        case multipart_error::truncated:
        case multipart_error::invalid_body: return status::bad_request;
        case multipart_error::too_many_parts:
        case multipart_error::too_many_files:
        case multipart_error::header_too_large:
        case multipart_error::filename_too_large:
        case multipart_error::field_too_large:
        case multipart_error::fields_too_large:
        case multipart_error::file_too_large:
        case multipart_error::body_too_large: return status::payload_too_large;
        case multipart_error::file_failed: return status::internal_server_error;
    }
    return status::bad_request;
}

const char* multipart_error_message(multipart_error error) {
    switch (error) {
        case multipart_error::none: return "";
        case multipart_error::not_multipart: return "Unsupported Media Type: expected multipart/form-data";
        case multipart_error::invalid_boundary: return "Invalid multipart boundary";
        case multipart_error::malformed: return "Malformed multipart body";
        case multipart_error::truncated: return "Incomplete multipart body";
        case multipart_error::invalid_body: return "Invalid request body";
        case multipart_error::too_many_parts: return "Too many parts";
        case multipart_error::too_many_files: return "Too many files";
        case multipart_error::header_too_large: return "Part header too large";
        case multipart_error::filename_too_large: return "Filename too long";
        case multipart_error::field_too_large: return "Field too large";
        case multipart_error::fields_too_large: return "Fields too large";
        case multipart_error::file_too_large: return "File too large";
        case multipart_error::body_too_large: return "Payload Too Large";
        case multipart_error::file_failed: return "Could not store the file";
    }
    return "Invalid multipart body";
}

std::string multipart_part::header(std::string_view name) const {
    for (const auto& [key, value] : headers) {
        if (::thinger::util::ascii::iequals(key, name)) return value;
    }
    return {};
}

std::string multipart_part::sanitize_filename(std::string_view filename) {
    auto separator = filename.find_last_of("/\\");
    if (separator != std::string_view::npos) filename.remove_prefix(separator + 1);
    std::string result;
    for (char c : filename) {
        auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) continue;
        result += c;
    }
    if (result == "." || result == "..") result.clear();
    return result;
}

bool multipart_parser::valid_boundary(std::string_view boundary) {
    if (boundary.empty() || boundary.size() > 70 || boundary.back() == ' ') return false;
    for (char c : boundary) {
        auto u = static_cast<unsigned char>(c);
        bool valid = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                     std::string_view("'()+_,-./:=? ").find(c) != std::string_view::npos;
        if (!valid) return false;
    }
    return true;
}

multipart_parser::multipart_parser(std::string_view boundary, multipart_limits limits)
    : limits_(limits) {
    if (!valid_boundary(boundary)) {
        fail(multipart_error::invalid_boundary);
        return;
    }
    delimiter_ = "\r\n--";
    delimiter_ += boundary;
    boundary_end_ = delimiter_.size();
    delimiter_ += "--";
    // The body may start with its first delimiter, without the CRLF before it
    start_matching();
}

multipart_parser::event multipart_parser::fail(multipart_error error) {
    if (state_ != state::error) {
        state_ = state::error;
        error_ = error;
    }
    data_ = {};
    return event::error;
}

void multipart_parser::start_matching() {
    // As if the CRLF of a delimiter were already matched: it is not in the input
    matched_ = 2;
    held_from_ = 2;
}

multipart_parser::match multipart_parser::match_delimiter(size_t& matched, std::string_view input, size_t& used) const {
    size_t i = 0;
    while (matched < boundary_end_) {
        if (i == input.size()) {
            used = i;
            return match::partial;
        }
        if (input[i] != delimiter_[matched]) return match::mismatch;
        ++matched;
        ++i;
    }
    if (i == input.size()) {
        used = i;
        return match::partial;
    }
    if (matched == boundary_end_) {
        char c = input[i];
        // Transport padding or CRLF: a delimiter (the line is checked after it)
        if (is_ows(c) || c == '\r') {
            used = i;
            return match::delimiter;
        }
        if (c != '-') return match::mismatch;
        ++matched;
        if (++i == input.size()) {
            used = i;
            return match::partial;
        }
    }
    // After the boundary and a dash: another one closes the body
    if (input[i] != '-') return match::mismatch;
    ++matched;
    used = i + 1;
    return match::close;
}

multipart_parser::event multipart_parser::content(std::string_view data) {
    if (data.empty() || state_ != state::content) return event::need_more;    // (the preamble is dropped)

    // Over a limit: the one crossed first, as if the content came byte by byte
    size_t limit = part_.has_filename ? limits_.max_file_size : limits_.max_field_size;
    size_t allowed = limit - part_size_;
    size_t fields_allowed = part_.has_filename ? allowed : limits_.max_fields_size - fields_size_;
    if (data.size() > allowed || data.size() > fields_allowed) {
        if (allowed <= fields_allowed) {
            return fail(part_.has_filename ? multipart_error::file_too_large : multipart_error::field_too_large);
        }
        return fail(multipart_error::fields_too_large);
    }
    part_size_ += data.size();
    if (!part_.has_filename) fields_size_ += data.size();
    data_ = data;
    return event::data;
}

multipart_parser::event multipart_parser::on_delimiter(match found) {
    bool in_part = state_ == state::content;
    state_ = found == match::close ? state::close_line : state::delimiter_line;
    return in_part ? event::part_end : event::need_more;
}

multipart_parser::event multipart_parser::scan(std::string_view& input) {
    for (;;) {
        // A delimiter matched in part by the input consumed before: go on with it
        if (matched_ > 0) {
            size_t matched = matched_;
            size_t used = 0;
            auto found = match_delimiter(matched, input, used);
            if (found == match::partial) {
                matched_ = matched;
                input.remove_prefix(used);
                return event::need_more;
            }
            if (found == match::mismatch) {
                // Not a delimiter: the bytes held back are content, and the input is
                // scanned from its start (it has no CR within those bytes)
                auto held = std::string_view(delimiter_).substr(held_from_, matched_ - held_from_);
                matched_ = 0;
                auto result = content(held);
                if (result != event::need_more) return result;
                continue;
            }
            matched_ = 0;
            input.remove_prefix(used);
            return on_delimiter(found);
        }

        if (input.empty()) return event::need_more;

        // Content up to the first CR that starts a delimiter, or a part of one at the end of
        // the input. A delimiter has no CR but its first byte, so a mismatch at a CR moves on
        // to the next one.
        size_t position = 0;
        for (;;) {
            auto cr = input.find('\r', position);
            if (cr == std::string_view::npos) {
                auto data = input;
                input = {};
                return content(data);
            }
            size_t matched = 0;
            size_t used = 0;
            auto found = match_delimiter(matched, input.substr(cr), used);
            if (found == match::mismatch) {
                position = cr + 1;
                continue;
            }
            if (cr > 0) {
                auto data = input.substr(0, cr);
                input.remove_prefix(cr);
                auto result = content(data);
                if (result != event::need_more) return result;
                break;
            }
            input.remove_prefix(used);
            if (found == match::partial) {
                matched_ = matched;
                held_from_ = 0;
                return event::need_more;
            }
            return on_delimiter(found);
        }
    }
}

multipart_parser::event multipart_parser::header_line(std::string_view line) {
    // A field line: "name: value" CRLF, with a token as its name and no folding
    if (line.size() < 2 || line[line.size() - 2] != '\r') return fail(multipart_error::malformed);
    line.remove_suffix(2);
    auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return fail(multipart_error::malformed);
    auto name = line.substr(0, colon);
    for (char c : name) {
        if (!is_tchar(c)) return fail(multipart_error::malformed);
    }
    auto value = trim(line.substr(colon + 1));
    for (char c : value) {
        if (!is_field_char(c)) return fail(multipart_error::malformed);
    }
    headers_.emplace_back(std::string(name), std::string(value));
    return event::need_more;
}

multipart_parser::event multipart_parser::header_section_end() {
    multipart_part part;
    part.headers = std::move(headers_);
    headers_.clear();

    bool disposition = false;
    bool content_type = false;
    for (const auto& [name, value] : part.headers) {
        if (::thinger::util::ascii::iequals(name, "content-disposition")) {
            if (disposition || !parse_content_disposition(value, part)) return fail(multipart_error::malformed);
            disposition = true;
        } else if (::thinger::util::ascii::iequals(name, "content-type")) {
            if (content_type) return fail(multipart_error::malformed);
            content_type = true;
            part.content_type = value;
        }
    }

    if (part.has_filename) {
        if (part.filename.size() > limits_.max_filename_size) return fail(multipart_error::filename_too_large);
        if (++files_ > limits_.max_files) return fail(multipart_error::too_many_files);
    }

    part_ = std::move(part);
    part_size_ = 0;
    state_ = state::content;
    // The content may start with the delimiter (an empty part): the CRLF before it ended
    // the header section
    start_matching();
    return event::part;
}

multipart_parser::event multipart_parser::parse(std::string_view& input) {
    data_ = {};
    for (;;) {
        event result = event::need_more;
        switch (state_) {
            case state::preamble:
            case state::content:
                result = scan(input);
                break;

            case state::delimiter_line:
            case state::close_line:
                // Transport padding, then CRLF
                while (!input.empty() && is_ows(input.front())) input.remove_prefix(1);
                if (input.empty()) break;
                if (input.front() != '\r') return fail(multipart_error::malformed);
                input.remove_prefix(1);
                state_ = state_ == state::delimiter_line ? state::delimiter_lf : state::close_lf;
                break;

            case state::delimiter_lf:
                if (input.empty()) break;
                if (input.front() != '\n') return fail(multipart_error::malformed);
                input.remove_prefix(1);
                if (++parts_ > limits_.max_parts) return fail(multipart_error::too_many_parts);
                state_ = state::headers;
                header_size_ = 0;
                line_.clear();
                break;

            case state::headers: {
                if (input.empty()) break;
                // Up to the end of the line, counted against the limit before it is checked,
                // so the error is the same however the section is split
                auto lf = input.find('\n');
                size_t size = lf == std::string_view::npos ? input.size() : lf + 1;
                if (size > limits_.max_header_size - header_size_) return fail(multipart_error::header_too_large);
                header_size_ += size;
                line_.append(input.data(), size);
                input.remove_prefix(size);
                if (lf == std::string_view::npos) break;
                if (line_ == "\r\n") {
                    line_.clear();
                    return header_section_end();
                }
                if (header_line(line_) == event::error) return event::error;
                line_.clear();
                break;
            }

            case state::close_lf:
                if (input.empty()) break;
                if (input.front() != '\n') return fail(multipart_error::malformed);
                input.remove_prefix(1);
                state_ = state::done;
                return event::end;

            case state::done:
                // The epilogue
                input = {};
                return event::end;

            case state::error:
                return event::error;
        }
        if (result != event::need_more) return result;
        if (input.empty() && state_ != state::done) return event::need_more;
    }
}

multipart_parser::event multipart_parser::finish() {
    data_ = {};
    switch (state_) {
        case state::done:
            return event::end;
        case state::close_line:
            // The closing delimiter may end the body, without CRLF
            state_ = state::done;
            return event::end;
        case state::error:
            return event::error;
        default:
            return fail(multipart_error::truncated);
    }
}

}

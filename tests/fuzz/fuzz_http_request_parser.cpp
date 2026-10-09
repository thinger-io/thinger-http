// Fuzz the request parser (request_factory) and the body framing rules.
//
// Invariants:
//  - no crash or undefined behavior, in the full mode (body read by the parser) and in the
//    headers-only mode the server uses
//  - headers-only parsing gives the same outcome, and stops at the same byte, however the
//    input is split across reads
//  - an accepted header section is well formed: it ends at the first empty line, every line
//    ends in CRLF (no bare CR or LF), no line is folded (starts with whitespace), and it is
//    within the header section limits
//  - framing: has_valid_framing() agrees with an independent check of the raw header lines.
//    Content-Length is accepted only as 1*DIGIT (no sign, whitespace, list or overflow),
//    repeated only with the same value; Transfer-Encoding only as a single "chunked", from
//    HTTP/1.1 on, and never together with Content-Length. The body length the server would
//    read follows from them.

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <boost/algorithm/string/predicate.hpp>
#include "fuzz_common.hpp"

using thinger::http::request_factory;

namespace {

    struct parsed_headers {
        int result = 0; // 1 accepted, 0 rejected, -1 incomplete
        size_t consumed = 0;
        bool too_large = false;
        std::shared_ptr<thinger::http::http_request> request;
    };

    int to_int(boost::tribool t) {
        return t ? 1 : (!t ? 0 : -1);
    }

    // Parse headers-only, feeding the input in pieces from `gen` (all at once if none)
    parsed_headers parse_headers(const uint8_t* data, size_t size, fuzz::split_generator* gen) {
        request_factory parser;
        parser.set_headers_only(true);
        parsed_headers r;
        r.result = -1;
        size_t offset = 0;
        while (offset < size) {
            size_t piece = gen ? gen->piece(size - offset) : size - offset;
            std::vector<uint8_t> buffer(data + offset, data + offset + piece);
            const uint8_t* begin = buffer.data();
            const uint8_t* end = begin + buffer.size();
            boost::tribool result = parser.parse(begin, end);
            offset += static_cast<size_t>(begin - buffer.data());
            if (!boost::indeterminate(result)) {
                r.result = to_int(result);
                break;
            }
            fuzz::check(begin == end, "parser needs more data with input left");
        }
        r.consumed = offset;
        r.too_large = parser.header_section_too_large();
        if (r.result == 1) r.request = parser.consume_request();
        return r;
    }

    bool is_digits(std::string_view s) {
        return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
    }

    // 1*DIGIT that fits in 64 bits
    std::optional<uint64_t> parse_length(std::string_view s) {
        if (!is_digits(s)) return std::nullopt;
        uint64_t value = 0;
        for (char c : s) {
            uint64_t digit = static_cast<uint64_t>(c - '0');
            if (value > (UINT64_MAX - digit) / 10) return std::nullopt;
            value = value * 10 + digit;
        }
        return value;
    }

    // Independent check of an accepted header section: its shape, and the framing it implies
    void check_accepted(std::string_view head, const thinger::http::http_request& request, const std::string& input) {
        // Ends at the first empty line, every line terminated by CRLF
        fuzz::check(head.size() >= 4 && head.substr(head.size() - 4) == "\r\n\r\n", "header section without a final empty line", input);
        fuzz::check(head.find("\r\n\r\n") == head.size() - 4, "header section past the first empty line", input);
        for (size_t i = 0; i < head.size(); i++) {
            if (head[i] == '\r') fuzz::check(i + 1 < head.size() && head[i + 1] == '\n', "bare CR accepted", input);
            if (head[i] == '\n') fuzz::check(i > 0 && head[i - 1] == '\r', "bare LF accepted", input);
        }
        fuzz::check(head.size() <= request_factory::max_header_section_size, "header section over the size limit", input);

        // Request line: method SP target SP HTTP/x.y
        size_t line_end = head.find("\r\n");
        std::string_view request_line = head.substr(0, line_end);
        size_t sp1 = request_line.find(' ');
        size_t sp2 = request_line.find(' ', sp1 + 1);
        fuzz::check(sp1 != std::string_view::npos && sp2 != std::string_view::npos, "request line without two spaces", input);
        std::string_view target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
        std::string_view version = request_line.substr(sp2 + 1);
        fuzz::check(!target.empty() && target[0] == '/', "target not in origin form", input);
        fuzz::check(request.get_uri() == target, "target differs from the request line", input);
        fuzz::check(version.size() == 8 && version.substr(0, 7) == "HTTP/1." && is_digits(version.substr(7, 1)),
                    "HTTP version other than HTTP/1.DIGIT accepted", input);
        int major = version[5] - '0';
        int minor = version[7] - '0';
        fuzz::check(request.get_http_version_major() == major && request.get_http_version_minor() == minor,
                    "HTTP version differs from the request line", input);

        // Header lines: name ":" SP value, none folded
        std::vector<std::string_view> content_lengths, transfer_encodings;
        size_t lines = 0;
        size_t pos = line_end + 2;
        while (pos < head.size() - 2) {
            size_t end = head.find("\r\n", pos);
            std::string_view line = head.substr(pos, end - pos);
            pos = end + 2;
            lines++;
            fuzz::check(!line.empty() && line[0] != ' ' && line[0] != '\t', "folded or empty header line accepted", input);
            size_t colon = line.find(':');
            fuzz::check(colon != std::string_view::npos && colon > 0, "header line without a name", input);
            fuzz::check(line.substr(colon + 1, 1) == " ", "header value not after \": \"", input);
            std::string_view name = line.substr(0, colon);
            std::string_view value = line.substr(colon + 2);
            if (boost::iequals(name, "Content-Length")) content_lengths.push_back(value);
            if (boost::iequals(name, "Transfer-Encoding")) transfer_encodings.push_back(value);
        }
        fuzz::check(lines <= request_factory::max_header_lines, "more header lines than the limit", input);

        // Framing (RFC 9112 section 6): the only forms accepted
        std::optional<uint64_t> length;
        bool valid_length = true;
        for (auto value : content_lengths) {
            auto parsed = parse_length(value);
            if (!parsed || (length && *length != *parsed)) valid_length = false;
            if (parsed && !length) length = parsed;
        }
        bool http_1_1 = major > 1 || (major == 1 && minor >= 1);
        bool chunked = !transfer_encodings.empty();
        bool valid_chunked = transfer_encodings.size() == 1 && boost::iequals(transfer_encodings[0], "chunked") && http_1_1;
        bool valid = valid_length && (!chunked || (valid_chunked && content_lengths.empty()));

        if (request.has_valid_framing() != valid) {
            fuzz::fail(valid ? "valid framing rejected" : "invalid framing accepted", input);
        }
        if (valid) {
            fuzz::check(request.is_chunked_transfer() == chunked, "chunked framing differs from Transfer-Encoding", input);
            uint64_t expected = chunked ? 0 : length.value_or(0);
            fuzz::check(request.pending_body_size() == expected, "body length differs from Content-Length", input);
            fuzz::check(request.has_content_length_header() == !content_lengths.empty(), "Content-Length presence differs", input);
        }
    }

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Full mode: the parser reads a Content-Length body itself
    {
        request_factory parser;
        auto begin = reinterpret_cast<const char*>(data);
        auto end = begin + size;
        parser.parse(begin, end);
    }

    // Headers-only mode, as the server uses it
    std::string input = fuzz::escape({reinterpret_cast<const char*>(data), size});
    parsed_headers whole = parse_headers(data, size, nullptr);

    for (uint64_t salt = 1; salt <= 2; salt++) {
        fuzz::split_generator gen(data, size, salt);
        parsed_headers split = parse_headers(data, size, &gen);
        fuzz::check(split.result == whole.result && split.consumed == whole.consumed && split.too_large == whole.too_large,
                    "request parsing depends on how the input is split", input);
    }

    if (whole.result == 1) {
        fuzz::check(whole.request != nullptr, "accepted request without an http_request", input);
        check_accepted({reinterpret_cast<const char*>(data), whole.consumed}, *whole.request, input);
    }

    // The whole request as the server processes it, body included (checks its own invariants)
    fuzz::process_request(data, size);
    return 0;
}

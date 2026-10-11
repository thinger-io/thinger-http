// Fuzz the response parser (response_factory) as the HTTP client and the reverse proxy use
// it, and the framing rules of responses (RFC 9112, section 6.3).
//
// The input is what a server sent before closing the connection, read as the response to a
// GET, a HEAD and a CONNECT request. Invariants:
//  - no crash or undefined behavior, and no byte read past the input
//  - the same outcome, error, end, framing and body however the input is split
//  - each head (interim responses included), parsed alone as the reverse proxy does, is
//    well formed when accepted: a status line "HTTP/1.DIGIT SP 3DIGIT [SP reason]" with a
//    status from 100 to 599, CRLF line endings, no folding, valid field names and values,
//    within the limits; and it fails as the client fails on it
//  - framing: a head is accepted only if an independent check of its raw Content-Length and
//    Transfer-Encoding lines finds them valid, and rejected as invalid framing only if it
//    does not (Content-Length as 1*DIGIT, repeated only with the same value;
//    Transfer-Encoding only as a single "chunked", from HTTP/1.1 on, never with
//    Content-Length); the body framing follows from the request method and the status
//  - the client skips interim responses and reads the body of the final one as its framing
//    says: no body, Content-Length bytes, the body chunked_decoder decodes from the same
//    bytes, or the rest of the input; a connection it keeps alive never had a body
//    delimited by its end, nor switched protocols

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <boost/algorithm/string/predicate.hpp>
#include "fuzz_response.hpp"
#include "thinger/http/common/chunked_decoder.hpp"

using thinger::http::client_error;
using thinger::http::method;
using thinger::http::response_factory;

namespace {

    using fuzz::body_framing;
    using fuzz::outcome;

    bool is_digit(char c) {
        return c >= '0' && c <= '9';
    }

    bool is_digits(std::string_view s) {
        return !s.empty() && std::all_of(s.begin(), s.end(), is_digit);
    }

    // tchar (RFC 9110, section 5.6.2), written again here to check the parser
    bool is_tchar(unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos;
    }

    // HTAB, SP, VCHAR, obs-text
    bool is_value_char(unsigned char c) {
        return c == '\t' || (c >= 0x20 && c != 0x7f);
    }

    std::string_view trim(std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
        return s;
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

    // Framing of a head from its raw header lines (a well formed head: its status line,
    // field lines, and the final empty line, all ending in CRLF)
    struct raw_framing {
        bool valid = true;
        bool chunked = false;
        std::optional<uint64_t> length;
    };

    raw_framing framing_of(std::string_view head) {
        size_t line_end = head.find("\r\n");
        bool http_1_1 = head.size() > 7 && head[7] >= '1';  // "HTTP/1.x"
        std::vector<std::string_view> content_lengths, transfer_encodings;
        size_t pos = line_end + 2;
        while (pos + 2 < head.size()) {
            size_t end = head.find("\r\n", pos);
            std::string_view line = head.substr(pos, end - pos);
            pos = end + 2;
            size_t colon = line.find(':');
            if (colon == std::string_view::npos) continue;
            std::string_view name = line.substr(0, colon);
            std::string_view value = trim(line.substr(colon + 1));
            if (boost::iequals(name, "Content-Length")) content_lengths.push_back(value);
            if (boost::iequals(name, "Transfer-Encoding")) transfer_encodings.push_back(value);
        }

        raw_framing f;
        for (auto value : content_lengths) {
            auto parsed = parse_length(value);
            if (!parsed || (f.length && *f.length != *parsed)) f.valid = false;
            if (parsed && !f.length) f.length = parsed;
        }
        f.chunked = !transfer_encodings.empty();
        if (f.chunked) {
            bool single_chunked = transfer_encodings.size() == 1 && boost::iequals(transfer_encodings[0], "chunked");
            if (!single_chunked || !http_1_1 || !content_lengths.empty()) f.valid = false;
        }
        return f;
    }

    // Independent check of the shape of an accepted head (`head`: status line, field lines
    // and the empty line) and of what was parsed from it
    void check_head(std::string_view head, const thinger::http::http_response& response, const std::string& input) {
        fuzz::check(head.size() >= 4 && head.substr(head.size() - 4) == "\r\n\r\n", "head without a final empty line", input);
        fuzz::check(head.find("\r\n\r\n") == head.size() - 4, "head past the first empty line", input);
        fuzz::check(head.size() <= response_factory::max_header_section_size, "head over the size limit", input);
        for (size_t i = 0; i < head.size(); i++) {
            if (head[i] == '\r') fuzz::check(i + 1 < head.size() && head[i + 1] == '\n', "bare CR accepted", input);
            if (head[i] == '\n') fuzz::check(i > 0 && head[i - 1] == '\r', "bare LF accepted", input);
        }

        // HTTP/1.DIGIT SP 3DIGIT [ SP reason-phrase ]
        std::string_view line = head.substr(0, head.find("\r\n"));
        fuzz::check(line.size() >= 12 && line.substr(0, 7) == "HTTP/1." && is_digit(line[7]) && line[8] == ' ' &&
                        is_digits(line.substr(9, 3)),
                    "status line other than HTTP/1.DIGIT SP 3DIGIT accepted", input);
        int status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
        fuzz::check(status >= 100 && status <= 599, "status out of 100-599 accepted", input);
        if (line.size() > 12) {
            fuzz::check(line[12] == ' ', "status code not followed by a space", input);
            for (unsigned char c : line.substr(13)) fuzz::check(is_value_char(c), "control character in the reason accepted", input);
            fuzz::check(response.get_reason() == line.substr(13), "reason differs from the status line", input);
        }
        fuzz::check(response.get_status_code() == status, "status differs from the status line", input);
        fuzz::check(response.get_http_version_major() == 1 && response.get_http_version_minor() == line[7] - '0',
                    "version differs from the status line", input);

        // name ":" OWS value OWS, none folded
        size_t lines = 0;
        size_t pos = line.size() + 2;
        while (pos + 2 < head.size()) {
            size_t end = head.find("\r\n", pos);
            std::string_view field = head.substr(pos, end - pos);
            pos = end + 2;
            lines++;
            size_t colon = field.find(':');
            fuzz::check(colon != std::string_view::npos && colon > 0, "header line without a name accepted", input);
            for (unsigned char c : field.substr(0, colon)) fuzz::check(is_tchar(c), "invalid header name accepted", input);
            std::string_view value = trim(field.substr(colon + 1));
            for (unsigned char c : value) fuzz::check(is_value_char(c), "control character in a header value accepted", input);
        }
        fuzz::check(lines <= response_factory::max_header_lines, "more header lines than the limit", input);
        fuzz::check(response.get_headers().size() == lines, "headers lost or added", input);
    }

    // Body framing of a final response, as RFC 9112 section 6.3 says
    body_framing expected_framing(int status, method request_method, const raw_framing& raw) {
        if (status < 200 || request_method == method::HEAD || status == 204 || status == 304) return body_framing::none;
        if (request_method == method::CONNECT && status < 300) return body_framing::none;
        if (raw.chunked) return body_framing::chunked;
        if (raw.length) return body_framing::content_length;
        return body_framing::until_close;
    }

    bool same(const fuzz::client_result& a, const fuzz::client_result& b) {
        if (a.result != b.result || a.error != b.error) return false;
        if (a.result != outcome::accepted) return true;
        return a.end == b.end && a.body == b.body && a.framing == b.framing && a.keep_alive == b.keep_alive &&
               a.response->get_status_code() == b.response->get_status_code() &&
               a.response->get_headers().size() == b.response->get_headers().size();
    }

    void check_response(const uint8_t* data, size_t size, method request_method, const std::string& input) {
        auto whole = fuzz::process_response(data, size, request_method);
        std::string context = std::string("as the response to ") + fuzz::method_name(request_method) + "\n" + input +
                              "\nwhole: " + fuzz::describe(whole);
        for (uint64_t salt = 1; salt <= 2; salt++) {
            fuzz::split_generator gen(data, size, salt);
            auto split = fuzz::process_response(data, size, request_method, &gen);
            if (!same(whole, split)) fuzz::fail("response parsing depends on how the input is split", context + "\nsplit: " + fuzz::describe(split));
        }

        // Each head alone: the interim ones, then the final one
        size_t offset = 0;
        while (true) {
            auto head = fuzz::parse_head(data + offset, size - offset, request_method);
            if (head.result == outcome::incomplete) {
                fuzz::check(whole.result == outcome::incomplete && whole.error == client_error::incomplete_response,
                            "incomplete head, complete response", context);
                return;
            }
            if (head.result == outcome::rejected) {
                fuzz::check(whole.result == outcome::rejected && whole.error == head.error, "head and response fail differently", context);
                if (head.error == client_error::invalid_framing) {
                    std::string_view rest(reinterpret_cast<const char*>(data + offset), size - offset);
                    auto text = rest.substr(0, rest.find("\r\n\r\n") + 4);
                    fuzz::check(!framing_of(text).valid, "valid framing rejected", context);
                }
                return;
            }

            std::string_view text(reinterpret_cast<const char*>(data + offset), head.end);
            check_head(text, *head.head, context);
            auto raw = framing_of(text);
            fuzz::check(raw.valid, "invalid framing accepted", context);
            int status = head.head->get_status_code();
            fuzz::check(head.framing == expected_framing(status, request_method, raw), "unexpected body framing", context);
            fuzz::check(head.head->is_chunked_transfer() == raw.chunked, "chunked differs from Transfer-Encoding", context);
            offset += head.end;

            // The client skips interim responses
            if (status < 200 && status != 101) continue;

            const uint8_t* body = data + offset;
            size_t available = size - offset;
            switch (head.framing) {
                case body_framing::none:
                    fuzz::check(whole.result == outcome::accepted && whole.end == offset && whole.body.empty(),
                                "response without a body read otherwise", context);
                    break;
                case body_framing::content_length:
                    if (*raw.length > available) {
                        fuzz::check(whole.result == outcome::incomplete, "Content-Length body longer than the input completed", context);
                    } else {
                        fuzz::check(whole.result == outcome::accepted && whole.end == offset + *raw.length &&
                                        whole.body == std::string(reinterpret_cast<const char*>(body), *raw.length),
                                    "Content-Length body read otherwise", context);
                    }
                    break;
                case body_framing::chunked: {
                    thinger::http::chunked_decoder decoder;
                    std::vector<uint8_t> decoded(available);
                    size_t produced = 0;
                    size_t consumed = decoder.decode(body, available, decoded.data(), decoded.size(), produced);
                    if (decoder.done()) {
                        fuzz::check(whole.result == outcome::accepted && whole.end == offset + consumed &&
                                        whole.body == std::string(decoded.begin(), decoded.begin() + static_cast<std::ptrdiff_t>(produced)),
                                    "chunked body read otherwise", context);
                    } else if (decoder.failed()) {
                        fuzz::check(whole.result == outcome::rejected && whole.error == client_error::malformed_body,
                                    "malformed chunked body not rejected as such", context);
                    } else {
                        fuzz::check(whole.result == outcome::incomplete, "unfinished chunked body completed", context);
                    }
                    break;
                }
                case body_framing::until_close:
                    fuzz::check(whole.result == outcome::accepted && whole.end == size &&
                                    whole.body == std::string(reinterpret_cast<const char*>(body), available),
                                "body delimited by the connection read otherwise", context);
                    break;
            }

            if (whole.result == outcome::accepted) {
                fuzz::check(whole.response->get_status_code() == status, "final status differs", context);
                fuzz::check(whole.framing == head.framing, "framing differs from that of the head", context);
                bool switching = status == 101 || (request_method == method::CONNECT && status < 300);
                bool keep_alive = !switching && head.framing != body_framing::until_close && head.head->keep_alive();
                fuzz::check(whole.keep_alive == keep_alive, "keep-alive differs from the framing and headers", context);
            }
            return;
        }
    }

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string input = fuzz::escape({reinterpret_cast<const char*>(data), size});
    for (auto request_method : {method::GET, method::HEAD, method::CONNECT}) {
        check_response(data, size, request_method, input);
    }
    return 0;
}

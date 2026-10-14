// Multipart parser (multipart_parser): the result (parts, their headers and content, the end
// or the error) is the same however the body is split, and byte by byte; errors and the end
// are final; content never holds a delimiter and stays within the limits; header names are
// tokens and values have no CR, LF or NUL; safe filenames are never paths. A body parsed
// whole is written again from its parts, which parses to the same parts. The multipart body
// decoder fails only with body_decode_error (400 or 413), and decodes exactly the bodies the
// parser reads whole. The first byte of the input picks the boundary and the limits.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "thinger/http/common/multipart_parser.hpp"
#include "thinger/http/server/body_decoders.hpp"
#include "thinger/http/util/utf8.hpp"
#include "fuzz_common.hpp"

namespace {

    using namespace thinger::http;

    struct parsed_part {
        multipart_part part;
        std::string content;
        bool ended = false;
    };

    struct parse_result {
        std::vector<parsed_part> parts;
        bool done = false;
        multipart_error error = multipart_error::none;
    };

    bool same_part(const multipart_part& a, const multipart_part& b) {
        return a.headers == b.headers && a.disposition == b.disposition && a.name == b.name &&
               a.filename == b.filename && a.has_filename == b.has_filename && a.content_type == b.content_type;
    }

    bool same(const parse_result& a, const parse_result& b) {
        if (a.done != b.done || a.error != b.error || a.parts.size() != b.parts.size()) return false;
        for (size_t i = 0; i < a.parts.size(); ++i) {
            // On error, the content of the part being read depends on the pieces before the failing one
            bool partial = a.error != multipart_error::none && i + 1 == a.parts.size() && !a.parts[i].ended;
            if (!same_part(a.parts[i].part, b.parts[i].part) || (!partial && a.parts[i].content != b.parts[i].content) ||
                a.parts[i].ended != b.parts[i].ended) {
                return false;
            }
        }
        return true;
    }

    std::string describe(const parse_result& r) {
        std::string text = "done=" + std::to_string(r.done) + " error=" + std::to_string(static_cast<int>(r.error)) +
                           " parts=" + std::to_string(r.parts.size());
        for (const auto& p : r.parts) {
            text += "\n  [" + fuzz::escape(p.part.name, 64) + "] " + std::to_string(p.content.size()) + " bytes: " +
                    fuzz::escape(p.content, 64);
        }
        return text;
    }

    // Parse `body` in pieces of the sizes `next_piece` gives (0: the rest)
    template<typename NextPiece>
    parse_result parse(std::string_view body, const std::string& boundary, const multipart_limits& limits,
                       NextPiece next_piece) {
        multipart_parser parser(boundary, limits);
        parse_result result;
        auto handle = [&](multipart_parser::event event) {
            switch (event) {
                case multipart_parser::event::part:
                    fuzz::check(result.parts.empty() || result.parts.back().ended, "part before the end of the previous one");
                    result.parts.push_back({parser.part(), {}, false});
                    break;
                case multipart_parser::event::data:
                    fuzz::check(!result.parts.empty() && !result.parts.back().ended, "data outside a part");
                    fuzz::check(!parser.data().empty(), "empty data event");
                    result.parts.back().content.append(parser.data());
                    fuzz::check(parser.part_size() == result.parts.back().content.size(), "part size miscounted");
                    break;
                case multipart_parser::event::part_end:
                    fuzz::check(!result.parts.empty() && !result.parts.back().ended, "part end outside a part");
                    result.parts.back().ended = true;
                    break;
                case multipart_parser::event::end:
                    fuzz::check(result.parts.empty() || result.parts.back().ended, "end within a part");
                    result.done = true;
                    break;
                case multipart_parser::event::error:
                    fuzz::check(parser.error() != multipart_error::none, "error event without an error");
                    result.error = parser.error();
                    break;
                case multipart_parser::event::need_more:
                    break;
            }
        };

        size_t offset = 0;
        while (offset < body.size() && !result.done && result.error == multipart_error::none) {
            size_t piece = next_piece(body.size() - offset);
            if (piece == 0) piece = body.size() - offset;
            // An exact-size copy, so reading past the piece is caught by ASan
            std::vector<char> copy(body.begin() + static_cast<std::ptrdiff_t>(offset),
                                   body.begin() + static_cast<std::ptrdiff_t>(offset + piece));
            std::string_view input(copy.data(), copy.size());
            offset += piece;
            for (;;) {
                size_t before = input.size();
                auto event = parser.parse(input);
                fuzz::check(input.size() <= before, "input grew");
                handle(event);
                if (event == multipart_parser::event::need_more) {
                    fuzz::check(input.empty(), "need_more with input left");
                    break;
                }
                if (event == multipart_parser::event::end || event == multipart_parser::event::error) break;
            }
        }
        if (!result.done && result.error == multipart_error::none) handle(parser.finish());

        // Final
        std::string_view more = "\r\n--x\r\n";
        if (result.error != multipart_error::none) {
            fuzz::check(parser.parse(more) == multipart_parser::event::error && parser.finish() == multipart_parser::event::error,
                        "error not final");
        } else {
            fuzz::check(result.done, "neither done nor failed after finish()");
            fuzz::check(parser.parse(more) == multipart_parser::event::end && more.empty(), "end not final");
            fuzz::check(parser.finish() == multipart_parser::event::end, "end not final after finish()");
        }
        return result;
    }

    bool is_tchar(char c) {
        auto u = static_cast<unsigned char>(c);
        return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
               std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
    }

    // A delimiter within content: CRLF "--" boundary, then "--", whitespace or CR
    bool has_delimiter(const std::string& content, const std::string& boundary) {
        std::string delimiter = "\r\n--" + boundary;
        for (auto position = content.find(delimiter); position != std::string::npos;
             position = content.find(delimiter, position + 1)) {
            size_t after = position + delimiter.size();
            if (after == content.size()) continue;      // followed by what is after the content
            char c = content[after];
            if (c == ' ' || c == '\t' || c == '\r') return true;
            if (c == '-' && after + 1 < content.size() && content[after + 1] == '-') return true;
        }
        // At the start, the delimiter of an empty part
        std::string start = "--" + boundary;
        if (content.starts_with(start) && content.size() > start.size()) {
            char c = content[start.size()];
            if (c == ' ' || c == '\t' || c == '\r' || content.substr(start.size()).starts_with("--")) return true;
        }
        return false;
    }

    void check_parts(const parse_result& result, const std::string& boundary, const multipart_limits& limits) {
        size_t files = 0;
        size_t fields_size = 0;
        for (const auto& p : result.parts) {
            size_t header_size = 2;
            for (const auto& [name, value] : p.part.headers) {
                fuzz::check(!name.empty() && std::all_of(name.begin(), name.end(), is_tchar), "header name not a token", name);
                for (char c : value) {
                    fuzz::check(c != '\r' && c != '\n' && c != '\0', "CR, LF or NUL in a header value", value);
                }
                header_size += name.size() + 1 + value.size() + 2;
            }
            fuzz::check(header_size <= limits.max_header_size, "header section over the limit");
            fuzz::check(!has_delimiter(p.content, boundary), "delimiter within content", fuzz::escape(p.content));
            if (p.part.is_file()) {
                ++files;
                fuzz::check(p.content.size() <= limits.max_file_size, "file over the limit");
                fuzz::check(p.part.filename.size() <= limits.max_filename_size, "filename over the limit");
            } else {
                fields_size += p.content.size();
                fuzz::check(p.content.size() <= limits.max_field_size, "field over the limit");
            }
            auto safe = p.part.safe_filename();
            fuzz::check(safe != "." && safe != ".." && safe.find_first_of("/\\") == std::string::npos, "safe filename is a path", safe);
            for (char c : safe) {
                auto u = static_cast<unsigned char>(c);
                fuzz::check(u >= 0x20 && u != 0x7F, "control character in a safe filename");
            }
        }
        fuzz::check(result.parts.size() <= limits.max_parts, "parts over the limit");
        fuzz::check(files <= limits.max_files, "files over the limit");
        fuzz::check(fields_size <= limits.max_fields_size, "fields over the limit");
    }

    // The parts written again as a body
    std::string write(const parse_result& result, const std::string& boundary) {
        std::string body;
        for (const auto& p : result.parts) {
            body += "--" + boundary + "\r\n";
            for (const auto& [name, value] : p.part.headers) body += name + ":" + value + "\r\n";
            body += "\r\n" + p.content + "\r\n";
        }
        return body + "--" + boundary + "--";
    }

    const char* const boundaries[] = {"boundary", "b", "----WebKitFormBoundary7MA4YWxkTrZu0gW", "a b", "b-", "--"};

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    const std::string boundary = boundaries[data[0] % std::size(boundaries)];
    multipart_limits limits;
    if (data[0] & 0x80) {
        // Small limits, to reach them
        limits.max_parts = 4;
        limits.max_files = 2;
        limits.max_header_size = 96;
        limits.max_filename_size = 8;
        limits.max_field_size = 16;
        limits.max_fields_size = 24;
        limits.max_file_size = 48;
    }
    std::string_view body(reinterpret_cast<const char*>(data) + 1, size - 1);

    auto whole = parse(body, boundary, limits, [](size_t) { return size_t{0}; });
    fuzz::split_generator splits(data, size);
    auto split = parse(body, boundary, limits, [&](size_t left) { return splits.piece(left); });
    fuzz::check(same(whole, split), "result depends on the split", describe(whole) + "\n---\n" + describe(split));
    if (body.size() <= 1024) {
        auto bytes = parse(body, boundary, limits, [](size_t) { return size_t{1}; });
        fuzz::check(same(whole, bytes), "result differs byte by byte", describe(whole) + "\n---\n" + describe(bytes));
    }
    if (whole.error == multipart_error::none) {
        check_parts(whole, boundary, limits);
        auto again = parse(write(whole, boundary), boundary, limits, [](size_t) { return size_t{0}; });
        fuzz::check(same(whole, again), "parts written again parse differently", describe(whole) + "\n---\n" + describe(again));
    } else {
        // Every part before the error is within the limits too
        check_parts(whole, boundary, limits);
    }

    // The body decoder, with the default limits
    auto type = media_type::parse("multipart/form-data; boundary=\"" + boundary + "\"");
    fuzz::check(type.has_value(), "boundary not parsed", boundary);
    bool decoded = false;
    try {
        auto value = body_decoder_registry::decode_multipart(body, *type, {});
        fuzz::check(value.is_object(), "multipart not decoded as an object");
        (void) value.dump();
        decoded = true;
    } catch (const body_decode_error& e) {
        fuzz::check(e.status() == http_response::status::bad_request || e.status() == http_response::status::payload_too_large,
                    "multipart decoding failed with another status", e.what());
    } catch (const std::exception& e) {
        fuzz::fail("multipart decoder threw something else than body_decode_error", e.what());
    }
    if (!(data[0] & 0x80)) {
        // Decoded exactly when parsed whole (unless its text is not UTF-8)
        if (decoded) fuzz::check(whole.done, "decoded a body the parser rejects");
        else if (whole.done) {
            bool utf8 = true;
            for (const auto& p : whole.parts) {
                if (p.part.disposition != "form-data" || p.part.name.empty()) continue;
                auto valid = [](const std::string& s) {
                    return utf8::is_valid(reinterpret_cast<const uint8_t*>(s.data()), s.size());
                };
                if (!valid(p.part.name) || (p.part.is_file() ? !valid(p.part.filename) || !valid(p.part.content_type)
                                                              : !valid(p.content))) {
                    utf8 = false;
                }
            }
            fuzz::check(!utf8, "a body the parser reads whole not decoded");
        }
    }
    return 0;
}

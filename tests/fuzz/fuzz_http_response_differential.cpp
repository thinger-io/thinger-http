// Differential fuzzing of response parsing and framing, as the HTTP client reads responses:
// ours, llhttp (in response mode) and Boost.Beast.
//
// The input is what a server sent before closing the connection, read as the response to a
// GET and to a HEAD request. llhttp, the parser of Node.js's HTTP client, is the reference
// (strict, no lenient flag set); interim responses (1xx but 101) are skipped, as the
// client does, and the end of the input ends the connection (llhttp_finish), which
// completes a body delimited by it. The target aborts when:
//  - we accept a response llhttp rejects, or complete one llhttp still needs data for (or
//    the other way around);
//  - both accept it but disagree on where it ends, the status, the version, the body, its
//    framing (chunked, or delimited by the end of the connection), or the protocol switch.
// We may reject what llhttp accepts: we are stricter on purpose. The few classes where we
// accept something llhttp rejects, or read it differently, are recognized by
// normalize_known_discrepancy() below (see README.md, next to this file).
//
// Beast's response parser reads the same input as a third opinion: its disagreements with
// llhttp are only counted and reported, never an error.
//
// Responses following a response are compared too, while the connection would stay open.
//
// FUZZ_TRACE=1 in the environment prints the three results for every response parsed.
//
// llhttp and Beast are used only by the fuzz targets: they are never linked into the library.

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <llhttp.h>
#include "fuzz_differential.hpp"
#include "fuzz_response.hpp"

using thinger::http::method;

namespace {

    using fuzz::body_framing;
    using fuzz::outcome;

    struct llhttp_result {
        outcome result = outcome::incomplete;
        llhttp_errno_t error = HPE_OK;
        std::string reason;
        int status = 0;
        unsigned major = 0;
        unsigned minor = 0;
        bool chunked = false;
        bool needs_eof = false;
        bool upgrade = false;
        std::string body;
        size_t end = 0;
        bool complete = false;
        bool head = false;      // the request was HEAD: no body
    };

    llhttp_result& result_of(llhttp_t* parser) {
        return *static_cast<llhttp_result*>(parser->data);
    }

    const llhttp_settings_t* llhttp_callbacks() {
        static const llhttp_settings_t settings = [] {
            llhttp_settings_t s;
            llhttp_settings_init(&s);
            s.on_body = [](llhttp_t* parser, const char* at, size_t length) {
                result_of(parser).body.append(at, length);
                return 0;
            };
            s.on_headers_complete = [](llhttp_t* parser) {
                auto& r = result_of(parser);
                r.status = parser->status_code;
                r.major = parser->http_major;
                r.minor = parser->http_minor;
                r.chunked = (parser->flags & F_CHUNKED) != 0;
                // from the header section (Transfer-Encoding in the trailer section changes
                // the flags it looks at, once the body is read)
                r.needs_eof = !r.head && llhttp_message_needs_eof(parser) != 0;
                // 1: no body, as Node.js answers for the response to a HEAD request
                return r.head ? 1 : 0;
            };
            s.on_message_complete = [](llhttp_t* parser) {
                auto& r = result_of(parser);
                r.complete = true;
                // Stop right after this response: the error position is where it ends
                return static_cast<int>(HPE_PAUSED);
            };
            return s;
        }();
        return &settings;
    }

    // Parse the response at the start of `data` with llhttp (strict), skipping interim
    // responses, then the end of the connection
    llhttp_result llhttp_process(const uint8_t* data, size_t size, bool head) {
        llhttp_result r;
        r.head = head;
        llhttp_t parser;
        llhttp_init(&parser, HTTP_RESPONSE, llhttp_callbacks());
        parser.data = &r;
        const char* begin = reinterpret_cast<const char*>(data);
        size_t offset = 0;
        while (true) {
            r.complete = false;
            r.body.clear();
            llhttp_errno_t error = llhttp_execute(&parser, begin + offset, size - offset);
            size_t end = r.complete ? static_cast<size_t>(llhttp_get_error_pos(&parser) - begin) : size;
            if (!r.complete && error == HPE_OK) {
                // all the input read: the server closed the connection
                error = llhttp_finish(&parser);
            }
            if (r.complete) {
                r.upgrade = parser.upgrade != 0;
                if (r.status < 200 && r.status != 101 && end < size) {
                    offset = end;
                    llhttp_resume(&parser);
                    continue;
                }
                if (r.status < 200 && r.status != 101) {
                    r.result = outcome::incomplete;  // only interim responses
                    return r;
                }
                r.result = outcome::accepted;
                r.end = end;
                return r;
            }
            if (error == HPE_OK || error == HPE_INVALID_EOF_STATE) {
                r.result = outcome::incomplete;
            } else {
                r.result = outcome::rejected;
                r.error = error;
                const char* reason = llhttp_get_error_reason(&parser);
                r.reason = reason ? reason : "";
            }
            return r;
        }
    }

    struct beast_result {
        outcome result = outcome::incomplete;
        boost::beast::error_code error;
        int status = 0;
        unsigned version = 0;
        bool chunked = false;
        std::string body;
        size_t end = 0;
    };

    // Parse the response at the start of `data` with Beast, skipping interim responses, then
    // the end of the connection
    beast_result beast_process(const uint8_t* data, size_t size, bool head) {
        namespace beast_http = boost::beast::http;
        beast_result r;
        size_t offset = 0;
        while (true) {
            beast_http::response_parser<beast_http::string_body> parser;
            parser.eager(true);
            parser.skip(head);
            parser.header_limit(UINT32_MAX);
            parser.body_limit(size);
            boost::beast::error_code ec;
            while (!parser.is_done()) {
                size_t bytes = 0;
                ec = {};
                if (offset < size) {
                    bytes = parser.put(boost::asio::buffer(data + offset, size - offset), ec);
                    offset += bytes;
                }
                if (ec && ec != beast_http::error::need_more) {
                    r.result = outcome::rejected;
                    r.error = ec;
                    return r;
                }
                if (parser.is_done()) break;
                if (bytes == 0) {
                    // it takes nothing more: the connection ends here
                    ec = {};
                    parser.put_eof(ec);
                    if (!ec && parser.is_done()) break;
                    r.result = ec && ec != beast_http::error::partial_message ? outcome::rejected : outcome::incomplete;
                    r.error = ec;
                    return r;
                }
            }
            auto& message = parser.get();
            r.status = static_cast<int>(message.result_int());
            if (r.status < 200 && r.status != 101) {
                if (offset < size) continue;
                return r;
            }
            r.result = outcome::accepted;
            r.version = message.version();
            r.chunked = parser.chunked();
            r.body = message.body();
            r.end = offset;
            return r;
        }
    }

    std::string describe(const llhttp_result& r) {
        std::string s = fuzz::to_string(r.result);
        if (r.result == outcome::rejected) s += std::string(" (") + llhttp_errno_name(r.error) + ": " + r.reason + ")";
        if (r.result == outcome::accepted) {
            s += " status=" + std::to_string(r.status) + " version=" + std::to_string(r.major) + "." + std::to_string(r.minor) +
                 " chunked=" + std::to_string(r.chunked) + " needs_eof=" + std::to_string(r.needs_eof) +
                 " upgrade=" + std::to_string(r.upgrade) + " end=" + std::to_string(r.end) + " body=[" + fuzz::escape(r.body) + "]";
        }
        return s;
    }

    std::string describe(const beast_result& r) {
        std::string s = fuzz::to_string(r.result);
        if (r.result == outcome::rejected) s += " (" + r.error.message() + ")";
        if (r.result == outcome::accepted) {
            s += " status=" + std::to_string(r.status) + " version=" + std::to_string(r.version / 10) + "." +
                 std::to_string(r.version % 10) + " chunked=" + std::to_string(r.chunked) + " end=" + std::to_string(r.end) +
                 " body=[" + fuzz::escape(r.body) + "]";
        }
        return s;
    }

    std::string describe_all(std::string_view input, method request_method, const fuzz::client_result& ours,
                             const llhttp_result& llhttp, const beast_result& beast) {
        return std::string("response to ") + fuzz::method_name(request_method) + "\ninput:  " + fuzz::escape(input, 2048) +
               "\nours:   " + fuzz::describe(ours) + "\nllhttp: " + describe(llhttp) + "\nbeast:  " + describe(beast);
    }

    fuzz::class_counter beast_disagreements("Beast vs llhttp disagreements");
    fuzz::class_counter known_discrepancies("Known discrepancies with llhttp (normalized)");

    // Compare Beast with llhttp, only to count their disagreements
    void compare_beast(std::string_view input, method request_method, const fuzz::client_result& ours,
                       const llhttp_result& llhttp, const beast_result& beast) {
        std::string what;
        if (beast.result != llhttp.result) {
            what = std::string("beast ") + fuzz::to_string(beast.result);
            if (beast.result == outcome::rejected) what += " (" + beast.error.message() + ")";
            what += std::string(", llhttp ") + fuzz::to_string(llhttp.result);
            if (llhttp.result == outcome::rejected) what += std::string(" (") + llhttp_errno_name(llhttp.error) + ": " + llhttp.reason + ")";
        } else if (beast.result == outcome::accepted) {
            if (beast.end != llhttp.end) what = "both accept, different end";
            else if (beast.status != llhttp.status) what = "both accept, different status";
            else if (beast.body != llhttp.body) what = "both accept, different body";
            else if (beast.version != llhttp.major * 10 + llhttp.minor) what = "both accept, different version";
        }
        if (!what.empty()) beast_disagreements.add(what, describe_all(input, request_method, ours, llhttp, beast));
    }

    // Start and end of each head (interim ones, then the final one) of a response we accepted
    std::vector<std::pair<size_t, size_t>> heads_of(const std::string& input, method request_method) {
        std::vector<std::pair<size_t, size_t>> heads;
        size_t offset = 0;
        while (true) {
            auto head = fuzz::parse_head(reinterpret_cast<const uint8_t*>(input.data()) + offset, input.size() - offset, request_method);
            if (head.result != outcome::accepted) break;
            heads.emplace_back(offset, offset + head.end);
            offset += head.end;
            int status = head.head->get_status_code();
            if (status >= 200 || status == 101) break;
        }
        return heads;
    }

    // Replace `from` with `to` in the values of the `name` header lines of a head
    bool replace_in_header(std::string& input, size_t begin, size_t end, std::string_view name, char from, char to) {
        bool found = false;
        size_t pos = input.find("\r\n", begin) + 2;
        while (pos + 2 < end) {
            size_t line_end = input.find("\r\n", pos);
            size_t colon = input.find(':', pos);
            if (colon < line_end && boost::iequals(std::string_view(input).substr(pos, colon - pos), name)) {
                for (size_t i = colon + 1; i < line_end; i++) {
                    if (input[i] == from) {
                        input[i] = to;
                        found = true;
                    }
                }
            }
            pos = line_end + 2;
        }
        return found;
    }

    bool has_header(const std::string& input, size_t begin, size_t end, std::string_view name) {
        std::string copy = input.substr(begin, end - begin);
        return fuzz::neutralize_header(copy, copy.size(), name);
    }

    /**
     * Discrepancies known to be acceptable, where we accept a response llhttp rejects or reads
     * otherwise (see README.md). If it is one of them, `input` is rewritten so that llhttp no
     * longer does it for that reason, and the reason returned; comparing again then checks the
     * rest of the response. The rewrite keeps our framing; when it changes the size, it is only
     * within the response. nullptr if it is not one.
     */
    const char* normalize_known_discrepancy(const fuzz::client_result& ours, const llhttp_result& theirs, std::string& input,
                                            method request_method) {
        auto heads = heads_of(input, request_method);
        if (heads.empty()) return nullptr;

        // HTTP/1.2 to HTTP/1.9: handled as HTTP/1.1 (RFC 9110, section 2.5), while llhttp only
        // accepts 0.9, 1.0, 1.1 and 2.0
        if (theirs.error == HPE_INVALID_VERSION) {
            bool found = false;
            for (auto [begin, end] : heads) {
                if (input[begin + 7] > '1') {
                    input[begin + 7] = '1';
                    found = true;
                }
            }
            if (found) return "higher HTTP/1 minor version";
        }

        // Interim responses never have a body (RFC 9112, section 6.3), while llhttp reads one
        // after a 1xx other than 100 to 103 with Content-Length or Transfer-Encoding. Made 100
        bool body_after_interim = false;
        for (size_t i = 0; i + 1 < heads.size(); i++) {
            auto [begin, end] = heads[i];
            int status = std::stoi(input.substr(begin + 9, 3));
            if (status > 103 && (has_header(input, begin, end, "Content-Length") || has_header(input, begin, end, "Transfer-Encoding"))) {
                input.replace(begin + 9, 3, "100");
                body_after_interim = true;
            }
        }
        if (body_after_interim) return "interim response with framing headers";

        // Whether the connection stays open is up to the final response: llhttp takes an
        // interim one from HTTP/1.0, or with Connection: close, as the last one, and fails
        // on what follows. Made HTTP/1.1, the Connection values overwritten
        if (theirs.error == HPE_CLOSED_CONNECTION && heads.size() > 1) {
            for (size_t i = 0; i + 1 < heads.size(); i++) {
                auto [begin, end] = heads[i];
                std::string head = input.substr(begin, end - begin);
                head[7] = '1';
                fuzz::neutralize_header(head, head.size(), "Connection");
                fuzz::neutralize_header(head, head.size(), "Proxy-Connection");
                input.replace(begin, end - begin, head);
            }
            return "interim response closing the connection";
        }

        // Repeated Content-Length header with the same value (ours rejects different ones):
        // RFC 9110 section 8.6 lets a recipient handle it as a single one
        if (theirs.error == HPE_UNEXPECTED_CONTENT_LENGTH) {
            bool found = false;
            for (auto [begin, end] : heads) {
                std::string head = input.substr(begin, end - begin);
                if (fuzz::rename_repeated_header(head, head.size(), "Content-Length")) {
                    input.replace(begin, end - begin, head);
                    found = true;
                }
            }
            if (found) return "repeated identical Content-Length";
        }

        // HTAB around "chunked" (OWS, not part of the value: RFC 9110 section 5.5): llhttp only
        // takes SP after it, and reads "chunked\t" as another coding, so the body as delimited
        // by the end of the connection. Made SP
        auto [final_begin, final_end] = heads.back();
        if (ours.framing == body_framing::chunked && !theirs.chunked &&
            replace_in_header(input, final_begin, final_end, "Transfer-Encoding", '\t', ' ')) {
            return "HTAB around chunked";
        }

        if (ours.framing == body_framing::chunked) {
            // Whitespace in chunk extensions: the grammar allows BWS before and after ";" and
            // "=" (RFC 9112 section 7.1.1), llhttp does not. They are skipped, after the size
            if (theirs.reason.find("chunk extension") != std::string::npos &&
                fuzz::remove_chunk_extensions_with_bws(input, final_end)) {
                return "whitespace in a chunk extension";
            }

            // llhttp processes trailer fields like header fields, rejecting Content-Length and
            // Transfer-Encoding there. We drop the trailer section: it is never merged into the
            // headers, so they cannot change the framing
            if ((theirs.error == HPE_INVALID_CONTENT_LENGTH || theirs.error == HPE_UNEXPECTED_CONTENT_LENGTH ||
                 theirs.error == HPE_INVALID_TRANSFER_ENCODING) &&
                fuzz::rename_trailers(input, final_end, ours.end)) {
                return "framing field in the trailer section";
            }
        }

        return nullptr;
    }

    bool trace = std::getenv("FUZZ_TRACE") != nullptr;

    // Compare the parsers on the response at the start of `data`. Returns the bytes of the
    // response when we and llhttp accept it and the connection would carry another one, 0
    // otherwise.
    size_t compare(const uint8_t* data, size_t size, method request_method, int normalizations = 0) {
        bool head = request_method == method::HEAD;
        auto ours = fuzz::process_response(data, size, request_method);
        auto theirs = llhttp_process(data, size, head);
        auto beast = beast_process(data, size, head);
        std::string_view input(reinterpret_cast<const char*>(data), size);

        if (trace) std::fprintf(stderr, "\n%s\n", describe_all(input, request_method, ours, theirs, beast).c_str());
        compare_beast(input, request_method, ours, theirs, beast);

        auto disagree = [&](const char* what) -> size_t {
            std::string normalized(input);
            // (each rewrite removes the cause of one class of discrepancy, of the 7 known)
            const char* known = normalizations < 7 && ours.result != outcome::rejected
                                    ? normalize_known_discrepancy(ours, theirs, normalized, request_method)
                                    : nullptr;
            if (!known) fuzz::fail(what, describe_all(input, request_method, ours, theirs, beast));
            known_discrepancies.add(known, describe_all(input, request_method, ours, theirs, beast));
            if (trace) std::fprintf(stderr, "normalized (%s): %s\n", known, fuzz::escape(normalized, 2048).c_str());
            size_t bytes = compare(reinterpret_cast<const uint8_t*>(normalized.data()), normalized.size(), request_method,
                                   normalizations + 1);
            // The rewrites keep the size, but for chunk extensions, within the response
            return bytes ? bytes + size - normalized.size() : 0;
        };

        if (ours.result == outcome::rejected) return 0;  // stricter than llhttp, or both reject
        if (ours.result == outcome::incomplete) {
            if (theirs.result == outcome::accepted) return disagree("we need more data for a response llhttp completed");
            return 0;
        }

        // We accept the response
        if (theirs.result != outcome::accepted) {
            return disagree(theirs.result == outcome::rejected ? "we accept a response llhttp rejects"
                                                               : "we complete a response llhttp needs more data for");
        }

        auto& res = *ours.response;
        if (ours.end != theirs.end) return disagree("the response ends at a different byte");
        if (res.get_status_code() != theirs.status) return disagree("different status");
        if (res.get_http_version_major() != static_cast<int>(theirs.major) ||
            res.get_http_version_minor() != static_cast<int>(theirs.minor)) {
            return disagree("different HTTP version");
        }
        if (ours.body != theirs.body) return disagree("different body");
        if (ours.framing != body_framing::none && (ours.framing == body_framing::chunked) != theirs.chunked) {
            return disagree("different body framing");
        }
        if ((ours.framing == body_framing::until_close) != theirs.needs_eof) {
            return disagree("different body framing (delimited by the end of the connection)");
        }

        return ours.keep_alive ? ours.end : 0;
    }

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    for (auto request_method : {method::GET, method::HEAD}) {
        size_t offset = 0;
        for (int response = 0; response < 4 && offset < size; response++) {
            size_t bytes = compare(data + offset, size - offset, request_method);
            if (bytes == 0) break;
            offset += bytes;
        }
    }
    return 0;
}

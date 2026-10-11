// Three-way differential fuzzing of request parsing and body framing: ours, llhttp and
// Boost.Beast.
//
// llhttp (Node.js's parser) is the reference: it is strict by default (no lenient flag is
// set here) and the parser most studied against request smuggling. Each input is processed
// as the server does (fuzz::process_request) and by llhttp, and the target aborts when:
//  - we accept a request llhttp rejects, or complete one llhttp still needs more data for
//    (or the other way around);
//  - both accept it but disagree on where it ends, the framing (chunked or Content-Length),
//    the decoded body, the method, the target or the version.
// We may reject what llhttp accepts: we are stricter on purpose in several places. The few
// classes where we accept something llhttp rejects are recognized by
// normalize_known_discrepancy() below (see README.md, next to this file).
//
// Beast parses the same input as a third opinion: its disagreements with llhttp are only
// counted and reported (the first input of each class, and the totals at exit), never an
// error. The ones that matter to us are checked by fuzz_http_differential.
//
// Pipelined requests are followed: after an agreed request, the next one is parsed from
// the leftover, as long as the connection would stay open.
//
// FUZZ_TRACE=1 in the environment prints the three results for every request parsed (to
// classify a corpus, e.g. running the target on seed files).
//
// llhttp and Beast are used only by the fuzz targets: they are never linked into the library.

#include <cctype>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <llhttp.h>
#include "fuzz_differential.hpp"

namespace {

    using fuzz::outcome;

    struct llhttp_result {
        outcome result = outcome::incomplete;
        llhttp_errno_t error = HPE_OK;
        std::string reason;
        std::string method;
        std::string target;
        unsigned major = 0;
        unsigned minor = 0;
        bool chunked = false;
        bool has_content_length = false;
        uint64_t content_length = 0;
        bool keep_alive = false;
        bool upgrade = false;
        std::string body;
        size_t end = 0;
        bool complete = false;
    };

    llhttp_result& result_of(llhttp_t* parser) {
        return *static_cast<llhttp_result*>(parser->data);
    }

    const llhttp_settings_t* llhttp_callbacks() {
        static const llhttp_settings_t settings = [] {
            llhttp_settings_t s;
            llhttp_settings_init(&s);
            s.on_url = [](llhttp_t* parser, const char* at, size_t length) {
                result_of(parser).target.append(at, length);
                return 0;
            };
            s.on_body = [](llhttp_t* parser, const char* at, size_t length) {
                result_of(parser).body.append(at, length);
                return 0;
            };
            s.on_headers_complete = [](llhttp_t* parser) {
                auto& r = result_of(parser);
                r.major = parser->http_major;
                r.minor = parser->http_minor;
                r.chunked = (parser->flags & F_CHUNKED) != 0;
                r.has_content_length = (parser->flags & F_CONTENT_LENGTH) != 0;
                r.content_length = parser->content_length;  // nothing of the body read yet
                return 0;
            };
            s.on_message_complete = [](llhttp_t* parser) {
                auto& r = result_of(parser);
                r.complete = true;
                r.keep_alive = llhttp_should_keep_alive(parser) != 0;
                // Stop right after this request: the error position is where it ends
                return static_cast<int>(HPE_PAUSED);
            };
            return s;
        }();
        return &settings;
    }

    // Parse the request at the start of `data` with llhttp, in its default (strict) mode
    llhttp_result llhttp_process(const uint8_t* data, size_t size) {
        llhttp_result r;
        llhttp_t parser;
        llhttp_init(&parser, HTTP_REQUEST, llhttp_callbacks());
        parser.data = &r;
        const char* begin = reinterpret_cast<const char*>(data);
        llhttp_errno_t error = llhttp_execute(&parser, begin, size);
        if (r.complete) {
            r.result = outcome::accepted;
            r.end = static_cast<size_t>(llhttp_get_error_pos(&parser) - begin);
            r.method = llhttp_method_name(static_cast<llhttp_method_t>(parser.method));
            // CONNECT, or Upgrade with Connection: upgrade: what follows is another protocol
            r.upgrade = parser.upgrade != 0;
        } else if (error == HPE_OK) {
            r.result = outcome::incomplete;
        } else {
            r.result = outcome::rejected;
            r.error = error;
            const char* reason = llhttp_get_error_reason(&parser);
            r.reason = reason ? reason : "";
        }
        return r;
    }

    std::string describe(const llhttp_result& r) {
        std::string s = fuzz::to_string(r.result);
        if (r.result == outcome::rejected) s += std::string(" (") + llhttp_errno_name(r.error) + ": " + r.reason + ")";
        if (r.result == outcome::accepted) {
            s += " method=" + r.method + " target=" + fuzz::escape(r.target) +
                 " version=" + std::to_string(r.major) + "." + std::to_string(r.minor) +
                 " chunked=" + std::to_string(r.chunked) + " content_length=" + std::to_string(r.content_length) +
                 " keep_alive=" + std::to_string(r.keep_alive) + " upgrade=" + std::to_string(r.upgrade) +
                 " end=" + std::to_string(r.end) + " body=[" + fuzz::escape(r.body) + "]";
        }
        return s;
    }

    std::string describe_all(std::string_view input, const fuzz::server_result& ours, const llhttp_result& llhttp,
                             const fuzz::beast_result& beast) {
        return "input:  " + fuzz::escape(input, 2048) + "\nours:   " + fuzz::describe(ours) + "\nllhttp: " + describe(llhttp) +
               "\nbeast:  " + fuzz::describe(beast);
    }

    fuzz::class_counter beast_disagreements("Beast vs llhttp disagreements");
    fuzz::class_counter known_discrepancies("Known discrepancies where we accept what llhttp rejects (normalized)");

    // Compare Beast with llhttp, only to count their disagreements
    void compare_beast(std::string_view input, const fuzz::server_result& ours, const llhttp_result& llhttp,
                       const fuzz::beast_result& beast) {
        std::string what;
        if (beast.result != llhttp.result) {
            what = std::string("beast ") + fuzz::to_string(beast.result);
            if (beast.result == outcome::rejected) what += " (" + beast.error.message() + ")";
            what += std::string(", llhttp ") + fuzz::to_string(llhttp.result);
            if (llhttp.result == outcome::rejected) what += std::string(" (") + llhttp_errno_name(llhttp.error) + ": " + llhttp.reason + ")";
        } else if (beast.result == outcome::accepted) {
            if (beast.end != llhttp.end) what = "both accept, different end";
            else if (beast.chunked != llhttp.chunked) what = "both accept, different framing";
            else if (beast.body != llhttp.body) what = "both accept, different body";
            else if (beast.target != llhttp.target) what = "both accept, different target";
            else if (beast.method != llhttp.method) what = "both accept, different method";
            else if (beast.version != llhttp.major * 10 + llhttp.minor) what = "both accept, different version";
        }
        if (!what.empty()) beast_disagreements.add(what, describe_all(input, ours, llhttp, beast));
    }

    /**
     * Discrepancies known to be acceptable, where we accept a request llhttp rejects or still
     * needs data for (see README.md). If it is one of them, `input` is rewritten so that
     * llhttp no longer does it for that reason, and the reason returned; comparing again then checks the
     * rest of the request. The rewrite keeps the framing; when it changes the size, it is
     * only within the request. nullptr if it is not one.
     */
    const char* normalize_known_discrepancy(const fuzz::server_result& ours, const llhttp_result& theirs, std::string& input) {
        auto& req = *ours.request;

        // A method llhttp does not know, or not for HTTP (RTSP methods), or in lowercase: any
        // token is a method (RFC 9110 section 9.1), which we keep as UNKNOWN for the
        // application to answer; it does not frame the message. Also PRI, which llhttp always
        // reads as the start of the HTTP/2 connection preface (waiting for the rest of it, or
        // stopping there): an HTTP/1.1 server sees a request with an unknown method, as the
        // preface intends (RFC 9113 section 3.4). Replaced by GET
        bool method_error = theirs.error == HPE_INVALID_METHOD || theirs.error == HPE_PAUSED_H2_UPGRADE ||
                            theirs.reason.find("method") != std::string::npos ||
                            theirs.reason.find("HTTP/2 Connection Preface") != std::string::npos ||
                            (theirs.result == outcome::incomplete && input.starts_with("PRI "));
        if (method_error && req.get_method() == thinger::http::method::UNKNOWN) {
            input.replace(0, input.find(' '), "GET");
            return "method unknown to llhttp";
        }

        // HTTP/1.2 to HTTP/1.9: handled as HTTP/1.1 (RFC 9110, section 2.5), while llhttp only
        // accepts 0.9, 1.0, 1.1 and 2.0
        if (theirs.error == HPE_INVALID_VERSION && req.get_http_version_minor() > 1) {
            input[input.find("\r\n") - 1] = '1';
            return "higher HTTP/1 minor version";
        }

        // Repeated Content-Length header with the same value (ours rejects different ones):
        // RFC 9110 section 8.6 lets a recipient handle it as a single one
        if (theirs.error == HPE_UNEXPECTED_CONTENT_LENGTH && !req.is_chunked_transfer()) {
            if (fuzz::rename_repeated_header(input, ours.header_end, "Content-Length")) return "repeated identical Content-Length";
        }

        if (req.is_chunked_transfer()) {
            // Whitespace in chunk extensions: the grammar allows BWS before and after ";" and
            // "=" (RFC 9112 section 7.1.1), llhttp does not. They are skipped, after the size
            if (theirs.reason.find("chunk extension") != std::string::npos &&
                fuzz::remove_chunk_extensions_with_bws(input, ours.header_end)) {
                return "whitespace in a chunk extension";
            }

            // llhttp processes trailer fields like header fields, rejecting Content-Length and
            // Transfer-Encoding there. We ignore the trailer section: it is never merged into
            // the headers, so they cannot change the framing
            if ((theirs.error == HPE_INVALID_CONTENT_LENGTH || theirs.error == HPE_UNEXPECTED_CONTENT_LENGTH ||
                 theirs.error == HPE_INVALID_TRANSFER_ENCODING) &&
                fuzz::rename_trailers(input, ours.header_end, ours.end)) {
                return "framing field in the trailer section";
            }
        }

        return nullptr;
    }

    [[noreturn]] void disagree(const char* what, std::string_view input, const fuzz::server_result& ours, const llhttp_result& llhttp,
                               const fuzz::beast_result& beast) {
        fuzz::fail(what, describe_all(input, ours, llhttp, beast));
    }

    bool trace = std::getenv("FUZZ_TRACE") != nullptr;

    // Compare the parsers on the request at the start of `data`. Returns the bytes of the
    // request when we and llhttp accept it and the connection would carry another one, 0
    // otherwise.
    size_t compare(const uint8_t* data, size_t size, int normalizations = 0) {
        fuzz::server_result ours = fuzz::process_request(data, size);
        llhttp_result theirs = llhttp_process(data, size);
        fuzz::beast_result beast = fuzz::beast_process(data, size);
        std::string_view input(reinterpret_cast<const char*>(data), size);

        if (trace) std::fprintf(stderr, "\n%s\n", describe_all(input, ours, theirs, beast).c_str());
        compare_beast(input, ours, theirs, beast);

        if (ours.result == outcome::rejected) return 0;  // stricter than llhttp, or both reject
        if (ours.result == outcome::incomplete) {
            // llhttp found the end of a request we would still read past
            if (theirs.result == outcome::accepted) disagree("we need more data for a request llhttp completed", input, ours, theirs, beast);
            return 0;
        }

        // We accept the request
        if (theirs.result != outcome::accepted) {
            std::string normalized(input);
            const char* known = normalizations < 4 ? normalize_known_discrepancy(ours, theirs, normalized) : nullptr;
            if (known) {
                known_discrepancies.add(known, describe_all(input, ours, theirs, beast));
                if (trace) std::fprintf(stderr, "normalized (%s): %s\n", known, fuzz::escape(normalized, 2048).c_str());
                size_t bytes = compare(reinterpret_cast<const uint8_t*>(normalized.data()), normalized.size(), normalizations + 1);
                // The rewrite is in the request line or headers: the end moves by the size difference
                return bytes ? bytes + size - normalized.size() : 0;
            }
            disagree(theirs.result == outcome::rejected ? "we accept a request llhttp rejects"
                                                        : "we complete a request llhttp needs more data for",
                     input, ours, theirs, beast);
        }

        auto& req = *ours.request;
        if (ours.end != theirs.end) disagree("the request ends at a different byte", input, ours, theirs, beast);
        if (req.is_chunked_transfer() != theirs.chunked) disagree("different body framing", input, ours, theirs, beast);
        if (!theirs.chunked && req.get_content_length() != theirs.content_length) {
            disagree("different Content-Length", input, ours, theirs, beast);
        }
        if (ours.body != theirs.body) disagree("different body", input, ours, theirs, beast);
        if (req.get_uri() != theirs.target) disagree("different target", input, ours, theirs, beast);
        if (req.get_http_version_major() != static_cast<int>(theirs.major) ||
            req.get_http_version_minor() != static_cast<int>(theirs.minor)) {
            disagree("different HTTP version", input, ours, theirs, beast);
        }
        if (!fuzz::same_method(req.get_method(), theirs.method)) disagree("different method", input, ours, theirs, beast);

        // After an upgrade (or CONNECT) the connection carries another protocol
        return req.keep_alive() && !theirs.upgrade ? ours.end : 0;
    }

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    size_t offset = 0;
    for (int request = 0; request < 8 && offset < size; request++) {
        size_t bytes = compare(data + offset, size - offset);
        if (bytes == 0) break;
        offset += bytes;
    }
    return 0;
}

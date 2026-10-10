// Differential fuzzing of request parsing and body framing against Boost.Beast.
//
// Each input is processed as the server does (fuzz::process_request: request_factory in
// headers-only mode, framing validation, body_reader) and by
// boost::beast::http::request_parser. Beast is used only by the fuzz targets, as a reference:
// it is never linked into the library.
//
// A request smuggling bug shows up as a disagreement on where a request ends: a front end
// that frames the stream like one parser and a back end like the other see different
// requests. So the target aborts when:
//  - we accept a request Beast rejects, or both accept but disagree on where it ends;
//  - both accept but disagree on the method, target, version, framing (chunked or
//    Content-Length) or the decoded body;
//  - one has a complete request where the other still needs more data.
// We may reject what Beast accepts: we are stricter on purpose in several places. These
// and the cases where Beast is the lenient one are documented in README.md, next to this
// file; the few classes where we accept something Beast rejects are recognized by
// known_discrepancy() below and skipped.
//
// Pipelined requests are followed: after an agreed request, the next one is parsed from
// the leftover by both, as long as the connection would stay open.

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include "fuzz_differential.hpp"

namespace beast_http = boost::beast::http;

namespace {

    using fuzz::beast_result;
    using fuzz::describe;

    /**
     * Discrepancies known to be acceptable, where we accept a request Beast rejects (see
     * README.md). If it is one of them, `input` is rewritten (with the same size and
     * framing) so that Beast no longer rejects it for that reason, and the reason returned;
     * comparing again then checks the rest of the request. nullptr if it is not one.
     */
    const char* normalize_known_discrepancy(const fuzz::server_result& ours, const beast_result& theirs, std::string& input) {
        auto& req = *ours.request;

        // HTTP/1.2 to HTTP/1.9: handled as HTTP/1.1 (RFC 9110, section 2.5), while Beast only
        // accepts 1.0 and 1.1
        if (theirs.error == beast_http::error::bad_version && req.get_http_version_minor() > 1) {
            input[input.find("\r\n") - 1] = '1';
            return "higher HTTP/1 minor version";
        }

        // Beast processes trailer fields like header fields: it rejects Content-Length or
        // Transfer-Encoding (framing fields a sender must not put in trailers) and invalid
        // Connection values there. We ignore the trailer section: it is never merged into
        // the headers, so they cannot change the framing.
        if (req.is_chunked_transfer() &&
            (theirs.error == beast_http::error::bad_transfer_encoding || theirs.error == beast_http::error::bad_content_length ||
             theirs.error == beast_http::error::multiple_content_length || theirs.error == beast_http::error::bad_value)) {
            if (fuzz::rename_trailers(input, ours.header_end, ours.end)) return "framing or Connection field in the trailer section";
        }

        // Beast rejects a Connection (or Proxy-Connection) value that is not a valid token
        // list. We only look for the tokens we know in it: it does not frame the message.
        if (theirs.error == beast_http::error::bad_value) {
            bool connection = fuzz::neutralize_header(input, ours.header_end, "Connection");
            bool proxy_connection = fuzz::neutralize_header(input, ours.header_end, "Proxy-Connection");
            if (connection || proxy_connection) return "Connection header that is not a token list";
        }

        return nullptr;
    }

    [[noreturn]] void disagree(const char* what, std::string_view input, const fuzz::server_result& ours, const beast_result& theirs) {
        fuzz::fail(what, "input:  " + fuzz::escape(input, 2048) + "\nours:   " + describe(ours) + "\nbeast:  " + describe(theirs));
    }

    // Compare both parsers on the request at the start of `data`. Returns the bytes of the
    // request when both accept it and the connection would carry another one, 0 otherwise.
    size_t compare(const uint8_t* data, size_t size, int normalizations = 0) {
        fuzz::server_result ours = fuzz::process_request(data, size);
        beast_result theirs = fuzz::beast_process(data, size);
        std::string_view input(reinterpret_cast<const char*>(data), size);

        using fuzz::outcome;
        if (ours.result == outcome::rejected) return 0;  // stricter than Beast, or both reject
        if (ours.result == outcome::incomplete) {
            // Beast found the end of a request we would still read past: unless it rejects it
            if (theirs.result == outcome::accepted) disagree("we need more data for a request Beast completed", input, ours, theirs);
            return 0;
        }

        // We accept the request
        if (theirs.result != outcome::accepted) {
            std::string normalized(input);
            if (normalizations < 4 && theirs.result == outcome::rejected && normalize_known_discrepancy(ours, theirs, normalized)) {
                // Same size and framing: where it ends applies to the original input too
                return compare(reinterpret_cast<const uint8_t*>(normalized.data()), normalized.size(), normalizations + 1);
            }
            disagree(theirs.result == outcome::rejected ? "we accept a request Beast rejects"
                                                        : "we complete a request Beast needs more data for",
                     input, ours, theirs);
        }

        auto& req = *ours.request;
        if (ours.end != theirs.end) disagree("the request ends at a different byte", input, ours, theirs);
        if (req.is_chunked_transfer() != theirs.chunked) disagree("different body framing", input, ours, theirs);
        if (!theirs.chunked && req.get_content_length() != theirs.content_length) disagree("different Content-Length", input, ours, theirs);
        if (ours.body != theirs.body) disagree("different body", input, ours, theirs);
        if (req.get_uri() != theirs.target) disagree("different target", input, ours, theirs);
        if (req.get_http_version_major() * 10 + req.get_http_version_minor() != static_cast<int>(theirs.version)) {
            disagree("different HTTP version", input, ours, theirs);
        }
        if (!fuzz::same_method(req.get_method(), theirs.method)) disagree("different method", input, ours, theirs);

        return req.keep_alive() ? ours.end : 0;
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

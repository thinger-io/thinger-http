// Differential fuzzing of request parsing and body framing against Boost.Beast.
//
// Each input is processed as the server does (fuzz::process_request: request_factory in
// headers-only mode, framing validation, body_reader) and by
// boost::beast::http::request_parser. Beast is used only here, as a reference: it is never
// linked into the library.
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
#include <boost/algorithm/string/predicate.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/string_body.hpp>
#include "fuzz_common.hpp"

namespace beast_http = boost::beast::http;

namespace {

    struct beast_result {
        fuzz::outcome result = fuzz::outcome::incomplete;
        boost::beast::error_code error;
        std::string method;
        std::string target;
        unsigned version = 0;
        bool chunked = false;
        uint64_t content_length = 0;
        bool keep_alive = false;
        std::string body;
        size_t end = 0;
    };

    beast_result beast_process(const uint8_t* data, size_t size) {
        beast_result r;
        beast_http::request_parser<beast_http::string_body> parser;
        parser.eager(true);
        // No size limits other than the input itself (ours are checked by their own tests)
        parser.header_limit(UINT32_MAX);
        parser.body_limit(size);

        size_t offset = 0;
        while (!parser.is_done()) {
            boost::beast::error_code ec;
            size_t bytes = parser.put(boost::asio::buffer(data + offset, size - offset), ec);
            offset += bytes;
            if (ec == beast_http::error::need_more) {
                r.result = fuzz::outcome::incomplete;
                return r;
            }
            if (ec) {
                r.result = fuzz::outcome::rejected;
                r.error = ec;
                return r;
            }
            if (bytes == 0 && !parser.is_done()) {
                r.result = fuzz::outcome::incomplete;
                return r;
            }
        }

        auto& message = parser.get();
        r.result = fuzz::outcome::accepted;
        r.method = std::string(message.method_string());
        r.target = std::string(message.target());
        r.version = message.version();
        r.chunked = parser.chunked();
        r.content_length = parser.content_length().value_or(0);
        r.keep_alive = parser.keep_alive();
        r.body = message.body();
        r.end = offset;
        return r;
    }

    bool is_known_method(std::string_view method) {
        for (auto known : {"GET", "HEAD", "POST", "PUT", "DELETE", "TRACE", "OPTIONS", "CONNECT", "PATCH"}) {
            if (method == known) return true;
        }
        return false;
    }

    // Overwrite the values of the header lines named `name` in the header section of `input`
    // with 'x' (keeping their size). Returns whether there was any.
    bool neutralize_header(std::string& input, size_t header_end, std::string_view name) {
        bool found = false;
        size_t pos = input.find("\r\n") + 2;
        while (pos < header_end - 2) {
            size_t end = input.find("\r\n", pos);
            size_t colon = input.find(':', pos);
            if (colon < end && boost::iequals(std::string_view(input).substr(pos, colon - pos), name)) {
                for (size_t i = colon + 2; i < end; i++) input[i] = 'x';
                found = true;
            }
            pos = end + 2;
        }
        return found;
    }

    // Rename (first character to 'X') the lines in the body of a chunked request, between
    // `begin` and `end`, that start with a field Beast validates in trailers. A line in chunk
    // data may be renamed too: it changes the body for both parsers, not its framing.
    bool rename_trailers(std::string& input, size_t begin, size_t end) {
        bool found = false;
        for (size_t pos = begin; pos < end; pos++) {
            if (pos >= 2 && input[pos - 2] == '\r' && input[pos - 1] == '\n') {
                std::string_view line = std::string_view(input).substr(pos, end - pos);
                for (std::string_view name : {"Content-Length:", "Transfer-Encoding:", "Connection:", "Proxy-Connection:"}) {
                    if (line.size() >= name.size() && boost::iequals(line.substr(0, name.size()), name)) {
                        input[pos] = 'X';
                        found = true;
                    }
                }
            }
        }
        return found;
    }

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
            if (rename_trailers(input, ours.header_end, ours.end)) return "framing or Connection field in the trailer section";
        }

        // Beast rejects a Connection (or Proxy-Connection) value that is not a valid token
        // list. We only look for the tokens we know in it: it does not frame the message.
        if (theirs.error == beast_http::error::bad_value) {
            bool connection = neutralize_header(input, ours.header_end, "Connection");
            bool proxy_connection = neutralize_header(input, ours.header_end, "Proxy-Connection");
            if (connection || proxy_connection) return "Connection header that is not a token list";
        }

        return nullptr;
    }

    std::string describe(const fuzz::server_result& r) {
        std::string s = fuzz::to_string(r.result);
        if (r.result == fuzz::outcome::accepted) {
            auto& req = *r.request;
            s += " method=" + thinger::http::get_method(req.get_method()) + " target=" + fuzz::escape(req.get_uri()) +
                 " version=" + std::to_string(req.get_http_version_major()) + "." + std::to_string(req.get_http_version_minor()) +
                 " chunked=" + std::to_string(req.is_chunked_transfer()) +
                 " content_length=" + std::to_string(req.get_content_length()) +
                 " header_end=" + std::to_string(r.header_end) + " end=" + std::to_string(r.end) +
                 " body=[" + fuzz::escape(r.body) + "]";
        }
        return s;
    }

    std::string describe(const beast_result& r) {
        std::string s = fuzz::to_string(r.result);
        if (r.result == fuzz::outcome::rejected) s += " (" + r.error.message() + ")";
        if (r.result == fuzz::outcome::accepted) {
            s += " method=" + r.method + " target=" + fuzz::escape(r.target) +
                 " version=" + std::to_string(r.version / 10) + "." + std::to_string(r.version % 10) +
                 " chunked=" + std::to_string(r.chunked) + " content_length=" + std::to_string(r.content_length) +
                 " end=" + std::to_string(r.end) + " body=[" + fuzz::escape(r.body) + "]";
        }
        return s;
    }

    [[noreturn]] void disagree(const char* what, std::string_view input, const fuzz::server_result& ours, const beast_result& theirs) {
        fuzz::fail(what, "input:  " + fuzz::escape(input, 2048) + "\nours:   " + describe(ours) + "\nbeast:  " + describe(theirs));
    }

    // Compare both parsers on the request at the start of `data`. Returns the bytes of the
    // request when both accept it and the connection would carry another one, 0 otherwise.
    size_t compare(const uint8_t* data, size_t size, int normalizations = 0) {
        fuzz::server_result ours = fuzz::process_request(data, size);
        beast_result theirs = beast_process(data, size);
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
        auto method = req.get_method();
        if (method == thinger::http::method::UNKNOWN ? is_known_method(theirs.method)
                                                      : thinger::http::get_method(method) != theirs.method) {
            disagree("different method", input, ours, theirs);
        }

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

#ifndef THINGER_HTTP_FUZZ_DIFFERENTIAL_HPP
#define THINGER_HTTP_FUZZ_DIFFERENTIAL_HPP

// Helpers shared by the differential targets: Boost.Beast's request parser as a reference,
// a printable description of each result, and the rewrites used to step over discrepancies
// known to be acceptable. Beast is used only by the fuzz targets: it is never linked into
// the library.

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/string_body.hpp>
#include "fuzz_common.hpp"

namespace fuzz {

    namespace beast_http = boost::beast::http;

    struct beast_result {
        outcome result = outcome::incomplete;
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

    inline beast_result beast_process(const uint8_t* data, size_t size) {
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
                r.result = outcome::incomplete;
                return r;
            }
            if (ec) {
                r.result = outcome::rejected;
                r.error = ec;
                return r;
            }
            if (bytes == 0 && !parser.is_done()) {
                r.result = outcome::incomplete;
                return r;
            }
        }

        auto& message = parser.get();
        r.result = outcome::accepted;
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

    // Methods we know by name (any other token is kept as method::UNKNOWN)
    inline bool is_known_method(std::string_view method) {
        for (auto known : {"GET", "HEAD", "POST", "PUT", "DELETE", "TRACE", "OPTIONS", "CONNECT", "PATCH"}) {
            if (method == known) return true;
        }
        return false;
    }

    // Whether our method is `name`: the same known method, or an unknown one for both
    inline bool same_method(thinger::http::method ours, std::string_view name) {
        return ours == thinger::http::method::UNKNOWN ? !is_known_method(name) : thinger::http::get_method(ours) == name;
    }

    // Overwrite the values of the header lines named `name` in the header section of `input`
    // (`header_end` bytes, from a request we accepted) with 'x', keeping their size. Returns
    // whether there was any.
    inline bool neutralize_header(std::string& input, size_t header_end, std::string_view name) {
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

    // Rename (first character to 'X') the header lines named `name` in the header section of
    // `input` (`header_end` bytes, from a request we accepted) but the first one. Returns
    // whether there was any.
    inline bool rename_repeated_header(std::string& input, size_t header_end, std::string_view name) {
        bool first = true, found = false;
        size_t pos = input.find("\r\n") + 2;
        while (pos < header_end - 2) {
            size_t end = input.find("\r\n", pos);
            size_t colon = input.find(':', pos);
            if (colon < end && boost::iequals(std::string_view(input).substr(pos, colon - pos), name)) {
                if (!first) {
                    input[pos] = 'X';
                    found = true;
                }
                first = false;
            }
            pos = end + 2;
        }
        return found;
    }

    // Rename (first character to 'X') the lines in the body of a chunked request, between
    // `begin` and `end`, that start with a framing or connection field. A line in chunk data
    // may be renamed too: it changes the body for every parser, not its framing.
    inline bool rename_trailers(std::string& input, size_t begin, size_t end) {
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

    inline std::string describe(const server_result& r) {
        std::string s = to_string(r.result);
        if (r.result == outcome::accepted) {
            auto& req = *r.request;
            s += " method=" + thinger::http::get_method(req.get_method()) + " target=" + escape(req.get_uri()) +
                 " version=" + std::to_string(req.get_http_version_major()) + "." + std::to_string(req.get_http_version_minor()) +
                 " chunked=" + std::to_string(req.is_chunked_transfer()) +
                 " content_length=" + std::to_string(req.get_content_length()) +
                 " header_end=" + std::to_string(r.header_end) + " end=" + std::to_string(r.end) +
                 " body=[" + escape(r.body) + "]";
        }
        return s;
    }

    inline std::string describe(const beast_result& r) {
        std::string s = to_string(r.result);
        if (r.result == outcome::rejected) s += " (" + r.error.message() + ")";
        if (r.result == outcome::accepted) {
            s += " method=" + r.method + " target=" + escape(r.target) +
                 " version=" + std::to_string(r.version / 10) + "." + std::to_string(r.version % 10) +
                 " chunked=" + std::to_string(r.chunked) + " content_length=" + std::to_string(r.content_length) +
                 " end=" + std::to_string(r.end) + " body=[" + escape(r.body) + "]";
        }
        return s;
    }

}

#endif

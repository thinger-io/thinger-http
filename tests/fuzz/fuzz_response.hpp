#ifndef THINGER_HTTP_FUZZ_RESPONSE_HPP
#define THINGER_HTTP_FUZZ_RESPONSE_HPP

// Helpers shared by the response targets: process what a server sent before closing the
// connection the way the client does (response_factory, then the end of the stream), and
// parse a single head the way the reverse proxy does (headers-only mode).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "fuzz_common.hpp"
#include "thinger/http/client/response_factory.hpp"
#include "thinger/http/common/http_response.hpp"

namespace fuzz {

    using thinger::http::client_error;
    using thinger::http::response_factory;
    using body_framing = response_factory::body_framing;

    inline const char* to_string(body_framing framing) {
        switch (framing) {
            case body_framing::none: return "none";
            case body_framing::content_length: return "content-length";
            case body_framing::chunked: return "chunked";
            case body_framing::until_close: return "until-close";
        }
        return "?";
    }

    inline const char* method_name(thinger::http::method request_method) {
        switch (request_method) {
            case thinger::http::method::HEAD: return "HEAD";
            case thinger::http::method::CONNECT: return "CONNECT";
            default: return "GET";
        }
    }

    struct client_result {
        outcome result = outcome::incomplete;
        client_error error = client_error::none;
        std::shared_ptr<thinger::http::http_response> response;
        body_framing framing = body_framing::none;
        bool keep_alive = false;
        size_t end = 0;     // bytes of the response, interim responses included
        std::string body;
    };

    /**
     * Process the response at the start of `data` as the client does when the server sent
     * `data` and closed the connection: parsed as it arrives (in pieces from `gen`, all at
     * once without it), then the end of the stream. Each piece is an exact-size copy, so
     * reading past it is caught. A response that would need more than the input (its
     * Content-Length is larger, or the connection ends before its end) is incomplete.
     */
    inline client_result process_response(const uint8_t* data, size_t size, thinger::http::method request_method,
                                          split_generator* gen = nullptr) {
        client_result r;
        response_factory parser;
        parser.set_request_method(request_method);
        // A body is never larger than its input: a larger Content-Length cannot complete
        parser.set_max_content_size(size);

        boost::tribool parsed = boost::indeterminate;
        size_t offset = 0;
        while (offset < size) {
            size_t piece = gen ? gen->piece(size - offset) : size - offset;
            std::vector<uint8_t> buffer(data + offset, data + offset + piece);
            const uint8_t* begin = buffer.data();
            parsed = parser.parse_some(begin, buffer.data() + buffer.size());
            offset += static_cast<size_t>(begin - buffer.data());
            if (!boost::indeterminate(parsed)) break;
            check(begin == buffer.data() + buffer.size(), "parser needs more data with input left");
        }
        if (boost::indeterminate(parsed)) parsed = parser.end_of_stream();

        r.error = parser.error();
        r.framing = parser.get_body_framing();
        if (parsed) {
            check(r.error == client_error::none, "response accepted with an error");
            r.result = outcome::accepted;
            r.end = offset;
            r.keep_alive = parser.keep_alive();
            r.response = parser.consume_response();
            check(r.response != nullptr, "accepted response without an http_response");
            r.body = r.response->get_content();
            check(r.body.size() <= r.end, "body larger than the response");
        } else {
            check(r.error != client_error::none, "response rejected without an error");
            check(r.error != client_error::aborted, "aborted without a streaming callback");
            check(r.error != client_error::body_too_large || r.framing == body_framing::content_length,
                  "body larger than its input");
            r.result = r.error == client_error::incomplete_response || r.error == client_error::body_too_large
                           ? outcome::incomplete
                           : outcome::rejected;
            // errors are final
            uint8_t more[] = {'H', '\r', '\n'};
            const uint8_t* begin = more;
            check(bool(!parser.parse_some(begin, more + sizeof(more))) && begin == more && parser.error() == r.error,
                  "parsing went on after an error");
        }
        return r;
    }

    struct head_result {
        outcome result = outcome::incomplete;
        client_error error = client_error::none;
        std::shared_ptr<thinger::http::http_response> head;
        body_framing framing = body_framing::none;
        size_t end = 0;     // bytes of the head
    };

    // Parse the head at the start of `data` alone (headers-only mode, as the reverse proxy
    // does): interim responses are returned on their own
    inline head_result parse_head(const uint8_t* data, size_t size, thinger::http::method request_method) {
        head_result r;
        response_factory parser;
        parser.set_headers_only(true);
        parser.set_request_method(request_method);
        const uint8_t* begin = data;
        boost::tribool parsed = parser.parse_some(begin, data + size);
        r.error = parser.error();
        if (boost::indeterminate(parsed)) {
            check(begin == data + size, "head parser needs more data with input left");
            return r;
        }
        if (!parsed) {
            r.result = outcome::rejected;
            return r;
        }
        r.result = outcome::accepted;
        r.end = static_cast<size_t>(begin - data);
        r.framing = parser.get_body_framing();
        r.head = parser.consume_response();
        check(r.head != nullptr, "accepted head without an http_response");
        check(r.head->get_content().empty(), "head with content");
        return r;
    }

    inline std::string describe(const client_result& r) {
        std::string s = to_string(r.result);
        if (r.result != outcome::accepted) {
            s += " (" + boost::system::error_code(r.error).message() + ")";
        } else {
            auto& res = *r.response;
            s += " status=" + std::to_string(res.get_status_code()) +
                 " version=" + std::to_string(res.get_http_version_major()) + "." + std::to_string(res.get_http_version_minor()) +
                 " framing=" + to_string(r.framing) + " keep_alive=" + std::to_string(r.keep_alive) +
                 " end=" + std::to_string(r.end) + " body=[" + escape(r.body) + "]";
        }
        return s;
    }

}

#endif

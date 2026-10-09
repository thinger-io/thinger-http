#ifndef THINGER_HTTP_FUZZ_COMMON_HPP
#define THINGER_HTTP_FUZZ_COMMON_HPP

// Helpers shared by the fuzz targets: abort on a broken invariant (so libFuzzer reports it
// with the input), derive pseudo-random splits from the input itself, and process a request
// the way the server does (request_factory in headers-only mode, framing validation, then
// body_reader on the bytes read past the headers).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include "thinger/http/common/http_request.hpp"
#include "thinger/http/server/body_reader.hpp"
#include "thinger/http/server/request_factory.hpp"

namespace fuzz {

    // Abort with a message: a fuzzing failure, reported with the input that caused it
    [[noreturn]] inline void fail(const char* message, const std::string& detail = {}) {
        std::fprintf(stderr, "\nFUZZ INVARIANT BROKEN: %s\n", message);
        if (!detail.empty()) std::fprintf(stderr, "%s\n", detail.c_str());
        std::abort();
    }

    inline void check(bool condition, const char* message, const std::string& detail = {}) {
        if (!condition) fail(message, detail);
    }

    // Printable form of some bytes, for failure messages
    inline std::string escape(std::string_view data, size_t max_size = 512) {
        std::string out;
        for (size_t i = 0; i < data.size() && i < max_size; i++) {
            auto c = static_cast<unsigned char>(data[i]);
            if (c == '\r') out += "\\r";
            else if (c == '\n') out += "\\n";
            else if (c == '\\') out += "\\\\";
            else if (c < 32 || c >= 127) {
                char hex[8];
                std::snprintf(hex, sizeof(hex), "\\x%02x", c);
                out += hex;
            } else out += static_cast<char>(c);
        }
        if (data.size() > max_size) out += "...";
        return out;
    }

    // Deterministic generator seeded from the input, to split it (and size output buffers)
    // differently for each input while keeping every run reproducible
    class split_generator {
    public:
        explicit split_generator(const uint8_t* data, size_t size, uint64_t salt = 0) {
            // FNV-1a of the input
            state_ = 0xcbf29ce484222325ULL ^ salt;
            for (size_t i = 0; i < size; i++) {
                state_ ^= data[i];
                state_ *= 0x100000001b3ULL;
            }
        }

        // splitmix64
        uint64_t next() {
            uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            return z ^ (z >> 31);
        }

        // In [1, max]: mostly small pieces, now and then a large one
        size_t piece(size_t max) {
            if (max <= 1) return 1;
            uint64_t r = next();
            size_t limit = (r & 3) == 0 ? max : std::min<size_t>(max, 16);
            return 1 + static_cast<size_t>((r >> 2) % limit);
        }

    private:
        uint64_t state_;
    };

    // Run a coroutine to completion on a reused io_context (body_reader is a coroutine API;
    // with no source it never waits on I/O)
    template<typename T>
    T run(thinger::awaitable<T> awaitable) {
        static boost::asio::io_context ioc;
        std::optional<T> result;
        std::exception_ptr error;
        boost::asio::co_spawn(ioc, std::move(awaitable), [&](std::exception_ptr e, T value) {
            if (e) error = e;
            else result = std::move(value);
        });
        ioc.restart();
        ioc.run();
        if (error) std::rethrow_exception(error);
        check(result.has_value(), "coroutine did not complete");
        return std::move(*result);
    }

    // Outcome of processing one request from a buffer
    enum class outcome { accepted, rejected, incomplete };

    inline const char* to_string(outcome o) {
        switch (o) {
            case outcome::accepted: return "accepted";
            case outcome::rejected: return "rejected";
            case outcome::incomplete: return "incomplete";
        }
        return "?";
    }

    struct server_result {
        outcome result = outcome::incomplete;
        std::shared_ptr<thinger::http::http_request> request;
        bool header_section_too_large = false;
        bool invalid_framing = false;       // headers parsed, but has_valid_framing() failed
        size_t header_end = 0;              // bytes of the request line and headers
        size_t end = 0;                     // bytes of the whole request (headers and body)
        std::string body;
    };

    /**
     * Process the request at the start of `data` as the server does: parse the request line
     * and headers with request_factory in headers-only mode (server_connection), reject
     * ambiguous framing (http_server_base::process_request), then read the body with
     * body_reader from the bytes following the headers (the read-ahead), which never reads
     * past the end of the body. The request ends where the read-ahead left by the body
     * reader starts. As there is no connection to read more data from, a request that needs
     * more bytes than `size` is incomplete.
     */
    inline server_result process_request(const uint8_t* data, size_t size) {
        server_result r;
        thinger::http::request_factory parser;
        parser.set_headers_only(true);

        const uint8_t* begin = data;
        const uint8_t* end = data + size;
        boost::tribool parsed = parser.parse(begin, end);
        if (!parsed) {
            r.result = outcome::rejected;
            r.header_section_too_large = parser.header_section_too_large();
            return r;
        }
        if (boost::indeterminate(parsed)) {
            r.result = outcome::incomplete;
            return r;
        }

        r.request = parser.consume_request();
        r.header_end = static_cast<size_t>(begin - data);
        check(r.request != nullptr, "accepted request without an http_request");

        if (!r.request->has_valid_framing()) {
            r.result = outcome::rejected;
            r.invalid_framing = true;
            return r;
        }

        size_t available = size - r.header_end;
        bool chunked = r.request->is_chunked_transfer();
        size_t content_length = r.request->pending_body_size();
        // A Content-Length body larger than the data cannot complete here (and is never
        // allocated: read_all() reserves the whole declared length)
        if (!chunked && content_length > available) {
            r.result = outcome::incomplete;
            return r;
        }

        thinger::http::body_reader reader;
        reader.set_framing(chunked, content_length);
        reader.set_read_ahead(begin, available);
        // A body can never be larger than the input it is decoded from
        bool ok = run(reader.read_all(r.body, size));
        if (!ok) {
            auto error = reader.error();
            check(error != thinger::http::body_error::none, "body read failed without an error");
            check(error != thinger::http::body_error::too_large, "body larger than its input");
            r.result = error == thinger::http::body_error::incomplete ? outcome::incomplete : outcome::rejected;
            return r;
        }
        check(!reader.has_pending(), "body read completed with pending data");

        size_t leftover = reader.read_ahead_available();
        check(leftover <= available, "read-ahead grew while reading the body");
        r.end = size - leftover;
        check(r.end >= r.header_end, "request ends before its headers");
        check(chunked || r.body.size() == content_length, "Content-Length body of a different size");
        check(chunked || r.end == r.header_end + content_length, "Content-Length body ends at the wrong byte");
        r.result = outcome::accepted;
        return r;
    }

}

#endif

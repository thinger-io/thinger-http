#ifndef THINGER_HTTP_SERVER_BODY_READER_HPP
#define THINGER_HTTP_SERVER_BODY_READER_HPP

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>
#include "../common/chunked_decoder.hpp"
#include "../../util/types.hpp"

namespace thinger::http {

    // Why reading a request body failed
    enum class body_error {
        none,
        too_large,      // larger than the allowed size
        malformed,      // invalid chunked framing
        incomplete,     // the connection ended before the body did
        bad_encoding    // the Content-Encoding (gzip, deflate) could not be decoded
    };

    /**
     * Reads a request body framed by Content-Length or by the chunked transfer coding: first
     * from the bytes already read past the request headers (read-ahead), then from the
     * connection. Reads never go past the end of the body, so any data following it (the
     * next pipelined request) is kept in the read-ahead for whoever reads next.
     */
    class body_reader {
    public:
        /// Reads up to `size` bytes from the connection: 0 on end of stream or error
        using source = std::function<thinger::awaitable<size_t>(uint8_t* buffer, size_t size)>;

        /// Bytes read from the connection at once while decoding chunked framing (never more
        /// than the connection read buffer, as the excess is handed to the next request)
        static constexpr size_t framing_read_size = 4096;

        body_reader() = default;

        /// Body framing: chunked, or `content_length` bytes
        void set_framing(bool chunked, size_t content_length);

        /// Connection to read from once the read-ahead is exhausted (none: the body is only
        /// what is in the read-ahead)
        void set_source(source src) { source_ = std::move(src); }

        /// Maximum body size (none by default): reading a larger body fails (too_large), a
        /// Content-Length one at its first read, before reading anything, and a chunked one
        /// once it exceeds the limit
        void set_limit(size_t limit) { limit_ = limit; }
        size_t limit() const { return limit_; }

        void set_read_ahead(const uint8_t* data, size_t size);
        size_t read_ahead_available() const;

        /// Remove and return the unconsumed read-ahead bytes (data following the body)
        std::vector<uint8_t> take_read_ahead();

        /// Read exactly `size` body bytes, or less at the end of the body or on error
        thinger::awaitable<size_t> read(uint8_t* buffer, size_t size);

        /// Read up to `max_size` body bytes; 0 at the end of the body or on error
        thinger::awaitable<size_t> read_some(uint8_t* buffer, size_t max_size);

        /// Append the rest of the body to `body`, failing (too_large) if `body` would exceed
        /// `max_size` bytes. A Content-Length body is checked before reading anything.
        thinger::awaitable<bool> read_all(std::string& body, size_t max_size);

        /// Read and drop the rest of the body; false if it exceeds `max_size` bytes or fails
        thinger::awaitable<bool> discard(size_t max_size);

        /// Whether part of the body is still unread (false once reading failed)
        bool has_pending() const;

        bool is_chunked() const { return chunked_; }

        body_error error() const { return error_; }
        void set_error(body_error error) { error_ = error; }

    private:
        source source_;
        bool chunked_ = false;
        size_t remaining_ = 0;      // unread bytes of a Content-Length body
        size_t read_ = 0;           // body bytes read
        size_t limit_ = std::numeric_limits<size_t>::max();
        chunked_decoder decoder_;
        body_error error_ = body_error::none;

        std::vector<uint8_t> read_ahead_;
        size_t read_ahead_offset_ = 0;

        /// Read raw bytes: from the read-ahead first, then from the source
        thinger::awaitable<size_t> read_raw(uint8_t* buffer, size_t max_size);

        /// Refill the (empty) read-ahead from the source; false on end of stream
        thinger::awaitable<bool> fill_read_ahead();

        thinger::awaitable<size_t> read_some_length(uint8_t* buffer, size_t max_size);
        thinger::awaitable<size_t> read_some_chunked(uint8_t* buffer, size_t max_size);
    };

}

#endif

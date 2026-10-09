#ifndef THINGER_HTTP_SERVER_BODY_READER_HPP
#define THINGER_HTTP_SERVER_BODY_READER_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
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
     * Strict decoder of the chunked transfer coding (RFC 9112, section 7.1). It is fed with
     * the raw bytes following the request headers and stops right after the end of the body
     * (last chunk and trailer section), so whatever follows is left for the next request.
     * Anything that does not follow the grammar fails the decoding: there is no recovery,
     * as a lenient parser would frame the body differently than a proxy in front of it.
     */
    class chunked_decoder {
    public:
        // Longest chunk size line (size and extensions) and trailer section accepted
        static constexpr size_t max_line_size = 4096;
        static constexpr size_t max_trailer_size = 8192;

        /// Decode up to `size` input bytes, writing up to `out_size` body bytes to `out`
        /// (their number is stored in `produced`). Returns the input bytes consumed: it stops
        /// at the end of the body, on error, or when `out` is full.
        size_t decode(const uint8_t* in, size_t size, uint8_t* out, size_t out_size, size_t& produced);

        /// Bytes of the current chunk that can be read straight from the input (0 if the
        /// decoder is not within chunk data), and notify that `size` of them were read so
        size_t data_pending() const;
        void data_consumed(size_t size);

        bool done() const { return state_ == state::done; }
        bool failed() const { return state_ == state::error; }

    private:
        // The states of the chunk size line (size and extensions) come first, up to ext_bws
        enum class state {
            size_start,         // first hex digit of the chunk size
            size,               // more hex digits, ';' or CR
            ext_name_start,     // after ';': whitespace or the first character of a name
            ext_name,           // within an extension name
            ext_after_name,     // whitespace after an extension name, then '=' or ';'
            ext_value_start,    // after '=': whitespace, then a token or a quoted string
            ext_value,          // within a token value
            ext_quoted,         // within a quoted string value
            ext_quoted_pair,    // after a backslash in a quoted string
            ext_after_quoted,   // after a quoted string: whitespace, ';' or CR
            ext_bws,            // whitespace after a value, before the next ';'
            size_lf,
            data,
            data_cr,
            data_lf,
            trailer_start,      // start of a trailer field line, or CR of the final CRLF
            trailer_name,       // within a trailer field name, up to ':'
            trailer,            // within a trailer field value
            trailer_lf,
            end_lf,             // LF of the final CRLF
            done,
            error
        };

        state state_ = state::size_start;
        size_t chunk_size_ = 0;     // size being parsed, then data left in the chunk
        size_t line_size_ = 0;      // bytes of the current size line
        size_t trailer_size_ = 0;   // bytes of the trailer section

        bool fail() { state_ = state::error; return false; }
        bool consume(uint8_t byte);
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

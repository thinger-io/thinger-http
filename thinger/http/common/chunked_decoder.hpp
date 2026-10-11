#ifndef THINGER_HTTP_COMMON_CHUNKED_DECODER_HPP
#define THINGER_HTTP_COMMON_CHUNKED_DECODER_HPP

#include <cstddef>
#include <cstdint>

namespace thinger::http {

    /**
     * Strict decoder of the chunked transfer coding (RFC 9112, section 7.1), of request and
     * response bodies. It is fed with the raw bytes following the header section and stops
     * right after the end of the body (last chunk and trailer section), so whatever follows
     * is left for the next message.
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

}

#endif

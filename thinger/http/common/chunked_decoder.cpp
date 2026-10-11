#include "chunked_decoder.hpp"
#include "syntax.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace thinger::http {

    namespace {

        int hex_value(uint8_t c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        // qdtext = HTAB / SP / %x21 / %x23-5B / %x5D-7E / obs-text (RFC 9110, section 5.6.4)
        bool is_qdtext(uint8_t c) {
            return c == '\t' || c == ' ' || c == 0x21 || (c >= 0x23 && c <= 0x5B) || (c >= 0x5D && c <= 0x7E) || c >= 0x80;
        }

    }

    size_t chunked_decoder::decode(const uint8_t* in, size_t size, uint8_t* out, size_t out_size, size_t& produced) {
        produced = 0;
        size_t i = 0;
        while (i < size && state_ != state::done && state_ != state::error) {
            if (state_ == state::data) {
                if (produced == out_size) break;
                size_t bytes = std::min({chunk_size_, size - i, out_size - produced});
                std::memcpy(out + produced, in + i, bytes);
                produced += bytes;
                i += bytes;
                data_consumed(bytes);
                continue;
            }
            if (!consume(in[i])) break;
            i++;
        }
        return i;
    }

    size_t chunked_decoder::data_pending() const {
        return state_ == state::data ? chunk_size_ : 0;
    }

    void chunked_decoder::data_consumed(size_t size) {
        chunk_size_ -= std::min(size, chunk_size_);
        if (chunk_size_ == 0) state_ = state::data_cr;
    }

    bool chunked_decoder::consume(uint8_t c) {
        // bound the chunk size line: size and extensions (the states up to ext_bws)
        if (state_ <= state::ext_bws && ++line_size_ > max_line_size) return fail();

        switch (state_) {
            // chunk-size = 1*HEXDIG, then [ chunk-ext ] CRLF, with no whitespace between them
            case state::size_start:
            case state::size: {
                int digit = hex_value(c);
                if (digit >= 0) {
                    if (chunk_size_ > (std::numeric_limits<size_t>::max() >> 4)) return fail();
                    chunk_size_ = chunk_size_ * 16 + digit;
                    state_ = state::size;
                    return true;
                }
                if (state_ == state::size_start) return fail();
                if (c == ';') {
                    state_ = state::ext_name_start;
                    return true;
                }
                if (c == '\r') {
                    state_ = state::size_lf;
                    return true;
                }
                return fail();
            }

            // chunk-ext = *( BWS ";" BWS ext-name [ BWS "=" BWS ext-val ] ), ext-val = token /
            // quoted-string (RFC 9112, section 7.1.1). Extensions are ignored, but must follow
            // the grammar: anything else could be read differently by another parser.
            case state::ext_name_start:
                if (syntax::is_whitespace(c)) return true;
                if (!syntax::is_token_char(c)) return fail();
                state_ = state::ext_name;
                return true;

            case state::ext_name:
                if (syntax::is_token_char(c)) return true;
                if (syntax::is_whitespace(c)) {
                    state_ = state::ext_after_name;
                    return true;
                }
                if (c == '=') {
                    state_ = state::ext_value_start;
                    return true;
                }
                if (c == ';') {
                    state_ = state::ext_name_start;
                    return true;
                }
                if (c == '\r') {
                    state_ = state::size_lf;
                    return true;
                }
                return fail();

            case state::ext_after_name:
                if (syntax::is_whitespace(c)) return true;
                if (c == '=') {
                    state_ = state::ext_value_start;
                    return true;
                }
                if (c == ';') {
                    state_ = state::ext_name_start;
                    return true;
                }
                return fail();

            case state::ext_value_start:
                if (syntax::is_whitespace(c)) return true;
                if (c == '"') {
                    state_ = state::ext_quoted;
                    return true;
                }
                if (!syntax::is_token_char(c)) return fail();
                state_ = state::ext_value;
                return true;

            case state::ext_value:
                if (syntax::is_token_char(c)) return true;
                if (syntax::is_whitespace(c)) {
                    state_ = state::ext_bws;
                    return true;
                }
                if (c == ';') {
                    state_ = state::ext_name_start;
                    return true;
                }
                if (c == '\r') {
                    state_ = state::size_lf;
                    return true;
                }
                return fail();

            case state::ext_quoted:
                if (c == '"') {
                    state_ = state::ext_after_quoted;
                    return true;
                }
                if (c == '\\') {
                    state_ = state::ext_quoted_pair;
                    return true;
                }
                return is_qdtext(c) ? true : fail();

            // quoted-pair = "\" ( HTAB / SP / VCHAR / obs-text )
            case state::ext_quoted_pair:
                if (!syntax::is_field_value_char(c)) return fail();
                state_ = state::ext_quoted;
                return true;

            case state::ext_after_quoted:
                if (syntax::is_whitespace(c)) {
                    state_ = state::ext_bws;
                    return true;
                }
                if (c == ';') {
                    state_ = state::ext_name_start;
                    return true;
                }
                if (c == '\r') {
                    state_ = state::size_lf;
                    return true;
                }
                return fail();

            // whitespace after a value: only before another extension (BWS ";")
            case state::ext_bws:
                if (syntax::is_whitespace(c)) return true;
                if (c == ';') {
                    state_ = state::ext_name_start;
                    return true;
                }
                return fail();

            case state::size_lf:
                if (c != '\n') return fail();
                line_size_ = 0;
                state_ = chunk_size_ == 0 ? state::trailer_start : state::data;
                return true;

            case state::data_cr:
                if (c != '\r') return fail();
                state_ = state::data_lf;
                return true;

            case state::data_lf:
                if (c != '\n') return fail();
                chunk_size_ = 0;
                state_ = state::size_start;
                return true;

            // trailer-section = *( field-line CRLF ), then the final CRLF, with
            // field-line = field-name ":" OWS field-value OWS (RFC 9112, section 5)
            case state::trailer_start:
                if (++trailer_size_ > max_trailer_size) return fail();
                if (c == '\r') {
                    state_ = state::end_lf;
                    return true;
                }
                // a field line starts with its name (no obsolete line folding), so that what
                // is not a field line (e.g. a request line) is never taken as a trailer
                if (!syntax::is_token_char(c)) return fail();
                state_ = state::trailer_name;
                return true;

            case state::trailer_name:
                if (++trailer_size_ > max_trailer_size) return fail();
                if (c == ':') {
                    state_ = state::trailer;
                    return true;
                }
                return syntax::is_token_char(c) ? true : fail();

            case state::trailer:
                if (++trailer_size_ > max_trailer_size) return fail();
                if (c == '\r') {
                    state_ = state::trailer_lf;
                    return true;
                }
                return syntax::is_field_value_char(c) ? true : fail();

            case state::trailer_lf:
                if (c != '\n') return fail();
                state_ = state::trailer_start;
                return true;

            case state::end_lf:
                if (c != '\n') return fail();
                state_ = state::done;
                return true;

            default:
                return fail();
        }
    }

}

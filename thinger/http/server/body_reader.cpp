#include "body_reader.hpp"
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

        // Control characters are not allowed in extensions and trailers (HTAB is whitespace)
        bool is_ctl(uint8_t c) {
            return (c < 32 && c != '\t') || c == 127;
        }

    }

    // --- chunked_decoder ---

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
        switch (state_) {
            // chunk-size = 1*HEXDIG, then [ chunk-ext ] CRLF
            case state::size_start:
            case state::size: {
                if (++line_size_ > max_line_size) return fail();
                int digit = hex_value(c);
                if (digit >= 0) {
                    if (chunk_size_ > (std::numeric_limits<size_t>::max() >> 4)) return fail();
                    chunk_size_ = chunk_size_ * 16 + digit;
                    state_ = state::size;
                    return true;
                }
                if (state_ == state::size_start) return fail();
                if (c == ';') {
                    state_ = state::extension;
                    return true;
                }
                if (c == '\r') {
                    state_ = state::size_lf;
                    return true;
                }
                return fail();
            }

            // chunk-ext = *( BWS ";" BWS ext-name [ BWS "=" BWS ext-val ] ): ignored
            case state::extension:
                if (++line_size_ > max_line_size) return fail();
                if (c == '\r') {
                    state_ = state::size_lf;
                    return true;
                }
                return is_ctl(c) ? fail() : true;

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

            // trailer-section = *( field-line CRLF ), then the final CRLF
            case state::trailer_start:
                if (++trailer_size_ > max_trailer_size) return fail();
                if (c == '\r') {
                    state_ = state::end_lf;
                    return true;
                }
                // a field line starts with its name (no obsolete line folding)
                if (is_ctl(c) || c == ' ' || c == '\t') return fail();
                state_ = state::trailer;
                return true;

            case state::trailer:
                if (++trailer_size_ > max_trailer_size) return fail();
                if (c == '\r') {
                    state_ = state::trailer_lf;
                    return true;
                }
                return is_ctl(c) ? fail() : true;

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

    // --- body_reader ---

    void body_reader::set_framing(bool chunked, size_t content_length) {
        chunked_ = chunked;
        remaining_ = chunked ? 0 : content_length;
        decoder_ = {};
        error_ = body_error::none;
    }

    void body_reader::set_read_ahead(const uint8_t* data, size_t size) {
        if (data && size > 0) {
            read_ahead_.assign(data, data + size);
            read_ahead_offset_ = 0;
        }
    }

    size_t body_reader::read_ahead_available() const {
        return read_ahead_.size() > read_ahead_offset_ ? read_ahead_.size() - read_ahead_offset_ : 0;
    }

    std::vector<uint8_t> body_reader::take_read_ahead() {
        std::vector<uint8_t> data(read_ahead_.begin() + static_cast<std::ptrdiff_t>(read_ahead_.size() - read_ahead_available()),
                                  read_ahead_.end());
        read_ahead_.clear();
        read_ahead_offset_ = 0;
        return data;
    }

    bool body_reader::has_pending() const {
        if (error_ != body_error::none) return false;
        return chunked_ ? !decoder_.done() : remaining_ > 0;
    }

    thinger::awaitable<size_t> body_reader::read_raw(uint8_t* buffer, size_t max_size) {
        if (size_t available = read_ahead_available()) {
            size_t bytes = std::min(available, max_size);
            std::memcpy(buffer, read_ahead_.data() + read_ahead_offset_, bytes);
            read_ahead_offset_ += bytes;
            co_return bytes;
        }
        if (!source_) co_return 0;
        co_return co_await source_(buffer, max_size);
    }

    thinger::awaitable<bool> body_reader::fill_read_ahead() {
        read_ahead_.resize(framing_read_size);
        read_ahead_offset_ = 0;
        size_t bytes = source_ ? co_await source_(read_ahead_.data(), read_ahead_.size()) : 0;
        read_ahead_.resize(bytes);
        co_return bytes > 0;
    }

    thinger::awaitable<size_t> body_reader::read_some_length(uint8_t* buffer, size_t max_size) {
        size_t to_read = std::min(max_size, remaining_);
        if (to_read == 0) co_return 0;
        size_t bytes = co_await read_raw(buffer, to_read);
        if (bytes == 0) {
            error_ = body_error::incomplete;
            co_return 0;
        }
        remaining_ -= bytes;
        co_return bytes;
    }

    thinger::awaitable<size_t> body_reader::read_some_chunked(uint8_t* buffer, size_t max_size) {
        while (!decoder_.done()) {
            if (read_ahead_available() == 0) {
                // Within chunk data: read it straight into the caller buffer
                if (size_t pending = decoder_.data_pending()) {
                    size_t bytes = source_ ? co_await source_(buffer, std::min(pending, max_size)) : 0;
                    if (bytes == 0) {
                        error_ = body_error::incomplete;
                        co_return 0;
                    }
                    decoder_.data_consumed(bytes);
                    co_return bytes;
                }
                // Framing: decode it from the read-ahead, where anything read past the end
                // of the body stays for the next request
                if (!co_await fill_read_ahead()) {
                    error_ = body_error::incomplete;
                    co_return 0;
                }
            }

            size_t produced = 0;
            read_ahead_offset_ += decoder_.decode(read_ahead_.data() + read_ahead_offset_, read_ahead_available(),
                                                  buffer, max_size, produced);
            if (decoder_.failed()) {
                error_ = body_error::malformed;
                co_return 0;
            }
            if (produced > 0) co_return produced;
        }
        co_return 0;
    }

    thinger::awaitable<size_t> body_reader::read_some(uint8_t* buffer, size_t max_size) {
        if (error_ != body_error::none || max_size == 0) co_return 0;
        if (chunked_) co_return co_await read_some_chunked(buffer, max_size);
        co_return co_await read_some_length(buffer, max_size);
    }

    thinger::awaitable<size_t> body_reader::read(uint8_t* buffer, size_t size) {
        size_t total = 0;
        while (total < size) {
            size_t bytes = co_await read_some(buffer + total, size - total);
            if (bytes == 0) break;
            total += bytes;
        }
        co_return total;
    }

    thinger::awaitable<bool> body_reader::read_all(std::string& body, size_t max_size) {
        if (error_ != body_error::none) co_return false;

        if (!chunked_) {
            // Known size: reject it before allocating anything
            if (remaining_ > max_size || body.size() > max_size - remaining_) {
                error_ = body_error::too_large;
                co_return false;
            }
            size_t offset = body.size();
            size_t pending = remaining_;
            body.resize(offset + pending);
            size_t bytes = co_await read(reinterpret_cast<uint8_t*>(body.data()) + offset, pending);
            body.resize(offset + bytes);
            co_return bytes == pending;
        }

        uint8_t buffer[8192];
        while (size_t bytes = co_await read_some(buffer, sizeof(buffer))) {
            if (bytes > max_size || body.size() > max_size - bytes) {
                error_ = body_error::too_large;
                co_return false;
            }
            body.append(reinterpret_cast<char*>(buffer), bytes);
        }
        co_return error_ == body_error::none;
    }

    thinger::awaitable<bool> body_reader::discard(size_t max_size) {
        if (error_ != body_error::none) co_return false;
        if (!has_pending()) co_return true;
        if (!chunked_ && remaining_ > max_size) co_return false;

        uint8_t buffer[8192];
        size_t discarded = 0;
        while (has_pending()) {
            size_t bytes = co_await read_some(buffer, sizeof(buffer));
            if (bytes == 0) break;
            discarded += bytes;
            if (discarded > max_size) co_return false;
        }
        co_return error_ == body_error::none && !has_pending();
    }

}

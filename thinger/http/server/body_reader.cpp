#include "body_reader.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace thinger::http {

    void body_reader::set_framing(bool chunked, size_t content_length) {
        chunked_ = chunked;
        remaining_ = chunked ? 0 : content_length;
        read_ = 0;
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
        // A Content-Length body over the limit fails before anything is read
        if (!chunked_ && remaining_ > 0 && (remaining_ > limit_ || read_ > limit_ - remaining_)) {
            error_ = body_error::too_large;
            co_return 0;
        }
        size_t bytes = chunked_ ? co_await read_some_chunked(buffer, max_size)
                                : co_await read_some_length(buffer, max_size);
        read_ += bytes;
        if (read_ > limit_) {
            error_ = body_error::too_large;
            co_return 0;
        }
        co_return bytes;
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

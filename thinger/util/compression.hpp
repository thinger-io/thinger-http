#ifndef THINGER_UTIL_COMPRESSION_HPP
#define THINGER_UTIL_COMPRESSION_HPP

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <zlib.h>
#include "ascii.hpp"

namespace thinger::util {

namespace detail {

    enum class inflate_format { zlib, gzip, raw };

    // Inflate `data` up to `max_size` bytes, checked as it is inflated (a decompression bomb
    // takes no more memory than the limit). gzip decodes every member of the data, one after
    // another, and accepts zero bytes after the last one (padding, as Node and gzip(1) do);
    // anything else after the end of the data fails: a zlib or raw deflate stream must take
    // the whole data, and a gzip member be followed by another one (as Go does).
    inline std::optional<std::string> inflate_data(std::string_view data, inflate_format format,
                                                   size_t max_size, bool* too_large) {
        if (too_large) *too_large = false;

        int window_bits = format == inflate_format::gzip ? 15 + 16 : format == inflate_format::raw ? -15 : 15;
        z_stream strm{};
        if (inflateInit2(&strm, window_bits) != Z_OK) {
            return std::nullopt;
        }
        struct end_guard {
            z_stream& strm;
            ~end_guard() { inflateEnd(&strm); }
        } guard{strm};

        // The input is given in pieces fitting in avail_in
        size_t consumed = 0;
        auto feed = [&] {
            if (strm.avail_in != 0 || consumed == data.size()) return;
            auto size = std::min<size_t>(data.size() - consumed, std::numeric_limits<uInt>::max());
            strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data() + consumed));
            strm.avail_in = static_cast<uInt>(size);
            consumed += size;
        };

        std::string result;
        char buffer[16384];
        while (true) {
            feed();
            strm.next_out = reinterpret_cast<Bytef*>(buffer);
            strm.avail_out = sizeof(buffer);

            // Anything but progress is an error; Z_BUF_ERROR means the input ended before
            // the compressed stream did (it would never reach Z_STREAM_END)
            int ret = inflate(&strm, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END) {
                return std::nullopt;
            }

            size_t produced = sizeof(buffer) - strm.avail_out;
            if (produced > max_size - result.size()) {
                if (too_large) *too_large = true;
                return std::nullopt;
            }
            result.append(buffer, produced);
            if (ret != Z_STREAM_END) continue;

            // End of the stream (or of a gzip member): what follows it, if anything
            auto rest = data.substr(consumed - strm.avail_in);
            if (rest.empty()) return result;
            if (format != inflate_format::gzip) return std::nullopt;
            if (rest.find_first_not_of('\0') == std::string_view::npos) return result;
            if (inflateReset(&strm) != Z_OK) return std::nullopt;
        }
    }

    // Whether `data` starts with a zlib header (RFC 1950, 2.2): deflate method, a window of
    // at most 32 KB, and a check of both bytes
    inline bool zlib_header(std::string_view data) {
        if (data.size() < 2) return false;
        auto cmf = static_cast<unsigned char>(data[0]);
        auto flg = static_cast<unsigned char>(data[1]);
        return (cmf & 0x0f) == 8 && (cmf >> 4) <= 7 && ((cmf << 8) | flg) % 31 == 0;
    }

}

class gzip {
public:
    // Compress string to gzip format
    static std::optional<std::string> compress(const std::string& data) {
        z_stream strm{};
        // windowBits = 15 + 16 enables gzip encoding
        if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            return std::nullopt;
        }

        strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        strm.avail_in = static_cast<uInt>(data.size());

        std::string result;
        result.resize(deflateBound(&strm, data.size()));

        strm.next_out = reinterpret_cast<Bytef*>(result.data());
        strm.avail_out = static_cast<uInt>(result.size());

        int ret = deflate(&strm, Z_FINISH);
        deflateEnd(&strm);

        if (ret != Z_STREAM_END) {
            return std::nullopt;
        }

        result.resize(strm.total_out);
        return result;
    }

    // Decompress gzip data: all its members, concatenated (RFC 1952, 2.2), and zero bytes
    // after them (padding). Fails on invalid or truncated data, other bytes after a member,
    // or if the result would exceed `max_size` bytes (then `too_large` is set, if given).
    static std::optional<std::string> decompress(std::string_view data,
                                                 size_t max_size = std::numeric_limits<size_t>::max(),
                                                 bool* too_large = nullptr) {
        return detail::inflate_data(data, detail::inflate_format::gzip, max_size, too_large);
    }

    // Check if data might be gzip compressed (by checking magic bytes)
    static bool is_gzip(const std::string& data) {
        return data.size() >= 2 &&
               static_cast<unsigned char>(data[0]) == 0x1f &&
               static_cast<unsigned char>(data[1]) == 0x8b;
    }
};

class deflate {
public:
    // Compress string using deflate (zlib format)
    static std::optional<std::string> compress(const std::string& data) {
        z_stream strm{};
        // windowBits = 15 for zlib format
        if (deflateInit(&strm, Z_DEFAULT_COMPRESSION) != Z_OK) {
            return std::nullopt;
        }

        strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        strm.avail_in = static_cast<uInt>(data.size());

        std::string result;
        result.resize(deflateBound(&strm, data.size()));

        strm.next_out = reinterpret_cast<Bytef*>(result.data());
        strm.avail_out = static_cast<uInt>(result.size());

        int ret = ::deflate(&strm, Z_FINISH);
        deflateEnd(&strm);

        if (ret != Z_STREAM_END) {
            return std::nullopt;
        }

        result.resize(strm.total_out);
        return result;
    }

    // Decompress deflate data: the zlib format, or raw deflate if it does not start with a
    // zlib header (as some servers send it; curl and browsers accept it too). Fails on
    // invalid or truncated data, bytes after its end, or if the result would exceed
    // `max_size` bytes (then `too_large` is set, if given).
    static std::optional<std::string> decompress(std::string_view data,
                                                 size_t max_size = std::numeric_limits<size_t>::max(),
                                                 bool* too_large = nullptr) {
        auto format = data.size() < 2 || detail::zlib_header(data) ? detail::inflate_format::zlib
                                                                    : detail::inflate_format::raw;
        return detail::inflate_data(data, format, max_size, too_large);
    }
};

// Outcome of decode_content()
enum class content_decoding {
    decoded,        // the body is decoded (or only listed identity)
    unsupported,    // a coding other than gzip, x-gzip, deflate or identity: the body is kept
    invalid,        // not valid data of its codings (corrupt, truncated, trailing bytes), or too many codings
    too_large       // larger than the maximum size once decoded
};

// Most codings a body may be encoded with (as curl and Node's fetch, against chains of
// decompressions taking a decoded size each)
inline constexpr size_t max_content_codings = 5;

// Decode a body, in place, as its Content-Encoding (`codings`: its comma-separated values, of
// every header) says (RFC 9110, 8.4): the codings were applied in the order listed, so they
// are undone in reverse. Names are case-insensitive, and x-gzip is gzip (8.4.1.3). Each step
// stops at `max_size` bytes decoded. A body listing a coding that is not supported is kept as
// it is (as Node's fetch does), with nothing decoded.
inline content_decoding decode_content(std::string_view codings, std::string& body, size_t max_size) {
    using ::thinger::util::ascii::iequals;
    enum class coding { gzip, deflate };
    coding listed[max_content_codings];
    size_t count = 0;
    bool unsupported = false;
    size_t elements = 0;
    while (!codings.empty()) {
        auto comma = codings.find(',');
        auto element = codings.substr(0, comma);
        codings = comma == std::string_view::npos ? std::string_view{} : codings.substr(comma + 1);
        auto first = element.find_first_not_of(" \t");
        if (first == std::string_view::npos) continue;
        element = element.substr(first, element.find_last_not_of(" \t") - first + 1);

        if (++elements > max_content_codings) return content_decoding::invalid;
        if (iequals(element, "gzip") || iequals(element, "x-gzip")) {
            listed[count++] = coding::gzip;
        } else if (iequals(element, "deflate")) {
            listed[count++] = coding::deflate;
        } else if (!iequals(element, "identity")) {
            unsupported = true;
        }
    }
    if (unsupported) return content_decoding::unsupported;

    while (count > 0) {
        bool too_large = false;
        auto decoded = listed[--count] == coding::gzip ? gzip::decompress(body, max_size, &too_large)
                                                       : deflate::decompress(body, max_size, &too_large);
        if (!decoded) return too_large ? content_decoding::too_large : content_decoding::invalid;
        body = std::move(*decoded);
    }
    return content_decoding::decoded;
}

} // namespace thinger::util

#endif // THINGER_UTIL_COMPRESSION_HPP

#ifndef THINGER_UTIL_COMPRESSION_HPP
#define THINGER_UTIL_COMPRESSION_HPP

#include <limits>
#include <string>
#include <optional>
#include <zlib.h>

namespace thinger::util {

namespace detail {

    // Inflate `data` (`window_bits` selects the zlib or gzip format), up to `max_size` bytes
    inline std::optional<std::string> inflate_data(const std::string& data, int window_bits,
                                                   size_t max_size, bool* too_large) {
        if (too_large) *too_large = false;

        z_stream strm{};
        if (inflateInit2(&strm, window_bits) != Z_OK) {
            return std::nullopt;
        }

        strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        strm.avail_in = static_cast<uInt>(data.size());

        std::string result;
        char buffer[16384];

        int ret;
        do {
            strm.next_out = reinterpret_cast<Bytef*>(buffer);
            strm.avail_out = sizeof(buffer);

            // Anything but progress is an error; Z_BUF_ERROR means the input ended before
            // the compressed stream did (it would never reach Z_STREAM_END)
            ret = inflate(&strm, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END) {
                inflateEnd(&strm);
                return std::nullopt;
            }

            size_t produced = sizeof(buffer) - strm.avail_out;
            if (produced > max_size - result.size()) {
                if (too_large) *too_large = true;
                inflateEnd(&strm);
                return std::nullopt;
            }
            result.append(buffer, produced);
        } while (ret != Z_STREAM_END);

        inflateEnd(&strm);
        return result;
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

    // Decompress gzip data. Fails on invalid or truncated data, or if the result would
    // exceed `max_size` bytes (then `too_large` is set, if given).
    static std::optional<std::string> decompress(const std::string& data,
                                                 size_t max_size = std::numeric_limits<size_t>::max(),
                                                 bool* too_large = nullptr) {
        // windowBits = 15 + 16 enables gzip decoding
        return detail::inflate_data(data, 15 + 16, max_size, too_large);
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

    // Decompress deflate data (zlib format). Fails on invalid or truncated data, or if the
    // result would exceed `max_size` bytes (then `too_large` is set, if given).
    static std::optional<std::string> decompress(const std::string& data,
                                                 size_t max_size = std::numeric_limits<size_t>::max(),
                                                 bool* too_large = nullptr) {
        return detail::inflate_data(data, 15, max_size, too_large);
    }
};

} // namespace thinger::util

#endif // THINGER_UTIL_COMPRESSION_HPP

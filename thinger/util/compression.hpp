#ifndef THINGER_UTIL_COMPRESSION_HPP
#define THINGER_UTIL_COMPRESSION_HPP

#include <string>
#include <optional>
#include <zlib.h>

namespace thinger::util {

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

    // Decompress gzip data
    static std::optional<std::string> decompress(const std::string& data) {
        z_stream strm{};
        // windowBits = 15 + 16 enables gzip decoding
        if (inflateInit2(&strm, 15 + 16) != Z_OK) {
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

            ret = inflate(&strm, Z_NO_FLUSH);
            if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
                inflateEnd(&strm);
                return std::nullopt;
            }

            result.append(buffer, sizeof(buffer) - strm.avail_out);
        } while (ret != Z_STREAM_END);

        inflateEnd(&strm);
        return result;
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

    // Decompress deflate data (zlib format)
    static std::optional<std::string> decompress(const std::string& data) {
        z_stream strm{};
        if (inflateInit(&strm) != Z_OK) {
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

            ret = inflate(&strm, Z_NO_FLUSH);
            if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
                inflateEnd(&strm);
                return std::nullopt;
            }

            result.append(buffer, sizeof(buffer) - strm.avail_out);
        } while (ret != Z_STREAM_END);

        inflateEnd(&strm);
        return result;
    }
};

} // namespace thinger::util

#endif // THINGER_UTIL_COMPRESSION_HPP

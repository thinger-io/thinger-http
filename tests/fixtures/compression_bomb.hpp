#ifndef THINGER_TESTS_COMPRESSION_BOMB_HPP
#define THINGER_TESTS_COMPRESSION_BOMB_HPP

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <zlib.h>

// Decompression bombs for tests: valid gzip and zlib (deflate) streams of a few KB per MiB of
// zeros they expand to, built without compressing (nor holding) what they expand to. A
// deflate segment encoding 1 MiB of zeros, ended with a full flush, refers to nothing before
// it, so it is repeated as many times as MiB the bomb expands to.
namespace compression_bomb {

    inline constexpr size_t mib = 1048576;

    enum class format { gzip, deflate };

    // Raw deflate of 1 MiB of zeros, ended with a full flush (no final block)
    inline const std::string& zero_segment() {
        static const std::string segment = [] {
            z_stream strm{};
            if (deflateInit2(&strm, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
                throw std::runtime_error("deflateInit2");
            }
            std::string zeros(mib, '\0');
            std::string out(deflateBound(&strm, zeros.size()) + 64, '\0');
            strm.next_in = reinterpret_cast<Bytef*>(zeros.data());
            strm.avail_in = static_cast<uInt>(zeros.size());
            strm.next_out = reinterpret_cast<Bytef*>(out.data());
            strm.avail_out = static_cast<uInt>(out.size());
            int ret = deflate(&strm, Z_FULL_FLUSH);
            size_t size = out.size() - strm.avail_out;
            deflateEnd(&strm);
            if (ret != Z_OK || strm.avail_in != 0) throw std::runtime_error("deflate");
            out.resize(size);
            return out;
        }();
        return segment;
    }

    inline void put_le32(std::string& out, uint32_t value) {
        for (int i = 0; i < 4; ++i) out += static_cast<char>((value >> (8 * i)) & 0xff);
    }

    inline void put_be32(std::string& out, uint32_t value) {
        for (int i = 3; i >= 0; --i) out += static_cast<char>((value >> (8 * i)) & 0xff);
    }

    // A stream expanding to `mebibytes` MiB of zeros, with a valid trailer (CRC-32 and size
    // for gzip, Adler-32 for zlib)
    inline std::string zeros(format type, size_t mebibytes) {
        const std::string& segment = zero_segment();
        std::string zeros_mib(mib, '\0');
        const auto* data = reinterpret_cast<const Bytef*>(zeros_mib.data());
        const uLong crc_mib = crc32(crc32(0L, Z_NULL, 0), data, static_cast<uInt>(mib));
        const uLong adler_mib = adler32(adler32(0L, Z_NULL, 0), data, static_cast<uInt>(mib));

        std::string out;
        out.reserve(segment.size() * mebibytes + 32);
        if (type == format::gzip) {
            out.append("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 10);
        } else {
            out.append("\x78\xda", 2);
        }
        uLong crc = crc32(0L, Z_NULL, 0);
        uLong adler = adler32(0L, Z_NULL, 0);
        for (size_t i = 0; i < mebibytes; ++i) {
            out += segment;
            crc = crc32_combine(crc, crc_mib, static_cast<z_off_t>(mib));
            adler = adler32_combine(adler, adler_mib, static_cast<z_off_t>(mib));
        }
        // An empty final fixed Huffman block
        out.append("\x03\x00", 2);
        if (type == format::gzip) {
            put_le32(out, static_cast<uint32_t>(crc));
            put_le32(out, static_cast<uint32_t>((mebibytes * mib) & 0xffffffffu));
        } else {
            put_be32(out, static_cast<uint32_t>(adler));
        }
        return out;
    }

}

#endif

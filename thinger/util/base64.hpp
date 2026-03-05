#ifndef THINGER_UTIL_BASE64_HPP
#define THINGER_UTIL_BASE64_HPP

#include <string>
#include <cstdint>
#include <array>

namespace thinger::util {

class base64 {
private:
    static constexpr const char encode_table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    static const std::array<uint8_t, 256>& get_decode_table() {
        static const auto table = [] {
            std::array<uint8_t, 256> t{};
            t.fill(0xFF);
            for (uint8_t i = 0; i < 64; ++i) {
                t[static_cast<uint8_t>(encode_table[i])] = i;
            }
            return t;
        }();
        return table;
    }

    static bool is_base64(uint8_t c) {
        return get_decode_table()[c] != 0xFF;
    }

public:
    static std::string encode(const std::string& input) {
        return encode(reinterpret_cast<const unsigned char*>(input.data()), input.size());
    }

    static std::string encode(const unsigned char* data, size_t len) {
        std::string result;
        result.reserve(((len + 2) / 3) * 4);

        for (size_t i = 0; i < len; i += 3) {
            uint32_t n = static_cast<uint32_t>(data[i]) << 16;
            if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
            if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);

            result += encode_table[(n >> 18) & 0x3F];
            result += encode_table[(n >> 12) & 0x3F];
            result += (i + 1 < len) ? encode_table[(n >> 6) & 0x3F] : '=';
            result += (i + 2 < len) ? encode_table[n & 0x3F] : '=';
        }

        return result;
    }

    static std::string decode(const std::string& input) {
        const auto& table = get_decode_table();
        std::string result;
        result.reserve(input.size() * 3 / 4);

        uint32_t buf = 0;
        int bits = 0;

        for (char c : input) {
            if (c == '=' || c == '\0') break;

            uint8_t val = table[static_cast<uint8_t>(c)];
            if (val == 0xFF) continue;

            buf = (buf << 6) | val;
            bits += 6;

            if (bits >= 8) {
                bits -= 8;
                result += static_cast<char>((buf >> bits) & 0xFF);
            }
        }

        return result;
    }
};

} // namespace thinger::util

#endif // THINGER_UTIL_BASE64_HPP

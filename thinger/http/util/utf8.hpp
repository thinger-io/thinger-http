#ifndef THINGER_HTTP_UTF8_HPP
#define THINGER_HTTP_UTF8_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <fstream>

namespace thinger::http::utf8 {

// UTF-8 validation based on Unicode 6.0 Table 3-7 (Well-Formed UTF-8 Byte Sequences)
// http://www.unicode.org/versions/Unicode6.0.0/ch03.pdf - page 94
//
// Returns true if the data is valid UTF-8, false otherwise.
inline bool is_valid(const uint8_t* data, size_t len) {
    while (len > 0) {
        const uint8_t b1 = data[0];

        size_t bytes;

        // U+0000..U+007F: 00..7F
        if (b1 <= 0x7F) {
            bytes = 1;

        // U+0080..U+07FF: C2..DF, 80..BF
        } else if (b1 >= 0xC2 && b1 <= 0xDF) {
            if (len < 2 || data[1] < 0x80 || data[1] > 0xBF) return false;
            bytes = 2;

        // U+0800..U+FFFF: 3-byte sequences
        } else if (b1 >= 0xE0 && b1 <= 0xEF) {
            if (len < 3) return false;
            const uint8_t b2 = data[1];
            const uint8_t b3 = data[2];
            if (b3 < 0x80 || b3 > 0xBF) return false;

            if (b1 == 0xE0) {
                // U+0800..U+0FFF: E0, A0..BF, 80..BF
                if (b2 < 0xA0 || b2 > 0xBF) return false;
            } else if (b1 == 0xED) {
                // U+D000..U+D7FF: ED, 80..9F, 80..BF (surrogates excluded)
                if (b2 < 0x80 || b2 > 0x9F) return false;
            } else {
                // U+1000..U+CFFF, U+E000..U+FFFF: E1..EC/EE..EF, 80..BF, 80..BF
                if (b2 < 0x80 || b2 > 0xBF) return false;
            }
            bytes = 3;

        // U+10000..U+10FFFF: 4-byte sequences
        } else if (b1 >= 0xF0 && b1 <= 0xF4) {
            if (len < 4) return false;
            const uint8_t b2 = data[1];
            const uint8_t b3 = data[2];
            const uint8_t b4 = data[3];
            if (b3 < 0x80 || b3 > 0xBF) return false;
            if (b4 < 0x80 || b4 > 0xBF) return false;

            if (b1 == 0xF0) {
                // U+10000..U+3FFFF: F0, 90..BF, 80..BF, 80..BF
                if (b2 < 0x90 || b2 > 0xBF) return false;
            } else if (b1 == 0xF4) {
                // U+100000..U+10FFFF: F4, 80..8F, 80..BF, 80..BF
                if (b2 < 0x80 || b2 > 0x8F) return false;
            } else {
                // U+40000..U+FFFFF: F1..F3, 80..BF, 80..BF, 80..BF
                if (b2 < 0x80 || b2 > 0xBF) return false;
            }
            bytes = 4;

        } else {
            // Invalid leading byte (C0, C1, F5..FF, or continuation byte 80..BF)
            return false;
        }

        data += bytes;
        len -= bytes;
    }

    return true;
}

// Convenience overload for string_view
inline bool is_valid(std::string_view sv) {
    return is_valid(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
}

// Check if a file contains valid UTF-8 content
inline bool file_is_valid(const std::string& file_path) {
    std::ifstream input(file_path, std::ifstream::binary);
    if (!input.is_open()) return false;

    char buffer[4096];
    while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
        if (!is_valid(reinterpret_cast<const uint8_t*>(buffer), static_cast<size_t>(input.gcount()))) {
            return false;
        }
        if (input.eof()) break;
    }

    return true;
}

} // namespace thinger::http::utf8

#endif // THINGER_HTTP_UTF8_HPP

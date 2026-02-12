#ifndef THINGER_UTIL_SHA1_HPP
#define THINGER_UTIL_SHA1_HPP

#include <string>
#include <cstring>
#include <cstdint>

namespace thinger::util {

class sha1 {
private:
    static constexpr uint32_t K[] = {
        0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6
    };

    uint32_t H[5];
    unsigned char buffer[64];
    uint64_t count;
    
    static uint32_t rotate_left(uint32_t value, size_t count) {
        return (value << count) | (value >> (32 - count));
    }

    void process_block() {
        uint32_t W[80];
        
        // Copy block to W[0..15]
        for (int i = 0; i < 16; i++) {
            W[i] = (buffer[i * 4] << 24) | 
                   (buffer[i * 4 + 1] << 16) | 
                   (buffer[i * 4 + 2] << 8) | 
                   (buffer[i * 4 + 3]);
        }
        
        // Extend W[16..79]
        for (int i = 16; i < 80; i++) {
            W[i] = rotate_left(W[i-3] ^ W[i-8] ^ W[i-14] ^ W[i-16], 1);
        }
        
        uint32_t A = H[0];
        uint32_t B = H[1];
        uint32_t C = H[2];
        uint32_t D = H[3];
        uint32_t E = H[4];
        
        // Main loop
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (B & C) | ((~B) & D);
                k = K[0];
            } else if (i < 40) {
                f = B ^ C ^ D;
                k = K[1];
            } else if (i < 60) {
                f = (B & C) | (B & D) | (C & D);
                k = K[2];
            } else {
                f = B ^ C ^ D;
                k = K[3];
            }
            
            uint32_t temp = rotate_left(A, 5) + f + E + k + W[i];
            E = D;
            D = C;
            C = rotate_left(B, 30);
            B = A;
            A = temp;
        }
        
        H[0] += A;
        H[1] += B;
        H[2] += C;
        H[3] += D;
        H[4] += E;
    }

public:
    sha1() {
        reset();
    }

    void reset() {
        H[0] = 0x67452301;
        H[1] = 0xEFCDAB89;
        H[2] = 0x98BADCFE;
        H[3] = 0x10325476;
        H[4] = 0xC3D2E1F0;
        count = 0;
        memset(buffer, 0, sizeof(buffer));
    }

    void update(const unsigned char* data, size_t len) {
        while (len > 0) {
            size_t buffer_pos = count % 64;
            size_t to_copy = std::min(len, 64 - buffer_pos);
            
            memcpy(buffer + buffer_pos, data, to_copy);
            count += to_copy;
            data += to_copy;
            len -= to_copy;
            
            if ((count % 64) == 0) {
                process_block();
            }
        }
    }

    void update(const std::string& str) {
        update(reinterpret_cast<const unsigned char*>(str.c_str()), str.length());
    }

    std::string finalize() {
        // Padding
        size_t buffer_pos = count % 64;
        buffer[buffer_pos++] = 0x80;
        
        if (buffer_pos > 56) {
            memset(buffer + buffer_pos, 0, 64 - buffer_pos);
            process_block();
            buffer_pos = 0;
        }
        
        memset(buffer + buffer_pos, 0, 56 - buffer_pos);
        
        // Append length in bits
        uint64_t bit_count = count * 8;
        for (int i = 0; i < 8; i++) {
            buffer[56 + i] = (bit_count >> ((7 - i) * 8)) & 0xFF;
        }
        
        process_block();
        
        // Convert hash to string
        std::string result;
        result.reserve(20);
        for (int i = 0; i < 5; i++) {
            result += static_cast<char>((H[i] >> 24) & 0xFF);
            result += static_cast<char>((H[i] >> 16) & 0xFF);
            result += static_cast<char>((H[i] >> 8) & 0xFF);
            result += static_cast<char>(H[i] & 0xFF);
        }
        
        return result;
    }

    static std::string hash(const std::string& input) {
        sha1 hasher;
        hasher.update(input);
        return hasher.finalize();
    }
};

} // namespace thinger::util

#endif // THINGER_UTIL_SHA1_HPP
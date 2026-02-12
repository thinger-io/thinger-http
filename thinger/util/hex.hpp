#ifndef THINGER_UTIL_HEX_HPP
#define THINGER_UTIL_HEX_HPP

#include <string>
#include <sstream>
#include <iomanip>

namespace thinger {
namespace util {

inline std::string lowercase_hex_encode(const std::string& input) {
    std::ostringstream oss;
    for (unsigned char c : input) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
    return oss.str();
}

inline std::string uppercase_hex_encode(const std::string& input) {
    std::ostringstream oss;
    oss << std::uppercase;
    for (unsigned char c : input) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
    return oss.str();
}

} // namespace util
} // namespace thinger

#endif // THINGER_UTIL_HEX_HPP
#ifndef THINGER_UTIL_ASCII_HPP
#define THINGER_UTIL_ASCII_HPP

#include <string>
#include <string_view>

// ASCII case folding, for protocol elements (header names, tokens, host names). Unlike
// boost::iequals or boost::to_lower, it does not depend on the global locale, and does not
// copy it on each call (its reference count is shared by every thread).
namespace thinger::util::ascii {

    constexpr char to_lower(char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }

    constexpr bool iequals(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (to_lower(a[i]) != to_lower(b[i])) return false;
        }
        return true;
    }

    constexpr bool istarts_with(std::string_view value, std::string_view prefix) {
        return value.size() >= prefix.size() && iequals(value.substr(0, prefix.size()), prefix);
    }

    inline void to_lower(std::string& value) {
        for (auto& c : value) c = to_lower(c);
    }

}

#endif

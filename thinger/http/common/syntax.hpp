#ifndef THINGER_HTTP_COMMON_SYNTAX_HPP
#define THINGER_HTTP_COMMON_SYNTAX_HPP

#include <algorithm>
#include <cstddef>
#include <string_view>

// Character classes and common rules of the HTTP grammar (RFC 9110, section 5.6), for the
// parsers and for checking what is sent
namespace thinger::http::syntax {

    // tchar
    constexpr bool is_token_char(unsigned char c) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
        switch (c) {
            case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
            case '-': case '.': case '^': case '_': case '`': case '|': case '~':
                return true;
            default:
                return false;
        }
    }

    // token = 1*tchar
    constexpr bool is_token(std::string_view value) {
        return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return is_token_char(c); });
    }

    // SP / HTAB
    constexpr bool is_whitespace(unsigned char c) {
        return c == ' ' || c == '\t';
    }

    // Field values (and reason phrases): HTAB, SP, VCHAR and obs-text; never CR, LF, NUL or
    // another control character
    constexpr bool is_field_value_char(unsigned char c) {
        return c == '\t' || (c >= 0x20 && c != 0x7f);
    }

    constexpr bool is_field_value(std::string_view value) {
        return std::all_of(value.begin(), value.end(), [](unsigned char c) { return is_field_value_char(c); });
    }

    // Without leading and trailing whitespace (OWS)
    constexpr std::string_view trim(std::string_view value) {
        while (!value.empty() && is_whitespace(value.front())) value.remove_prefix(1);
        while (!value.empty() && is_whitespace(value.back())) value.remove_suffix(1);
        return value;
    }

    // Whether `predicate` holds for an element of a comma-separated list (RFC 9110, 5.6.1),
    // given without whitespace around it; empty elements are skipped. Stops at the first one.
    template<typename Predicate>
    constexpr bool any_element(std::string_view list, Predicate&& predicate) {
        while (!list.empty()) {
            auto comma = list.find(',');
            auto element = trim(list.substr(0, comma));
            if (!element.empty() && predicate(element)) return true;
            if (comma == std::string_view::npos) break;
            list.remove_prefix(comma + 1);
        }
        return false;
    }

    template<typename Function>
    constexpr void for_each_element(std::string_view list, Function&& function) {
        any_element(list, [&](std::string_view element) {
            function(element);
            return false;
        });
    }

    // Weight (qvalue, RFC 9110, 12.4.2) in thousandths: "0" or "1" with up to three decimals;
    // -1 if invalid
    constexpr int parse_qvalue(std::string_view value) {
        if (value.empty() || value.size() > 5) return -1;
        if (value[0] != '0' && value[0] != '1') return -1;
        int quality = (value[0] - '0') * 1000;
        if (value.size() == 1) return quality;
        if (value[1] != '.') return -1;
        int scale = 100;
        for (size_t i = 2; i < value.size(); ++i, scale /= 10) {
            if (value[i] < '0' || value[i] > '9') return -1;
            quality += (value[i] - '0') * scale;
        }
        return quality <= 1000 ? quality : -1;
    }

}

#endif

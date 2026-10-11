#ifndef THINGER_HTTP_COMMON_SYNTAX_HPP
#define THINGER_HTTP_COMMON_SYNTAX_HPP

#include <algorithm>
#include <string_view>

// Character classes of the HTTP grammar (RFC 9110, section 5.6), for the parsers and for
// checking what is sent
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

}

#endif

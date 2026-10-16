#include "url.hpp"
#include <array>

namespace thinger::http::util::url{

    namespace {
        constexpr char hex_chars[] = "0123456789ABCDEF";

        inline int hex_digit(char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        inline bool is_unreserved(char c) {
            return std::isalnum(static_cast<unsigned char>(c))
                || c == '-' || c == '_' || c == '.' || c == '~';
        }
    }

    // RFC 3986 Section 2.3: unreserved characters are not percent-encoded
    std::string url_encode(const std::string &value) {
        std::string result;
        result.reserve(value.size() * 1.2);

        for (unsigned char c : value) {
            if (is_unreserved(c)) {
                result += static_cast<char>(c);
            } else {
                result += '%';
                result += hex_chars[c >> 4];
                result += hex_chars[c & 0x0F];
            }
        }

        return result;
    }

    // RFC 3986 path encoding: also preserves '/' and '~'
    std::string uri_path_encode(const std::string &value) {
        std::string result;
        result.reserve(value.size() * 1.2);

        for (unsigned char c : value) {
            if (is_unreserved(c) || c == '/' ) {
                result += static_cast<char>(c);
            } else {
                result += '%';
                result += hex_chars[c >> 4];
                result += hex_chars[c & 0x0F];
            }
        }

        return result;
    }

    bool url_decode(const std::string &in, std::string &out) {
        out.clear();
        out.reserve(in.size());

        for (size_t i = 0; i < in.size(); ++i) {
            if (in[i] == '%') {
                if (i + 2 >= in.size()) return false;
                int hi = hex_digit(in[i + 1]);
                int lo = hex_digit(in[i + 2]);
                if (hi < 0 || lo < 0) return false;
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
            } else if (in[i] == '+') {
                out += ' ';
            } else {
                out += in[i];
            }
        }
        return true;
    }

    std::string url_decode(const std::string &in) {
        std::string out;
        if (url_decode(in, out)) {
            return out;
        }
        return {};
    }

    void parse_url_encoded_data(const std::string &data, std::multimap<std::string, std::string>& store) {
        auto start = data.cbegin();
        auto end = data.cend();
        parse_url_encoded_data(start, end, store);
    }

    void parse_url_encoded_data(std::string::const_iterator& start, std::string::const_iterator& end, std::multimap<std::string, std::string>& store) {
        if (start == end) return;

        auto it = start;
        while (it != end) {
            // find the end of the current key=value pair
            auto pair_end = std::find(it, end, '&');

            // find the '=' separator within the pair
            auto eq_pos = std::find(it, pair_end, '=');

            std::string key(it, eq_pos);
            std::string value;
            if (eq_pos != pair_end) {
                value.assign(eq_pos + 1, pair_end);
            }

            if (!key.empty()) {
                store.emplace(url_decode(key), url_decode(value));
            }

            it = (pair_end != end) ? pair_end + 1 : end;
        }

        start = it;
    }

    std::string get_url_encoded_data(const std::multimap<std::string, std::string>& store) {
        std::string result;
        bool first = true;
        for (const auto& [key, value] : store) {
            if (!first) result += '&';
            result += url_encode(key);
            result += '=';
            result += url_encode(value);
            first = false;
        }
        return result;
    }

    namespace {

        constexpr std::string_view redacted = "[redacted]";

        bool is_credential_parameter(std::string_view name) {
            static constexpr std::array<std::string_view, 8> names{
                "access_token", "authorization", "token", "api_key", "apikey", "key", "password", "client_secret"};
            return std::any_of(names.begin(), names.end(), [&](std::string_view candidate) {
                return candidate.size() == name.size() &&
                       std::equal(candidate.begin(), candidate.end(), name.begin(), [](char a, char b) {
                           return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                       });
            });
        }

        // "scheme://user:password@host...": the password redacted
        std::string redact_user_info(std::string_view uri) {
            auto scheme_end = uri.find("://");
            if (scheme_end == std::string_view::npos) return std::string(uri);
            auto authority_start = scheme_end + 3;
            auto authority_end = uri.find_first_of("/?#", authority_start);
            auto authority = uri.substr(authority_start, authority_end == std::string_view::npos ? std::string_view::npos
                                                                                                   : authority_end - authority_start);
            auto at = authority.rfind('@');
            if (at == std::string_view::npos) return std::string(uri);
            auto colon = authority.substr(0, at).find(':');
            if (colon == std::string_view::npos) return std::string(uri);
            std::string out(uri.substr(0, authority_start + colon + 1));
            out += redacted;
            out += uri.substr(authority_start + at);
            return out;
        }

    }

    std::string loggable_uri(std::string_view uri) {
        auto query = uri.find('?');
        if (query == std::string_view::npos) return redact_user_info(uri);
        auto fragment = uri.find('#', query);
        auto query_end = fragment == std::string_view::npos ? uri.size() : fragment;

        std::string out = redact_user_info(uri.substr(0, query + 1));
        auto pos = query + 1;
        while (pos <= query_end) {
            auto amp = uri.find('&', pos);
            auto end = (amp == std::string_view::npos || amp > query_end) ? query_end : amp;
            auto pair = uri.substr(pos, end - pos);
            auto eq = pair.find('=');
            if (eq != std::string_view::npos && is_credential_parameter(url_decode(std::string(pair.substr(0, eq))))) {
                out += pair.substr(0, eq + 1);
                out += redacted;
            } else {
                out += pair;
            }
            if (end == query_end) break;
            out += '&';
            pos = end + 1;
        }
        out += uri.substr(query_end);
        return out;
    }

}

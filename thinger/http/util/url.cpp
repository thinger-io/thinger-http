#include "url.hpp"

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

}

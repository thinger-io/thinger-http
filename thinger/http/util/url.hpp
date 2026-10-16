#ifndef THINGER_HTTP_UTIL_URL_HPP
#define THINGER_HTTP_UTIL_URL_HPP

#include <algorithm>
#include <cctype>
#include <string>
#include <map>
#include <string_view>

namespace thinger::http::util::url{

    std::string uri_path_encode(const std::string &value);

    std::string url_encode(const std::string &value);

    bool url_decode(const std::string &in, std::string &out);

    std::string url_decode(const std::string &in);

    void parse_url_encoded_data(const std::string &in, std::multimap<std::string, std::string>& store);

    void parse_url_encoded_data(std::string::const_iterator& start, std::string::const_iterator& end, std::multimap<std::string, std::string>& store);

    std::string get_url_encoded_data(const std::multimap<std::string, std::string>& store);

    // `uri` (a request target or a URL) for a log line: the values of the query parameters that
    // carry credentials (access_token, authorization, token, api_key, apikey, key, password,
    // client_secret, compared without case) and the password of the user info are replaced by
    // "[redacted]"; the rest is kept as is
    std::string loggable_uri(std::string_view uri);

}

#endif

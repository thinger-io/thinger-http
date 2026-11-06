#ifndef THINGER_HTTP_UTIL_URL_HPP
#define THINGER_HTTP_UTIL_URL_HPP

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <map>
#include <string_view>
#include <vector>

namespace thinger::http::util::url{

    std::string uri_path_encode(const std::string &value);

    std::string url_encode(const std::string &value);

    bool url_decode(const std::string &in, std::string &out);

    std::string url_decode(const std::string &in);

    namespace detail {
        // Percent-decodes the path of a URI into `out`; false on an invalid escape. Unlike
        // url_decode(), "+" stays literal: it means a space only in query and form data
        // (application/x-www-form-urlencoded), not in a path (RFC 3986; Node's URL.pathname
        // and Go's URL.Path keep it too)
        bool path_decode(std::string_view in, std::string &out);
    }

    void parse_url_encoded_data(const std::string &in, std::multimap<std::string, std::string>& store);

    void parse_url_encoded_data(std::string::const_iterator& start, std::string::const_iterator& end, std::multimap<std::string, std::string>& store);

    std::string get_url_encoded_data(const std::multimap<std::string, std::string>& store);

    // The names of the query parameters whose values are hidden from a log line. A name is
    // compared whole, without case, with the percent-decoded name of a parameter; there are no
    // wildcards, and empty names are ignored. A set replaces the defaults: start from defaults()
    // to add or remove a name, and from {} to redact no parameter at all. Copies share the names,
    // so copying one costs a reference count
    class redacted_parameters {
    public:
        redacted_parameters() = default;
        redacted_parameters(std::initializer_list<std::string_view> names);

        // access_token, api_key, apikey, authorization, client_secret, code, id_token, key,
        // password, refresh_token, secret, sig, signature, token, x-amz-security-token,
        // x-amz-signature, x-goog-signature
        static const redacted_parameters& defaults();

        redacted_parameters& add(std::string_view name);
        redacted_parameters& remove(std::string_view name);

        bool contains(std::string_view name) const;
        size_t size() const { return names_ ? names_->size() : 0; }

    private:
        std::shared_ptr<const std::vector<std::string>> names_;
    };

    // `uri` (a request target or a URL) for a log line: the values of the query parameters in
    // `names` and the password of the user info, always, are replaced by "[redacted]"; the rest
    // is kept as is
    std::string loggable_uri(std::string_view uri, const redacted_parameters& names);

    // loggable_uri() with redacted_parameters::defaults()
    std::string loggable_uri(std::string_view uri);

}

#endif

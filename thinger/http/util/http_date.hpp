#ifndef THINGER_HTTP_UTIL_HTTP_DATE_HPP
#define THINGER_HTTP_UTIL_HTTP_DATE_HPP

#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace thinger::http::util {

    // HTTP-date (RFC 9110, 5.6.7) of a time in seconds since the epoch, as IMF-fixdate:
    // "Sun, 06 Nov 1994 08:49:37 GMT"
    std::string format_http_date(std::time_t time);

    // Time of an HTTP-date: IMF-fixdate, or the obsolete RFC 850 ("Sunday, 06-Nov-94
    // 08:49:37 GMT") and asctime ("Sun Nov  6 08:49:37 1994") formats; nullopt if invalid
    std::optional<std::time_t> parse_http_date(std::string_view date);

}

#endif

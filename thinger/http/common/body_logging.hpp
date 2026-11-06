#ifndef THINGER_HTTP_BODY_LOGGING_HPP
#define THINGER_HTTP_BODY_LOGGING_HPP

#include <cstddef>
#include "../util/url.hpp"

namespace thinger::http {

    // What the logs of the HTTP messages sent and received show. By default they show the
    // request line or the status (info), the header names (debug) and the body size (trace),
    // but never a header value or the body, as they may hold credentials (tokens, cookies,
    // passwords, private keys...). Enabled, an opt-in for debugging, they also show the
    // header values (debug) and the start of the body, up to `max_length` bytes (trace).
    // The values of Authorization, Proxy-Authorization, Cookie and Set-Cookie are always
    // redacted, and WebSocket messages are never logged, only their size.
    struct body_logging {
        static constexpr size_t default_max_length = 1024;

        bool enabled = false;
        size_t max_length = default_max_length;
        // The query parameters hidden from the URIs logged, whether `enabled` or not: the
        // request line and, if `enabled`, the values of Location, Content-Location and Referer.
        // Assigning the two fields above with {true, 512} sets this back to the defaults
        util::url::redacted_parameters redact = util::url::redacted_parameters::defaults();
    };

}

#endif

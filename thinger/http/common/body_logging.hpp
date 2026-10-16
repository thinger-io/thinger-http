#ifndef THINGER_HTTP_BODY_LOGGING_HPP
#define THINGER_HTTP_BODY_LOGGING_HPP

#include <cstddef>

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
    };

}

#endif

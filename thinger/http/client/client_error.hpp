#ifndef THINGER_HTTP_CLIENT_ERROR_HPP
#define THINGER_HTTP_CLIENT_ERROR_HPP

#include <type_traits>
#include <boost/system/error_code.hpp>

namespace thinger::http {

    /// Why a response could not be read, or a request not sent (client_response::error_code(),
    /// stream_result::error). The connection is never reused after any of them.
    enum class client_error {
        none = 0,
        invalid_status_line,        // not "HTTP/1.DIGIT SP 3DIGIT [SP reason] CRLF", or a status out of 100-599
        invalid_header,             // a malformed header line: bare CR or LF, obs-fold, invalid name or value
        header_section_too_large,   // too many header lines, or too many bytes
        invalid_framing,            // invalid Content-Length, Transfer-Encoding other than chunked, or both
        malformed_body,             // invalid chunked framing
        body_too_large,             // over the maximum content size of a buffered response
        incomplete_response,        // the connection ended before the end of the response
        unexpected_upgrade,         // 101 Switching Protocols for a request without Upgrade
        aborted,                    // the streaming callback stopped the download
        invalid_ca_certificate,     // a CA certificate (file or PEM) could not be loaded
        proxy_authentication_required,  // the proxy answered CONNECT with 407 (see client_response::status())
        proxy_tunnel_failed,        // the proxy answered CONNECT with another status than 2xx, or sent data before the origin
        invalid_proxy               // the proxy URL is invalid or not supported (only http:// proxies are)
    };

    const boost::system::error_category& client_error_category();

    inline boost::system::error_code make_error_code(client_error error) {
        return {static_cast<int>(error), client_error_category()};
    }

}

template<>
struct boost::system::is_error_code_enum<thinger::http::client_error> : std::true_type {};

#endif

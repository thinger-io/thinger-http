#include "client_error.hpp"
#include <string>

namespace thinger::http {

    namespace {

        class client_error_category_impl : public boost::system::error_category {
        public:
            const char* name() const noexcept override {
                return "thinger.http.client";
            }

            std::string message(int value) const override {
                switch (static_cast<client_error>(value)) {
                    case client_error::none: return "success";
                    case client_error::invalid_status_line: return "invalid response status line";
                    case client_error::invalid_header: return "invalid response header";
                    case client_error::header_section_too_large: return "response header section too large";
                    case client_error::invalid_framing: return "invalid response framing (Content-Length or Transfer-Encoding)";
                    case client_error::malformed_body: return "malformed chunked response body";
                    case client_error::body_too_large: return "response body too large";
                    case client_error::incomplete_response: return "connection closed before the end of the response";
                    case client_error::unexpected_upgrade: return "unexpected protocol switch (101) without Upgrade in the request";
                    case client_error::aborted: return "download aborted";
                    case client_error::invalid_ca_certificate: return "a CA certificate could not be loaded";
                }
                return "unknown client error";
            }
        };

    }

    const boost::system::error_category& client_error_category() {
        static const client_error_category_impl category;
        return category;
    }

}

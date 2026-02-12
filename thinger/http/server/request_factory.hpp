#ifndef HTTP_REQUEST_PARSER_HPP
#define HTTP_REQUEST_PARSER_HPP

#include <memory>
#include <boost/logic/tribool.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/lexical_cast.hpp>

namespace thinger::http {

    class http_request;

    /// Parser for incoming requests.
    class request_factory {
    public:
        /// Construct ready to parse the http_request method.
        request_factory();

        /// Parse some data. The tribool return value is true when a complete http_request
        /// has been parsed, false if the data is invalid, indeterminate when more
        /// data is required. The InputIterator return value indicates how much of the
        /// input has been consumed.
        template<typename InputIterator>
        boost::tribool parse(InputIterator& begin, InputIterator end) {
            while (begin != end) {
                boost::tribool result = consume(*begin++);
                // parsed completed or parse failed
                if (result || !result)
                    return result;
            }
            // still not finished
            return boost::indeterminate;
        }

        void set_headers_only(bool headers_only) {
            headers_only_ = headers_only;
        }

        bool get_headers_only() const {
            return headers_only_;
        }

        std::shared_ptr<http_request> consume_request();


        void on_http_method(const std::string& method);

        void on_http_status_code(unsigned short status_code);

        void on_http_uri(const std::string& uri);

        void on_http_major_version(uint8_t major);

        void on_http_minor_version(uint16_t minor);

        void on_http_header(const std::string& name, const std::string& value);

        void on_content(char content);

        size_t get_content_length();

        size_t get_content_read();

        bool empty_headers();

    private:
        /// Handle the next character of input.
        boost::tribool consume(char input);

        /// Check if a byte is an HTTP character.
        static bool is_char(int c);

        /// Check if a byte is an HTTP control character.
        static bool is_ctl(int c);

        /// Check if a byte is defined as an HTTP special character.
        static bool is_tspecial(int c);

        /// Check if a byte is a digit.
        static bool is_digit(int c);

        std::shared_ptr<http_request> req;

        std::string tempString1_;
        std::string tempString2_;
        size_t tempInt_;
        bool headers_only_ = false;

        /// The current state of the parser.
        enum state {
            method_start,
            method,
            uri,
            http_version_h,
            http_version_t_1,
            http_version_t_2,
            http_version_p,
            http_version_slash,
            http_version_major_start,
            http_version_major,
            http_version_minor_start,
            http_version_minor,
            expecting_newline_1,
            header_line_start,
            header_lws,
            header_name,
            space_before_header_value,
            header_value,
            expecting_newline_2,
            expecting_newline_3,
            content
        } state_;
    };
}

#endif // HTTP_REQUEST_PARSER_HPP

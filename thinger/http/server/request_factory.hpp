#ifndef HTTP_REQUEST_PARSER_HPP
#define HTTP_REQUEST_PARSER_HPP

#include <memory>
#include <string>
#include <boost/logic/tribool.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/lexical_cast.hpp>

namespace thinger::http {

    class http_request;

    /// Parser for incoming requests.
    class request_factory {
    public:
        /// Limits of the header section (request line and header fields): a request over
        /// them fails to parse, with header_section_too_large() set
        static constexpr size_t max_header_lines = 100;
        static constexpr size_t max_header_section_size = 16 * 1024;

        /// Construct ready to parse the http_request method.
        request_factory();

        /// Parse some data. The tribool return value is true when a complete http_request
        /// has been parsed, false if the data is invalid, indeterminate when more
        /// data is required. The InputIterator return value indicates how much of the
        /// input has been consumed.
        template<typename InputIterator>
        boost::tribool parse(InputIterator& begin, InputIterator end) {
            while (begin != end) {
                // The bytes of the uri, a header name or a header value are appended at once,
                // as consume() would one by one (up to the header section limit: the byte
                // over it is left to consume(), which fails)
                if (std::string* target = run_target()) {
                    size_t room = max_header_section_size - header_section_size_;
                    auto run_end = begin;
                    size_t size = 0;
                    while (run_end != end && size < room && is_run_char(static_cast<char>(*run_end))) {
                        ++run_end;
                        ++size;
                    }
                    if (size > 0) {
                        target->append(begin, run_end);
                        header_section_size_ += size;
                        begin = run_end;
                        continue;
                    }
                }
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

        /// Whether the last parse failed for exceeding the header section limits
        bool header_section_too_large() const {
            return header_section_too_large_;
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

    private:
        /// Handle the next character of input.
        boost::tribool consume(char input);

        /// String a byte that consume() only appends in the current state would go to (the
        /// uri, a header name or a header value), null in the other states
        std::string* run_target() {
            switch (state_) {
                case uri: return &tempString1_;
                case header_name: return &tempString1_;
                case header_value: return &tempString2_;
                default: return nullptr;
            }
        }

        /// Whether consume() only appends `c` in the current state (see run_target)
        bool is_run_char(char c) const {
            switch (state_) {
                case uri: return c != ' ' && !is_ctl(c);
                case header_name: return is_char(c) && !is_ctl(c) && !is_tspecial(c);
                case header_value: return !is_ctl(c);
                default: return false;
            }
        }

        /// Check if a byte is an HTTP character.
        static bool is_char(int c) {
            return c >= 0 && c <= 127;
        }

        /// Check if a byte is an HTTP control character.
        static bool is_ctl(int c) {
            return (c >= 0 && c <= 31) || (c == 127);
        }

        /// Check if a byte is defined as an HTTP special character.
        static bool is_tspecial(int c) {
            switch (c) {
                case '(': case ')': case '<': case '>': case '@': case ',': case ';': case ':':
                case '\\': case '"': case '/': case '[': case ']': case '?': case '=': case '{':
                case '}': case ' ': case '\t':
                    return true;
                default:
                    return false;
            }
        }

        /// Check if a byte is a digit.
        static bool is_digit(int c) {
            return c >= '0' && c <= '9';
        }

        std::shared_ptr<http_request> req;

        std::string tempString1_;
        std::string tempString2_;
        size_t tempInt_;
        bool headers_only_ = false;
        size_t header_lines_ = 0;
        size_t header_section_size_ = 0;
        bool header_section_too_large_ = false;

        /// Fail the parsing for exceeding the header section limits
        bool header_section_exceeded() {
            header_section_too_large_ = true;
            return false;
        }

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

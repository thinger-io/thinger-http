#include <sstream>
#include "request_factory.hpp"
#include "../common/http_request.hpp"
#include "../util/url.hpp"
#include "../../util/logger.hpp"

namespace thinger::http {

    request_factory::request_factory() : state_(method_start) {
    }

    boost::tribool request_factory::consume(char input) {
        // bound the request line and headers, as they are buffered whole
        if (state_ != content && ++header_section_size_ > max_header_section_size) {
            return header_section_exceeded();
        }
        switch (state_) {
            case method_start:
                if (!is_char(input) || is_ctl(input) || is_tspecial(input)) {
                    return false;
                }
                else {
                    state_ = method;
                    tempString1_.push_back(input);
                    return boost::indeterminate;
                }
            case method:
                if (input == ' ') {
                    // initialize a new http request if necessary
                    if(!req){
                        req = std::make_shared<http_request>();
                        // room for the headers of most requests, without growing
                        req->get_headers().reserve(16);
                    }
                    // store read method
                    on_http_method(tempString1_);
                    tempString1_.clear();
                    state_ = uri;
                    return boost::indeterminate;
                }
                else if (!is_char(input) || is_ctl(input) || is_tspecial(input)) {
                    return false;
                }
                else {
                    tempString1_.push_back(input);
                    return boost::indeterminate;
                }
            case uri:
                if (input == ' ') {
                    // decode get uri
                    std::string parsed_uri;
                    bool parsed_ok = util::url::url_decode(tempString1_, parsed_uri);
                    if (parsed_ok) {
                        // http_request path must be absolute and not contain "..".
                        if (parsed_uri.empty() || parsed_uri[0] != '/' || parsed_uri.find("..") != std::string::npos) {
                            return false;
                        }
                        on_http_uri(tempString1_);
                        tempString1_.clear();
                        // next step reading http version
                        state_ = http_version_h;
                        return boost::indeterminate;
                    } else {
                        tempString1_.clear();
                        return false;
                    }
                }
                else if (is_ctl(input)) {
                    return false;
                }
                else {
                    tempString1_.push_back(input);
                    return boost::indeterminate;
                }
            case http_version_h:
                if (input == 'H') {
                    state_ = http_version_t_1;
                    return boost::indeterminate;
                }
                return false;
            case http_version_t_1:
                if (input == 'T') {
                    state_ = http_version_t_2;
                    return boost::indeterminate;
                }
                return false;
            case http_version_t_2:
                if (input == 'T') {
                    state_ = http_version_p;
                    return boost::indeterminate;
                }
                return false;
            case http_version_p:
                if (input == 'P') {
                    state_ = http_version_slash;
                    return boost::indeterminate;
                }
                return false;
            case http_version_slash:
                if (input == '/') {
                    state_ = http_version_major_start;
                    return boost::indeterminate;
                }
                return false;
            // HTTP-version = HTTP-name "/" DIGIT "." DIGIT (RFC 9112, section 2.3): a single
            // digit each, as longer numbers would be truncated (e.g. "HTTP/1.10" as 1.1). Only
            // major version 1 uses this message syntax; a higher minor version is handled as
            // 1.1 (RFC 9110, section 2.5)
            case http_version_major_start:
                if (input == '1') {
                    tempInt_ = input - '0';
                    state_ = http_version_major;
                    return boost::indeterminate;
                }
                return false;
            case http_version_major:
                if (input == '.') {
                    on_http_major_version(tempInt_);
                    state_ = http_version_minor_start;
                    return boost::indeterminate;
                }
                return false;
            case http_version_minor_start:
                if (is_digit(input)) {
                    tempInt_ = input - '0';
                    state_ = http_version_minor;
                    return boost::indeterminate;
                }
                return false;
            case http_version_minor:
                if (input == '\r') {
                    on_http_minor_version(tempInt_);
                    tempInt_ = -1; // reserve temp int for storing content-lenght value
                    state_ = expecting_newline_1;
                    return boost::indeterminate;
                }
                return false;
            case expecting_newline_1:
                if (input == '\n') {
                    state_ = header_line_start;
                    return boost::indeterminate;
                }
                return false;
            case header_line_start:
                if (input == '\r') {
                    state_ = expecting_newline_3;
                    return boost::indeterminate;
                }
                // A line starting with whitespace (obsolete line folding, RFC 9112 section 5.2)
                // is rejected, as is any other byte that cannot start a header name
                else if (!is_char(input) || is_ctl(input) || is_tspecial(input)) {
                    return false;
                }
                else {
                    if (++header_lines_ > max_header_lines) {
                        return header_section_exceeded();
                    }
                    tempString1_.clear();
                    tempString1_.push_back(input);
                    state_ = header_name;
                    return boost::indeterminate;
                }
            case header_name:
                if (input == ':') {
                    state_ = space_before_header_value;
                    return boost::indeterminate;
                }
                else if (!is_char(input) || is_ctl(input) || is_tspecial(input)) {
                    return false;
                }
                else {
                    tempString1_.push_back(input);
                    return boost::indeterminate;
                }
            case space_before_header_value:
                if (input == ' ') {
                    tempString2_.clear();
                    state_ = header_value;
                    return boost::indeterminate;
                }
                return false;
            case header_value:
                if (input == '\r') {
                    on_http_header(tempString1_, tempString2_);
                    state_ = expecting_newline_2;
                    return boost::indeterminate;
                }
                else if (is_ctl(input)) {
                    return false;
                }
                else {
                    tempString2_.push_back(input);
                    return boost::indeterminate;
                }
            case expecting_newline_2:
                if (input == '\n') {
                    state_ = header_line_start;
                    return boost::indeterminate;
                }
                return false;
            case expecting_newline_3:
                if (input == '\n'){
                    if(headers_only_ || get_content_length() == 0){
                        return true;
                    }
                    state_ = content;
                    return boost::indeterminate;
                }
                return false;
            case content:
                on_content(input);
                if(get_content_read()<get_content_length()){
                    return boost::indeterminate;
                }
                return true;
            default:
                return false;
        }
    }

    bool request_factory::is_char(int c) {
        return c >= 0 && c <= 127;
    }

    bool request_factory::is_ctl(int c) {
        return (c >= 0 && c <= 31) || (c == 127);
    }

    bool request_factory::is_tspecial(int c) {
        switch (c) {
            case '(':
            case ')':
            case '<':
            case '>':
            case '@':
            case ',':
            case ';':
            case ':':
            case '\\':
            case '"':
            case '/':
            case '[':
            case ']':
            case '?':
            case '=':
            case '{':
            case '}':
            case ' ':
            case '\t':
                return true;
            default:
                return false;
        }
    }

    bool request_factory::is_digit(int c) {
        return c >= '0' && c <= '9';
    }

    std::shared_ptr<http_request> request_factory::consume_request() {
        std::shared_ptr<http_request> request(req);
        req.reset();
        state_ =  method_start;
        tempString1_.clear();
        tempString2_.clear();
        header_lines_ = 0;
        header_section_size_ = 0;
        header_section_too_large_ = false;
        return request;
    }

    void request_factory::on_http_method(const std::string& method){
        req->set_method(method);
    }

    void request_factory::on_http_status_code(unsigned short status_code){

    }

    void request_factory::on_http_uri(const std::string& uri){
        req->set_uri(uri);
    }

    void request_factory::on_http_major_version(uint8_t major)
    {
        req->set_http_version_major(major);
    }

    void request_factory::on_http_minor_version(uint16_t minor){
        req->set_http_version_minor(minor);
    }

    void request_factory::on_http_header(const std::string& name, const std::string& value){
        req->process_header(name, value);
    }

    void request_factory::on_content(char content){
        req->get_body().push_back(content);
    }

    size_t request_factory::get_content_length(){
        return req->get_content_length();
    }

    size_t request_factory::get_content_read(){
        return req->get_body().size();
    }
}

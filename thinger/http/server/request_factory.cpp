#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>
#include "request_factory.hpp"
#include "../common/http_request.hpp"
#include "../util/url.hpp"
#include "../../util/ascii.hpp"
#include "../../util/logger.hpp"

namespace thinger::http {

    namespace {

        bool is_percent_encoded(std::string_view text) {
            for (auto percent = text.find('%'); percent != std::string_view::npos; percent = text.find('%', percent + 3)) {
                if (percent + 2 >= text.size() || !std::isxdigit(static_cast<unsigned char>(text[percent + 1])) ||
                    !std::isxdigit(static_cast<unsigned char>(text[percent + 2]))) {
                    return false;
                }
            }
            return true;
        }

        bool has_dot_dot_segment(std::string_view path) {
            for (size_t start = 0; start < path.size();) {
                auto end = std::min(path.find('/', start), path.size());
                if (end - start == 2 && path[start] == '.' && path[start + 1] == '.') return true;
                start = end + 1;
            }
            return false;
        }

        // A target in origin form is accepted when it starts with "/" as sent (RFC 9112 section
        // 3.2.1: an encoded "%2F" is not one), its percent-encoding is valid, and no segment of
        // its decoded path is "..". The path ends at the first "?" as sent (an encoded "%3F" is
        // part of it) and the query is not checked for "..", as "?range=1..5" or
        // "/v1..v2.txt" are no traversal
        bool is_accepted_target(std::string_view target) {
            if (target.empty() || target[0] != '/') return false;
            auto query = target.find('?');
            if (query != std::string_view::npos && !is_percent_encoded(target.substr(query + 1))) return false;
            auto path = target.substr(0, query);
            // without escapes, the path is checked as sent
            if (path.find('%') == std::string_view::npos) return !has_dot_dot_segment(path);
            std::string decoded;
            return util::url::detail::path_decode(path, decoded) && !has_dot_dot_segment(decoded);
        }

    }

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
                    // CONNECT asks for a tunnel to the authority in its target (authority-form,
                    // RFC 9112 section 3.2.3), which is not served. What follows its header
                    // section is tunnel data, not content nor another request (RFC 9110 section
                    // 9.3.6): reading it as HTTP would frame the stream unlike a proxy does
                    if (req->get_method() == method::CONNECT) {
                        return false;
                    }
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
                    if (!is_accepted_target(tempString1_)) {
                        tempString1_.clear();
                        return false;
                    }
                    on_http_uri(tempString1_);
                    tempString1_.clear();
                    // next step reading http version
                    state_ = http_version_h;
                    return boost::indeterminate;
                }
                // The request-target is ASCII (RFC 9112 section 3.2, RFC 3986): other bytes must
                // be percent-encoded. An invalid request line is rejected, not autocorrected, as
                // it may be crafted to bypass filters along the request chain (RFC 9112 section 3)
                else if (!is_char(input) || is_ctl(input)) {
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
            // field-line = field-name ":" OWS field-value OWS (RFC 9112 section 5): no whitespace
            // between the name and the colon (a header name is a token, so it fails here)
            case header_name:
                if (input == ':') {
                    tempString2_.clear();
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
            // Any SP and HTAB before the value are skipped, as Node, Go and Beast do; the value
            // may be empty
            case space_before_header_value:
                if (is_ows(input)) {
                    return boost::indeterminate;
                }
                state_ = header_value;
                [[fallthrough]];
            // Trailing SP and HTAB are not part of the value (RFC 9110 section 5.5): trimmed
            // before the field is stored, so framing fields are validated without them
            case header_value:
                if (input == '\r') {
                    tempString2_.erase(tempString2_.find_last_not_of(" \t") + 1);
                    // A single Host field (RFC 9110 section 7.2), whatever the values: two may
                    // route differently along a proxy chain (Go rejects them too; Node keeps the
                    // first)
                    if (::thinger::util::ascii::iequals(tempString1_, header::host)) {
                        if (host_field_) return false;
                        host_field_ = true;
                    }
                    on_http_header(tempString1_, tempString2_);
                    state_ = expecting_newline_2;
                    return boost::indeterminate;
                }
                else if (is_ctl(input) && input != '\t') {
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
                    // An HTTP/1.1 request carries a Host field, maybe empty (RFC 9110 section
                    // 7.2), as Node and Go require; an HTTP/1.0 one may omit it
                    if (!host_field_ && req->get_http_version_minor() >= 1) {
                        return false;
                    }
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

    std::shared_ptr<http_request> request_factory::consume_request() {
        std::shared_ptr<http_request> request(req);
        req.reset();
        state_ =  method_start;
        tempString1_.clear();
        tempString2_.clear();
        header_lines_ = 0;
        header_section_size_ = 0;
        header_section_too_large_ = false;
        host_field_ = false;
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

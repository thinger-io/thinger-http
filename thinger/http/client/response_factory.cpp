#include "response_factory.hpp"
#include "../common/http_response.hpp"
#include "../common/syntax.hpp"
#include "../../util/logger.hpp"
#include <algorithm>
#include <cstring>
#include <utility>

namespace thinger::http {

    namespace {

        // Start of every status line: HTTP-version = "HTTP/1." DIGIT, then SP (RFC 9112,
        // section 2.3). Only major version 1 uses this message syntax; a higher minor version
        // is handled as 1.1 (RFC 9110, section 2.5)
        constexpr std::string_view version_prefix = "HTTP/1.";
        constexpr size_t status_prefix_size = 9;    // "HTTP/1.1 "

        bool is_digit(char c) {
            return c >= '0' && c <= '9';
        }

        // Whether the byte at `position` of a status line can be there, as far as the
        // version and the space after it are concerned
        bool valid_status_prefix_char(size_t position, char c) {
            if (position < version_prefix.size()) return c == version_prefix[position];
            if (position == version_prefix.size()) return is_digit(c);
            return position != status_prefix_size - 1 || c == ' ';
        }

        // Body bytes reserved at once for a Content-Length body (it grows past it as read)
        constexpr size_t max_reserve = 1024 * 1024;

    }

    boost::tribool response_factory::parse_buffer(const char* data, size_t size, size_t& consumed) {
        consumed = 0;
        while (true) {
            if (state_ == state::done) return true;
            if (state_ == state::error) return false;
            if (consumed == size) return boost::indeterminate;

            const char* begin = data + consumed;
            size_t available = size - consumed;
            switch (state_) {
                case state::status_line:
                case state::header_lines:
                    consumed += read_head(begin, available);
                    break;
                case state::body_length:
                    consumed += read_length(begin, available);
                    break;
                case state::body_chunked:
                    consumed += read_chunked(begin, available);
                    break;
                case state::body_until_close:
                    if (add_body(begin, available)) consumed += available;
                    break;
                default:
                    break;
            }
        }
    }

    size_t response_factory::read_head(const char* data, size_t size) {
        // Up to the end of the line (LF), if it is there
        auto newline = static_cast<const char*>(std::memchr(data, '\n', size));
        size_t bytes = newline ? static_cast<size_t>(newline - data) + 1 : size;

        // Not a response from the first bytes (e.g. another protocol): fail right away
        // instead of waiting for the end of a line that may never come
        if (state_ == state::status_line) {
            for (size_t position = line_.size(); position < status_prefix_size && position - line_.size() < bytes; ++position) {
                if (!valid_status_prefix_char(position, data[position - line_.size()])) {
                    fail(client_error::invalid_status_line);
                    return 0;
                }
            }
        }

        // The head is buffered whole: bound it
        if (bytes > max_header_section_size - header_section_size_) {
            fail(client_error::header_section_too_large);
            return 0;
        }
        header_section_size_ += bytes;

        if (!newline) {
            line_.append(data, bytes);
            return bytes;
        }
        if (line_.empty()) {
            process_line({data, bytes});
        } else {
            line_.append(data, bytes);
            process_line(line_);
        }
        line_.clear();
        return bytes;
    }

    void response_factory::process_line(std::string_view line) {
        // Every line ends with CRLF: a bare LF is not a line terminator (and a bare CR, a
        // control character, is never valid within a line)
        if (line.size() < 2 || line[line.size() - 2] != '\r') {
            fail(state_ == state::status_line ? client_error::invalid_status_line : client_error::invalid_header);
            return;
        }
        line.remove_suffix(2);

        if (state_ == state::status_line) {
            process_status_line(line);
        } else if (line.empty()) {
            process_head();
        } else {
            process_header_line(line);
        }
    }

    void response_factory::process_status_line(std::string_view line) {
        // status-line = HTTP-version SP status-code SP [ reason-phrase ] (RFC 9112, section
        // 4), with a three-digit status code from 100 to 599 (RFC 9110, section 15). As in
        // llhttp, the space after the status code may be missing when there is no reason
        if (line.size() < status_prefix_size + 3 || !line.starts_with(version_prefix) || !is_digit(line[7]) || line[8] != ' ' ||
            !is_digit(line[9]) || !is_digit(line[10]) || !is_digit(line[11])) {
            fail(client_error::invalid_status_line);
            return;
        }
        int status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
        std::string_view reason;
        if (line.size() > 12) {
            if (line[12] != ' ') {
                fail(client_error::invalid_status_line);
                return;
            }
            reason = line.substr(13);
        }
        if (status < 100 || status > 599 || !syntax::is_field_value(reason)) {
            fail(client_error::invalid_status_line);
            return;
        }

        resp_ = std::make_shared<http_response>();
        resp_->set_http_version_major(1);
        resp_->set_http_version_minor(static_cast<uint8_t>(line[7] - '0'));
        resp_->set_status(static_cast<uint16_t>(status));
        resp_->set_reason_phrase(std::string(reason));
        state_ = state::header_lines;
    }

    void response_factory::process_header_line(std::string_view line) {
        // field-line = field-name ":" OWS field-value OWS (RFC 9112, section 5). A line
        // starting with whitespace (obsolete line folding, section 5.2) has no valid name
        auto colon = line.find(':');
        if (colon == std::string_view::npos || !syntax::is_token(line.substr(0, colon))) {
            fail(client_error::invalid_header);
            return;
        }
        auto value = syntax::trim(line.substr(colon + 1));
        if (!syntax::is_field_value(value)) {
            fail(client_error::invalid_header);
            return;
        }
        if (++header_lines_ > max_header_lines) {
            fail(client_error::header_section_too_large);
            return;
        }
        resp_->process_header(std::string(line.substr(0, colon)), std::string(value));
    }

    void response_factory::process_head() {
        // Ambiguous framing fails whatever the status: the server is not doing what it says
        if (!resp_->has_valid_framing()) {
            fail(client_error::invalid_framing);
            return;
        }

        int status = resp_->get_status_code();
        if (status < 200 && status != 101 && !headers_only_) {
            // 100 Continue, 103 Early Hints...: the final response follows
            start_response();
            return;
        }

        framing_ = body_framing_of(status);
        keep_alive_ = !switches_protocols(status) && framing_ != body_framing::until_close && resp_->keep_alive();
        if (headers_only_) {
            state_ = state::done;
        } else {
            start_body();
        }
    }

    bool response_factory::switches_protocols(int status) const {
        return status == 101 || (request_method_ == method::CONNECT && status >= 200 && status < 300);
    }

    // RFC 9112, section 6.3
    response_factory::body_framing response_factory::body_framing_of(int status) const {
        if (status < 200 || switches_protocols(status) || request_method_ == method::HEAD || status == 204 || status == 304) {
            return body_framing::none;
        }
        if (resp_->is_chunked_transfer()) return body_framing::chunked;
        if (resp_->has_content_length_header()) return body_framing::content_length;
        return body_framing::until_close;
    }

    void response_factory::start_body() {
        switch (framing_) {
            case body_framing::none:
                state_ = state::done;
                break;
            case body_framing::content_length:
                remaining_ = resp_->get_content_length();
                if (remaining_ == 0) {
                    state_ = state::done;
                } else if (!on_streaming_ && remaining_ > max_content_size_) {
                    LOG_ERROR("the response size exceeds the maximum allowed size: {} ({} max)", remaining_, max_content_size_);
                    fail(client_error::body_too_large);
                } else {
                    if (!on_streaming_) resp_->get_content().reserve(std::min(remaining_, max_reserve));
                    state_ = state::body_length;
                }
                break;
            case body_framing::chunked:
                decoder_ = {};
                if (on_chunked_ && !on_streaming_) on_chunked_(0, std::string());
                state_ = state::body_chunked;
                break;
            case body_framing::until_close:
                state_ = state::body_until_close;
                break;
        }
    }

    size_t response_factory::read_length(const char* data, size_t size) {
        size_t bytes = std::min(size, remaining_);
        if (!add_body(data, bytes)) return 0;
        remaining_ -= bytes;
        if (remaining_ == 0) state_ = state::done;
        return bytes;
    }

    size_t response_factory::read_chunked(const char* data, size_t size) {
        // Chunk data is taken straight from the input, the framing goes through the decoder
        if (size_t pending = decoder_.data_pending()) {
            size_t bytes = std::min(size, pending);
            if (!add_body(data, bytes)) return 0;
            decoder_.data_consumed(bytes);
            if (on_chunked_ && !on_streaming_ && decoder_.data_pending() == 0) {
                on_chunked_(1, resp_->get_content());
                resp_->get_content().clear();
            }
            return bytes;
        }

        size_t produced = 0;
        size_t bytes = decoder_.decode(reinterpret_cast<const uint8_t*>(data), size, nullptr, 0, produced);
        if (decoder_.failed()) {
            fail(client_error::malformed_body);
        } else if (decoder_.done()) {
            if (on_chunked_ && !on_streaming_) on_chunked_(2, std::string());
            state_ = state::done;
        }
        return bytes;
    }

    bool response_factory::add_body(const char* data, size_t size) {
        if (on_streaming_) {
            streaming_downloaded_ += size;
            size_t total = framing_ == body_framing::content_length ? resp_->get_content_length() : 0;
            if (!on_streaming_(std::string_view(data, size), streaming_downloaded_, total)) {
                fail(client_error::aborted);
                return false;
            }
            return true;
        }
        auto& content = resp_->get_content();
        if (size > max_content_size_ - std::min(content.size(), max_content_size_)) {
            LOG_ERROR("the response size exceeds the maximum allowed size: {} max", max_content_size_);
            fail(client_error::body_too_large);
            return false;
        }
        content.append(data, size);
        return true;
    }

    bool response_factory::end_of_stream() {
        if (state_ == state::body_until_close) state_ = state::done;
        if (state_ == state::done) return true;
        if (state_ != state::error) fail(client_error::incomplete_response);
        return false;
    }

    bool response_factory::keep_alive() const {
        return state_ == state::done && keep_alive_;
    }

    void response_factory::fail(client_error error) {
        error_ = error;
        state_ = state::error;
    }

    void response_factory::start_response() {
        resp_.reset();
        state_ = state::status_line;
        framing_ = body_framing::none;
        keep_alive_ = false;
        line_.clear();
        header_section_size_ = 0;
        header_lines_ = 0;
        remaining_ = 0;
    }

    std::shared_ptr<http_response> response_factory::consume_response() {
        auto response = std::exchange(resp_, nullptr);
        if (response) response->log("HTTP CLIENT RESPONSE", 0);
        return response;
    }

    void response_factory::reset() {
        start_response();
        request_method_ = method::GET;
        error_ = client_error::none;
        on_chunked_ = nullptr;
        on_streaming_ = nullptr;
        streaming_downloaded_ = 0;
    }

    int response_factory::get_status_code() const {
        return resp_ ? resp_->get_status_code() : 0;
    }

    void response_factory::setOnChunked(const std::function<void(int, const std::string&)>& onChunked) {
        on_chunked_ = onChunked;
    }

    void response_factory::setOnStreaming(std::function<bool(const std::string_view&, size_t, size_t)> callback) {
        on_streaming_ = std::move(callback);
        streaming_downloaded_ = 0;
    }

}

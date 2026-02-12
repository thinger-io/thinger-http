#ifndef THINGER_HTTP_RESPONSE_FACTORY_HPP
#define THINGER_HTTP_RESPONSE_FACTORY_HPP

#include <memory>
#include <functional>
#include <string_view>
#include <boost/logic/tribool.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/lexical_cast.hpp>

namespace thinger::http {

    class http_response;

    /// Parser for incoming requests.
    class response_factory {
    public:
        /// Specify the maximum response size to be read
        const size_t MAX_CONTENT_SIZE = 8*1048576;  // 1MB
        const size_t MAX_HEADERS_SIZE = 8*1024;     // 8KB

        /// Construct ready to parse the http_request method.
        response_factory() = default;

        /// Parse some data. The tribool return value is true when a complete http_request
        /// has been parsed, false if the data is invalid, indeterminate when more
        /// data is required. The InputIterator return value indicates how much of the
        /// input has been consumed.
        template<typename InputIterator>
        boost::tribool parse(InputIterator begin, InputIterator end, bool head_request = false) {
            // iterate over all input chars
            while (begin != end) {
                // Optimization: batch process content in streaming mode
                if (on_streaming_ && state_ == lenght_delimited_content && tempInt_ > 0) {
                    size_t available = static_cast<size_t>(std::distance(begin, end));
                    size_t to_read = std::min(available, tempInt_);

                    // Call streaming callback with batch data
                    // Need to cast to char* since buffer may be uint8_t*
                    std::string_view data(reinterpret_cast<const char*>(&*begin), to_read);
                    streaming_downloaded_ += to_read;

                    if (!on_streaming_(data, streaming_downloaded_, get_content_length())) {
                        streaming_aborted_ = true;
                        return false;
                    }

                    std::advance(begin, to_read);
                    tempInt_ -= to_read;

                    if (tempInt_ == 0) {
                        return true;  // Done reading content
                    }
                    continue;
                }

                // consume character
                boost::tribool result = consume(*begin++, head_request);

                // parse completed/failed
                if (result || !result){
                    return result;
                }
            }
            // still not finished
            return boost::indeterminate;
        }

        std::shared_ptr<http_response> consume_response();
        void reset();

        void on_http_major_version(uint8_t major);

        void on_http_minor_version(uint16_t minor);

        void on_http_status_code(unsigned short status_code);

        void on_http_reason_phrase(const std::string& reason);

        void on_http_header(const std::string& name, const std::string& value);

        void on_content_data(char content);

        bool on_chunk_read(size_t size);

        bool on_length_delimited_content(size_t size);

        size_t get_content_length();

        size_t get_content_read();

        bool empty_headers();

        /**
         * Get the HTTP status code (available after headers are parsed).
         */
        int get_status_code() const;

        /**
         * Get the response object without consuming it.
         * Note: The response is still being built, so don't modify it.
         */
        std::shared_ptr<http_response> get_response() const { return resp; }

        void setOnChunked(const std::function<void(int, const std::string&)>& onChunked);

        /**
         * Set streaming callback for both chunked and length-delimited responses.
         * @param callback Function called with (data, downloaded_so_far, total_size).
         *                 total_size is 0 for chunked responses.
         *                 Return false to abort download.
         */
        void setOnStreaming(std::function<bool(const std::string_view&, size_t, size_t)> callback);

        /**
         * Check if streaming mode is enabled.
         */
        bool is_streaming() const { return on_streaming_ != nullptr; }

    private:
        /// Handle the next character of input.
        boost::tribool consume(char input, bool head_request=false);

        /// Check if a byte is an HTTP character.
        static bool is_char(char c);

        /// Check if a byte is an HTTP control character.
        static bool is_ctl(char c);

        /// Check if a byte is defined as an HTTP special character.
        static bool is_tspecial(char c);

        /// Check if a byte is a digit.
        static bool is_digit(char c);

        /// Check if a byte is hexadecimal digit
        bool is_hexadecimal(char c);

        uint8_t get_hex_value(char c);

        std::shared_ptr<http_response> resp;

        std::string tempString1_;
        std::string tempString2_;
        size_t tempInt_         = 0;
        size_t headers_size_    = 0;
        bool lastChunk_         = false;
        std::function<void(int, const std::string&)> on_chunked_;
        std::function<bool(const std::string_view&, size_t, size_t)> on_streaming_;
        size_t streaming_downloaded_ = 0;
        bool streaming_aborted_ = false;

        enum content_type{
            none,
            lenght_delimited,
            chunked
        };

        content_type content_ = content_type::none;

        /// The current state of the parser.
        enum state {
            http_version_h,
            http_version_t_1,
            http_version_t_2,
            http_version_p,
            http_version_slash,
            http_version_major_start,
            http_version_major,
            http_version_minor_start,
            http_version_minor,
            status_code,
            reason_phrase,
            expecting_newline_1,
            header_line_start,
            header_lws,
            header_name,
            space_before_header_value,
            header_value,
            expecting_newline_2,
            expecting_newline_3,
            lenght_delimited_content,
            chunked_content_size,
            chunked_content_size_expecting_n,
            chunked_content,
            chunked_content_expecting_n
        } state_ = http_version_h;
    };

}

#endif

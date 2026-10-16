#ifndef THINGER_HTTP_RESPONSE_FACTORY_HPP
#define THINGER_HTTP_RESPONSE_FACTORY_HPP

#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <boost/logic/tribool.hpp>
#include "client_error.hpp"
#include "../common/chunked_decoder.hpp"
#include "../common/http_request.hpp"

namespace thinger::http {

    class http_response;

    /**
     * Strict parser of HTTP/1.x responses (RFC 9112), fed with what is read from the
     * connection after sending a request. The head is parsed line by line, and the body
     * delimited as RFC 9112 section 6.3 says, from the request method and the status:
     *  - none for responses to HEAD, 1xx, 204 and 304, whatever their headers, nor for 101 or
     *    a 2xx to CONNECT (what follows is another protocol);
     *  - "Transfer-Encoding: chunked": decoded strictly (chunked_decoder), trailers dropped;
     *  - Content-Length;
     *  - neither: the body ends with the connection (end_of_stream()).
     * Interim responses (1xx but 101) are skipped. Anything else fails the parsing, with
     * error() telling why: there is no recovery, as where the response ends, and so where
     * the next one on the connection starts, would be a guess.
     */
    class response_factory {
    public:
        /// Maximum response size when buffered (streaming bypasses it)
        static constexpr size_t DEFAULT_MAX_CONTENT_SIZE = 8 * 1048576;

        /// Limits of the head of each response (status line and header lines), as for requests
        static constexpr size_t max_header_lines = 100;
        static constexpr size_t max_header_section_size = 16 * 1024;

        /// How the body of a response is delimited
        enum class body_framing { none, content_length, chunked, until_close };

        response_factory() = default;

        /// Method of the request the response answers: responses to HEAD and CONNECT have no
        /// body. Set it after reset()
        void set_request_method(method request_method) { request_method_ = request_method; }

        /// Stop after the head of each response, interim ones included: the caller reads the
        /// body (as get_body_framing() says)
        void set_headers_only(bool headers_only) { headers_only_ = headers_only; }

        /// Parse some data. True when the response is complete (its head in headers-only
        /// mode), false on error (see error()), indeterminate when more data is required.
        /// `begin` is left where parsing stopped: past the end of the response when it is
        /// complete, so what follows it is in [begin, end).
        template<typename InputIterator>
        boost::tribool parse_some(InputIterator& begin, std::type_identity_t<InputIterator> end) {
            static_assert(std::contiguous_iterator<InputIterator>, "the input must be contiguous bytes");
            auto size = static_cast<size_t>(end - begin);
            const char* data = size ? reinterpret_cast<const char*>(std::to_address(begin)) : nullptr;
            size_t consumed = 0;
            boost::tribool result = parse_buffer(data, size, consumed);
            begin += static_cast<std::iter_difference_t<InputIterator>>(consumed);
            return result;
        }

        /// Same as parse_some(), when what follows the response does not matter
        template<typename InputIterator>
        boost::tribool parse(InputIterator begin, InputIterator end) {
            return parse_some(begin, end);
        }

        /// The connection ended (the server closed it): true if that completes the response
        /// (a body delimited by the end of the connection), false otherwise
        bool end_of_stream();

        /// Why parsing failed
        client_error error() const { return error_; }

        /// Framing of the body of the response, once its head is parsed
        body_framing get_body_framing() const { return framing_; }

        /// Whether the connection can carry another response after this one (once it is
        /// complete): framed by its length or chunked, kept alive, and not switching protocols
        bool keep_alive() const;

        std::shared_ptr<http_response> consume_response();

        /// Ready for a new response, with no request method nor callbacks
        void reset();

        /// HTTP status code (once the status line is parsed), or 0
        int get_status_code() const;

        /// The response being parsed (do not modify it)
        std::shared_ptr<http_response> get_response() const { return resp_; }

        /// Called with 0 at the start of a chunked body, 1 and the data of each chunk, and 2
        /// at its end. The data is not kept in the response.
        void setOnChunked(const std::function<void(int, const std::string&)>& onChunked);

        /**
         * Set streaming callback for every response body.
         * @param callback Function called with (data, downloaded_so_far, total_size).
         *                 total_size is 0 unless the body has a Content-Length.
         *                 Return false to abort download.
         */
        void setOnStreaming(std::function<bool(const std::string_view&, size_t, size_t)> callback);

        /**
         * Check if streaming mode is enabled.
         */
        bool is_streaming() const { return on_streaming_ != nullptr; }

        /**
         * Set the maximum response size when buffering the body. Responses
         * larger than this are rejected. In streaming mode the limit does
         * not apply because the body is handed off chunk-by-chunk.
         */
        void set_max_content_size(size_t size) { max_content_size_ = size; }
        size_t get_max_content_size() const { return max_content_size_; }

        // How the responses are logged as they are consumed (see body_logging)
        void set_body_logging(const body_logging& options) { body_logging_ = options; }

    private:
        enum class state {
            status_line,
            header_lines,
            body_length,
            body_chunked,
            body_until_close,
            done,
            error
        };

        boost::tribool parse_buffer(const char* data, size_t size, size_t& consumed);

        // Each returns the bytes of `data` it consumed
        size_t read_head(const char* data, size_t size);
        size_t read_length(const char* data, size_t size);
        size_t read_chunked(const char* data, size_t size);

        void process_line(std::string_view line);
        void process_status_line(std::string_view line);
        void process_header_line(std::string_view line);
        void process_head();
        void start_response();
        void start_body();
        bool switches_protocols(int status) const;
        body_framing body_framing_of(int status) const;

        /// Hand body data to the streaming callback, or keep it in the response
        bool add_body(const char* data, size_t size);

        void fail(client_error error);

        std::shared_ptr<http_response> resp_;
        method request_method_ = method::GET;
        bool headers_only_ = false;
        state state_ = state::status_line;
        client_error error_ = client_error::none;
        body_framing framing_ = body_framing::none;
        bool keep_alive_ = false;

        std::string line_;              // line being read
        size_t header_section_size_ = 0;
        size_t header_lines_ = 0;
        size_t remaining_ = 0;          // unread bytes of a Content-Length body
        chunked_decoder decoder_;

        std::function<void(int, const std::string&)> on_chunked_;
        std::function<bool(const std::string_view&, size_t, size_t)> on_streaming_;
        size_t streaming_downloaded_ = 0;
        size_t max_content_size_ = DEFAULT_MAX_CONTENT_SIZE;
        body_logging body_logging_;
    };

}

#endif

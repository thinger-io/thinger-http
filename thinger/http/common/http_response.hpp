#ifndef HTTP_RESPONSE_HPP
#define HTTP_RESPONSE_HPP

#include <string>
#include <vector>
#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/lexical_cast.hpp>
#include "../data/out_data.hpp"
#include "headers.hpp"

namespace thinger::http {

class http_response : public headers {

public:

    // the status of the http_response.
    enum class status {
        ok = 200,
        created = 201,
        accepted = 202,
        no_content = 204,
        partial_content = 206,
        multiple_choices = 300,
        moved_permanently = 301,
        moved_temporarily = 302,
        not_modified = 304,
        temporary_redirect = 307,
        permanent_redirect = 308,
        bad_request = 400,
        unauthorized = 401,
        forbidden = 403,
        not_found = 404,
        not_allowed = 405,
        timed_out = 408,
        conflict = 409,
        payload_too_large = 413,
        unsupported_media_type = 415,
        range_not_satisfiable = 416,
        request_header_fields_too_large = 431,
        upgrade_required = 426,
        too_many_requests = 429,
        internal_server_error = 500,
        not_implemented = 501,
        bad_gateway = 502,
        service_unavailable = 503,
        gateway_timeout = 504,
        switching_protocols = 101
    } ;

    // constructor
    http_response();
    ~http_response() override = default;

    // response to buffer
    void to_buffer(std::vector<boost::asio::const_buffer>&buffer) const override;

    // some setters
    void set_content(std::string content);
    void set_content(std::string content, std::string content_type);
    void set_content_length(size_t content_length);
    void set_content_type(std::string && content_type);

    void set_content_type(const std::string &content_type);

    void set_status(uint16_t status_code);
    void set_status(status status_code);
    // Reason phrase of the status line, for statuses without a standard one (e.g. those
    // received from another server); only printable characters are sent
    void set_reason_phrase(const std::string& reason);

    // some getters (inline: read on every response)
    const std::string& get_content() const { return content_; }
    std::string& get_content() { return content_; }
    size_t get_content_size() const { return content_.size(); }
    size_t get_size() override;
    status get_status() const { return status_; }
    int get_status_code() const { return static_cast<int>(status_); }
    const std::string& get_reason() const { return reason_phrase_; }
    bool is_ok() const;
    bool is_redirect_response() const;

    // log
    void log(const char* scope, int level) const override;

    // factory methods
    static std::shared_ptr<http_response> stock_http_reply(http_response::status status);

    // Standard reason phrase of a status ("Not Found" for 404)
    static std::string get_reason_phrase(http_response::status status);

private:
    std::string content_;
    status status_ = status::ok;
    std::string reason_phrase_;
    // Status line of a status without a standard reason phrase, built when sent
    mutable std::string status_line_;
};

}

#endif

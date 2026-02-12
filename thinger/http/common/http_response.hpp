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
        upgrade_required = 426,
        too_many_requests = 429,
        internal_server_error = 500,
        not_implemented = 501,
        bad_gateway = 502,
        service_unavailable = 503,
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
    void set_reason_phrase(const std::string& reason);

    // some getters
    const std::string& get_content() const;
    std::string& get_content();
    size_t get_content_size() const;
    size_t get_size() override;
    status get_status() const;
    int get_status_code() const;
    bool is_ok() const;
    bool is_redirect_response() const;

    // log
    void log(const char* scope, int level) const override;

    // factory methods
    static std::shared_ptr<http_response> stock_http_reply(http_response::status status);

private:
    std::string content_;
    status status_ = status::ok;
    std::string reason_phrase_;
};

}

#endif

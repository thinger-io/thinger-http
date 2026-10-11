#ifndef HTTP_REQUEST_HPP
#define HTTP_REQUEST_HPP

#include <boost/asio.hpp>
#include <regex>
#include <utility>
#include "headers.hpp"
#include "../client/cookie_store.hpp"
#include "../client/proxy.hpp"
#include "../util/url.hpp"

namespace thinger::http {

    enum class method{
        GET,
        HEAD,
        POST,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATCH,
        UNKNOWN
    };

    method get_method(const std::string& method);
    const std::string& get_method(method);
    static const std::string https = "https";
    static const std::string wss = "wss";
    static const std::string ws = "ws";
    static const std::string https_port = "443";
    static const std::string http = "http";
    static const std::string http_port = "80";

class http_request : public headers{

public:

    // constructor
    http_request() = default;
    ~http_request() override = default;

    // setters
    bool set_url(const std::string& url);
    void set_host(std::string host);
    void set_port(const std::string &port);
    void set_protocol(std::string protocol);
    void set_ssl(bool ssl);
    void set_unix_socket(const std::string& unix_socket);
    void set_uri(const std::string &uri);
    void set_method(const std::string &method);
    void set_method(http::method method);
    void set_content(std::string content);
    void set_content(std::string content, std::string content_type);
    void add_uri_parameter(const std::string &key, const std::string &value);
    void set_resource(const std::string& resource);

    // getters
    std::string get_url() const;
    std::string get_base_path() const;
    const std::string& get_body() const;
    bool has_resource() const;
    bool has_query_parameters() const;
    bool has_uri_parameter(const std::string& key) const;
    bool has_uri_value(const std::string& key, const std::string& value) const;
    bool has_uri_parameters() const;
    const std::string& get_uri_parameter(const std::string& key) const;
    const std::multimap<std::string, std::string>& get_uri_parameters() const;
    bool has_content() const;
    // (inline: checked on every request and response)
    method get_method() const { return method_; }
    const std::string& get_method_string() const;
    std::string get_query_string() const;
    std::string get_path() const;
    std::string& get_uri();
    const std::string& get_uri() const;
    const std::string& get_unix_socket() const;
    const std::string& get_protocol() const;
    bool is_default_port() const;
    const std::string &get_port() const;
    cookie_store& get_cookie_store();
    const std::string &get_host() const;
    bool is_ssl() const;
    const std::string& get_resource() const;
    std::string& get_resource();
    std::string& get_body();

    template<class T>
    T get_uri_parameter(const std::string& key, T default_value) const{
        if(has_uri_parameter(key)){
            const std::string& ts_str = get_uri_parameter(key);
            if(!ts_str.empty()){
                try
                {
                    T value = boost::lexical_cast<T>(ts_str);
                    return value;
                }catch(const boost::bad_lexical_cast &){

                }
            }
        }
        return default_value;
    }

    // chunked callbacks
    std::function<void(int, const std::string&)> get_chunked_callback();
    void set_chunked_callback(std::function<void(int, const std::string&)> callback);

    // client side: proxy settings for this request, instead of those of the client (see
    // http_client_base::proxy()); an empty proxy_config connects directly
    void set_proxy_config(proxy_config config) { proxy_config_ = std::move(config); }
    const std::optional<proxy_config>& get_proxy_config() const { return proxy_config_; }

    // client side: CAs trusted to verify the server of this request, besides those of the
    // client (see http_client_base::add_ca_file()): a PEM file, or PEM text
    void add_ca_file(std::string path) { ca_files_.push_back(std::move(path)); }
    void add_ca_pem(std::string pem) { ca_pems_.push_back(std::move(pem)); }
    const std::vector<std::string>& get_ca_files() const { return ca_files_; }
    const std::vector<std::string>& get_ca_pems() const { return ca_pems_; }

    // stream related
    bool end_stream() override;
    size_t get_size() override;
    void to_buffer(std::vector<boost::asio::const_buffer> &buffer) const override;
    // The request with another request target (e.g. absolute-form, for a proxy), and
    // `extra_headers` (complete lines, CRLF included) after its own headers
    void to_buffer(std::vector<boost::asio::const_buffer> &buffer, std::string_view request_target,
                   std::string_view extra_headers) const;
    void process_header(std::string key, std::string value) override;

    // logs
    void log(const char* scope, int level) const override;

    // factory methods
    static std::shared_ptr<http_request> create_http_request(const std::string& method, const std::string& url);
    static std::shared_ptr<http_request> create_http_request(http::method method, const std::string& url, const std::string& unix_socket="");

    // body status (infrastructure for deferred body reading)
    bool has_pending_body() const {
        return (get_content_length() > 0 && content_.size() < get_content_length())
            || chunked_transfer_;
    }

    size_t pending_body_size() const {
        return get_content_length() > content_.size() ? get_content_length() - content_.size() : 0;
    }

    // other
    void refresh_uri();

private:
    bool ssl_ = false;
    method method_ = method::UNKNOWN;
    std::string uri_;
    std::string resource_;
    std::multimap<std::string, std::string> uri_params_;
    std::string content_;
    std::string host_;
    std::string port_;
    std::string protocol_;
    std::string unix_socket_;
    cookie_store cookie_store_;
    std::function<void(int, const std::string&)> on_chunked_;
    std::optional<proxy_config> proxy_config_;
    std::vector<std::string> ca_files_;
    std::vector<std::string> ca_pems_;

};


}

#endif

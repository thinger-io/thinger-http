#ifndef THINGER_HTTP_CLIENT_RESPONSE_HPP
#define THINGER_HTTP_CLIENT_RESPONSE_HPP

#include "../common/http_response.hpp"
#include <boost/system/error_code.hpp>
#include <memory>
#include <nlohmann/json.hpp>

namespace thinger::http {

class client_response {
private:
    std::shared_ptr<http_response> response_;
    boost::system::error_code error_;
    
public:
    // Constructors
    client_response() = default;
    
    client_response(const boost::system::error_code& ec, 
                   std::shared_ptr<http_response> res)
        : response_(std::move(res)), error_(ec) {}
    
    // Status checks
    bool ok() const { 
        return !error_ && response_ && static_cast<int>(response_->get_status()) >= 200 && 
               static_cast<int>(response_->get_status()) < 300; 
    }
    
    bool success() const { return ok(); }
    
    operator bool() const { return !error_ && response_; }
    
    bool has_error() const { return error_ || !response_; }
    
    bool has_network_error() const { return error_.value() != 0; }
    
    bool has_http_error() const { 
        return !error_ && response_ && static_cast<int>(response_->get_status()) >= 400; 
    }
    
    // Content access
    const std::string& body() const { 
        static const std::string empty;
        return response_ ? response_->get_content() : empty; 
    }
    
    std::string_view text() const { return body(); }
    
    nlohmann::json json() const {
        if (!response_ || response_->get_content().empty()) {
            return nlohmann::json();
        }
        try {
            return nlohmann::json::parse(response_->get_content());
        } catch (...) {
            return nlohmann::json();
        }
    }
    
    // Status info
    int status() const { 
        return response_ ? static_cast<int>(response_->get_status()) : 0; 
    }
    
    int status_code() const { return status(); }
    
    bool is_redirect() const {
        return response_ && response_->is_redirect_response();
    }
    
    bool is_client_error() const {
        return response_ && status() >= 400 && status() < 500;
    }
    
    bool is_server_error() const {
        return response_ && status() >= 500;
    }
    
    // Headers
    std::string header(const std::string& key) const {
        return response_ && response_->has_header(key) ? 
               response_->get_header(key) : "";
    }
    
    bool has_header(const std::string& key) const {
        return response_ && response_->has_header(key);
    }
    
    // Error info
    std::string error() const {
        if (error_) {
            return error_.message();
        } else if (!response_) {
            return "No response received";
        } else if (has_http_error()) {
            return "HTTP error " + std::to_string(status());
        }
        return "";
    }
    
    std::string error_message() const { return error(); }
    
    const boost::system::error_code& error_code() const { return error_; }
    
    // Advanced access
    const http_response* operator->() const { return response_.get(); }
    
    const http_response& operator*() const { 
        if (!response_) throw std::runtime_error("No response available");
        return *response_; 
    }
    
    std::shared_ptr<http_response> get() const { return response_; }
    
    // Content type helpers
    std::string content_type() const {
        return header("Content-Type");
    }
    
    size_t content_length() const {
        if (!response_) return 0;
        return response_->get_content_size();
    }
    
    bool is_json() const {
        auto ct = content_type();
        return ct.find("application/json") != std::string::npos;
    }
    
    bool is_html() const {
        auto ct = content_type();
        return ct.find("text/html") != std::string::npos;
    }
    
    bool is_text() const {
        auto ct = content_type();
        return ct.find("text/") == 0;
    }
};

} // namespace thinger::http

#endif // THINGER_HTTP_CLIENT_RESPONSE_HPP
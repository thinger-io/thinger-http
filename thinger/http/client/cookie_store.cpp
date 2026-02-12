#include "cookie_store.hpp"

namespace thinger::http{

    cookie_store::cookie_store() = default;

    cookie_store::~cookie_store() = default;

    bool cookie_store::update_from_headers(const headers& hdrs){
        bool updated = false;
        std::vector<std::string> cookie_headers = hdrs.get_headers_with_key(header::set_cookie);
        for(const auto& header_value : cookie_headers){
            cookie c = cookie::parse(header_value);
            if(c.is_valid()){
                cookies_[c.get_name()] = c;
                updated = true;
            }
        }
        return updated;
    }

    std::string cookie_store::get_cookie_string() const{
        std::string result;
        for(const auto& [name, c] : cookies_){
            if(!result.empty()){
                result += "; ";
            }
            result += c.get_name();
            result += "=";
            result += c.get_value();
        }
        return result;
    }

    void cookie_store::set_cookie(const cookie& c) {
        if(c.is_valid()){
            cookies_[c.get_name()] = c;
        }
    }

    void cookie_store::set_cookie(const std::string& name, const std::string& value) {
        set_cookie(cookie(name, value));
    }

    std::optional<cookie> cookie_store::get_cookie(const std::string& name) const {
        auto it = cookies_.find(name);
        if(it != cookies_.end()){
            return it->second;
        }
        return std::nullopt;
    }

    bool cookie_store::has_cookie(const std::string& name) const {
        return cookies_.find(name) != cookies_.end();
    }

    void cookie_store::remove_cookie(const std::string& name) {
        cookies_.erase(name);
    }

    void cookie_store::clear() {
        cookies_.clear();
    }

    size_t cookie_store::size() const {
        return cookies_.size();
    }

    bool cookie_store::empty() const {
        return cookies_.empty();
    }

}
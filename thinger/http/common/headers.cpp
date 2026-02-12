#include "headers.hpp"
#include <regex>
#include "../../util/logger.hpp"

namespace thinger::http{

    void headers::process_header(std::string key, std::string value){
        if(boost::indeterminate(keep_alive_) && is_header(key, header::connection)){
            /*
             * Firefox send both keep-alive and upgrade values in Connection header, i.e., when opening a WebSocket,
             * so it is necessary to test values separately.
             */
            std::vector<std::string> strs;
            boost::split(strs, value, boost::is_any_of(","));
            for(auto& str:strs){
                boost::algorithm::trim(str);
                if(is_header(str, connection::keep_alive)){
                    keep_alive_ = true;
                }
                else if (is_header(str, connection::close)){
                    keep_alive_ = false;
                }
                else if(is_header(str, connection::upgrade)){
                    upgrade_ = true;
                }
            }
        }

        if(is_header(key, header::accept)){
            stream_ = boost::iequals(value, accept::event_stream);
        }else if(is_header(key, header::content_length)){
            try{
                content_length_ = boost::lexical_cast<unsigned long>(value);
            }catch(const boost::bad_lexical_cast &)
            {
                content_length_ = 0;
            }
        }

        headers_.emplace_back(std::move(key), std::move(value));
    }

    void headers::add_header(std::string key, std::string value){
        if(key.empty()) return;
        headers_.emplace_back(std::move(key), std::move(value));
    }

    void headers::add_proxy(std::string key, std::string value){
        if(key.empty()) return;
        proxy_headers_.emplace_back(std::move(key), std::move(value));
    }

    void headers::set_header(std::string key, std::string value){
        for(auto & header : headers_)
        {
            if(is_header(header.first, key)){
                header.second = std::move(value);
                return;
            }
        }
        add_header(std::move(key), std::move(value));
    }

    void headers::set_proxy(std::string key, std::string value){
        for(auto & header : proxy_headers_)
        {
            if(is_header(header.first, key)){
                header.second = std::move(value);
                return;
            }
        }
        add_proxy(std::move(key), std::move(value));
    }

    bool headers::upgrade() const{
        return upgrade_;
    }

    bool headers::stream() const{
        return stream_;
    }

    bool headers::has_header(std::string_view key) const{
        for(const auto & header : headers_)
        {
            if(is_header(header.first, key)){
                return true;
            }
        }
        return false;
    }

    const std::string& headers::get_header(std::string_view key) const
    {
        for(const auto & header : headers_)
        {
            if(is_header(header.first, key)){
                return header.second;
            }
        }
        static std::string emtpy;
        return emtpy;
    }

    std::vector<std::string> headers::get_headers_with_key(std::string_view key) const{
        std::vector<std::string> headers;
        for(const auto & header : headers_)
        {
            if(is_header(header.first, key)){
                headers.push_back(header.second);
            }
        }
        return headers;
    }

    const std::vector<headers::http_header>& headers::get_headers() const{
        return headers_;
    }

    std::vector<headers::http_header>& headers::get_headers(){
        return headers_;
    }

    bool headers::remove_header(std::string_view key)
    {
        for(auto it=headers_.begin(); it!=headers_.end(); ++it){
            if(is_header(it->first, key)){
                headers_.erase(it);
                return true;
            }
        }
        return false;
    }

    const std::string& headers::get_authorization() const
    {
        return get_header(header::authorization);
    }

    const std::string& headers::get_cookie() const
    {
        return get_header(header::cookie);
    }

    const std::string& headers::get_user_agent() const{
        return get_header(header::user_agent);
    }

    const std::string& headers::get_content_type() const
    {
        return get_header(header::content_type);
    }

    bool headers::is_content_type(const std::string& value) const
    {
        return boost::istarts_with(get_header(header::content_type), value);
    }

    bool headers::empty_headers() const{
        return headers_.empty();
    }

    void headers::debug_headers(std::ostream& os) const{
        for(const std::pair<std::string, std::string>& t: headers_){
            os << "\t> " << t.first << ": " << t.second << std::endl;
        }
    }

    void headers::log(const char* scope, int level) const{
        LOG_DEBUG("[{}] Headers:", scope);
        for(const auto& t: headers_){
            LOG_DEBUG("  {}: {}", t.first, t.second);
        }
        for(const auto& t: proxy_headers_){
            LOG_DEBUG("  (PROXY REPLACE) {}: {}", t.first, t.second);
        }
    }

    size_t headers::get_content_length() const{
        return content_length_;
    }

    bool headers::keep_alive() const
    {
        if(boost::indeterminate(keep_alive_)){
            return http_version_major_>=1 && http_version_minor_>=1;
        }else{
            return (bool) keep_alive_;
        }
    }

    void headers::set_keep_alive(bool keep_alive){
	    keep_alive_ = keep_alive;
	    set_header(http::header::connection, (keep_alive ? "Keep-Alive" : "Close"));
    }

    void headers::set_http_version_major(uint8_t http_version_major) {
        http_version_major_ = http_version_major;
    }

    void headers::set_http_version_minor(uint8_t http_version_minor) {
        http_version_minor_ = http_version_minor;
    }

    int headers::get_http_version_major() const {
        return http_version_major_;
    }

    int headers::get_http_version_minor() const {
        return http_version_minor_;
    }

    std::string headers::get_parameter(const std::string& header_value, std::string_view name) {

        auto start = header_value.begin();
		auto end   = header_value.end();

		if(start==end) return "";

		static std::regex cookie_regex("([^;^\\s]+)=\"?([^;^\"]*)\"?");
		std::smatch what;

		while (std::regex_search(start, end, what, cookie_regex))
		{
			std::string key(what[1].first, what[1].second);
			std::string value(what[2].first, what[2].second);

			if(key==name) return value;

			start = what[0].second;
		}

		return "";
	}

    bool inline headers::is_header(std::string_view key, std::string_view header) const{
        return boost::iequals(key, header);
    }
}
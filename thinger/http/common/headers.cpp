#include "headers.hpp"
#include <charconv>
#include <regex>
#include "../../util/ascii.hpp"
#include "../../util/logger.hpp"

namespace thinger::http{

    void headers::process_header(std::string key, std::string value){
        if(boost::indeterminate(keep_alive_) && is_header(key, header::connection)){
            /*
             * Firefox send both keep-alive and upgrade values in Connection header, i.e., when opening a WebSocket,
             * so it is necessary to test values separately.
             */
            static constexpr std::string_view whitespace = " \t\n\v\f\r";
            std::string_view options = value;
            while(true){
                auto comma = options.find(',');
                auto option = options.substr(0, comma);
                auto first = option.find_first_not_of(whitespace);
                option = first == std::string_view::npos ? std::string_view{} :
                         option.substr(first, option.find_last_not_of(whitespace) - first + 1);
                if(is_header(option, connection::keep_alive)){
                    keep_alive_ = true;
                }
                else if (is_header(option, connection::close)){
                    keep_alive_ = false;
                }
                else if(is_header(option, connection::upgrade)){
                    upgrade_ = true;
                }
                if(comma == std::string_view::npos) break;
                options.remove_prefix(comma + 1);
            }
        }

        if(is_header(key, header::accept)){
            stream_ = ::thinger::util::ascii::iequals(value, accept::event_stream);
        }else{
            process_framing_header(key, value);
        }

        on_header_added(key);
        headers_.emplace_back(std::move(key), std::move(value));
    }

    void headers::process_framing_header(std::string_view key, std::string_view value){
        if(is_header(key, header::content_length)){
            // Content-Length = 1*DIGIT: anything else (sign, spaces, lists, overflow) or a
            // repeated header with a different value makes the message framing ambiguous
            size_t length = 0;
            auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), length);
            if (ec != std::errc{} || ptr != value.data() + value.size()) {
                invalid_content_length_ = true;
                length = 0;
            } else if (content_length_header_ && length != content_length_) {
                invalid_content_length_ = true;
            }
            content_length_ = length;
        }else if(is_header(key, header::transfer_encoding)){
            // chunked must be the only coding, in a single header
            if(::thinger::util::ascii::iequals(value, "chunked") && !transfer_encoding_header_){
                chunked_transfer_ = true;
            }else{
                invalid_transfer_encoding_ = true;
            }
        }
    }

    void headers::add_header(std::string key, std::string value){
        if(key.empty()) return;
        on_header_added(key);
        headers_.emplace_back(std::move(key), std::move(value));
    }

    void headers::on_header_added(std::string_view key){
        if(is_header(key, header::content_length)){
            content_length_header_ = true;
        }else if(is_header(key, header::transfer_encoding)){
            transfer_encoding_header_ = true;
        }
    }

    void headers::on_header_removed(std::string_view key){
        if(is_header(key, header::content_length)){
            content_length_header_ = has_header(header::content_length);
        }else if(is_header(key, header::transfer_encoding)){
            transfer_encoding_header_ = has_header(header::transfer_encoding);
        }
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
                on_header_removed(key);
                return true;
            }
        }
        return false;
    }

    size_t headers::remove_headers(std::string_view key)
    {
        auto removed = std::erase_if(headers_, [&](const http_header& header){
            return is_header(header.first, key);
        });
        if(removed) on_header_removed(key);
        return removed;
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
        return ::thinger::util::ascii::istarts_with(get_header(header::content_type), value);
    }

    bool headers::empty_headers() const{
        return headers_.empty();
    }

    void headers::debug_headers(std::ostream& os) const{
        for(const std::pair<std::string, std::string>& t: headers_){
            os << "\t> " << t.first << ": " << t.second << std::endl;
        }
    }

    namespace {

        [[maybe_unused]] constexpr std::string_view redacted = "[redacted]";

        // The first `max_length` bytes of `data` for a log line: control characters escaped,
        // so a value cannot forge log lines, and how much is left out
        [[maybe_unused]] std::string printable(std::string_view data, size_t max_length) {
            static constexpr char hex[] = "0123456789abcdef";
            auto shown = data.substr(0, max_length);
            std::string out;
            out.reserve(shown.size() + 32);
            for (unsigned char c : shown) {
                switch (c) {
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    case '\\': out += "\\\\"; break;
                    default:
                        if (c < 0x20 || c == 0x7f) {
                            out += "\\x";
                            out += hex[c >> 4];
                            out += hex[c & 0xf];
                        } else {
                            out += static_cast<char>(c);
                        }
                }
            }
            if (shown.size() < data.size()) {
                out += "... (";
                out += std::to_string(data.size() - shown.size());
                out += " more bytes)";
            }
            return out;
        }

        [[maybe_unused]] void log_header(std::string_view label, const std::string& name, const std::string& value,
                        const body_logging& options) {
            if (!options.enabled) {
                LOG_DEBUG("  {}{}", label, printable(name, name.size()));
            } else if (headers::is_sensitive_header(name)) {
                LOG_DEBUG("  {}{}: {}", label, printable(name, name.size()), redacted);
            } else {
                LOG_DEBUG("  {}{}: {}", label, printable(name, name.size()), printable(value, options.max_length));
            }
        }

    }

    bool headers::is_sensitive_header(std::string_view name) {
        return is_header(name, "Authorization") || is_header(name, "Proxy-Authorization") ||
               is_header(name, "Cookie") || is_header(name, "Set-Cookie");
    }

    void headers::log_headers(const body_logging& options) const {
        if (!LOG_DEBUG_ENABLED()) return;
        for (const auto& [name, value] : headers_) {
            log_header("", name, value, options);
        }
        for (const auto& [name, value] : proxy_headers_) {
            log_header("(PROXY) ", name, value, options);
        }
    }

    void headers::log_body(std::string_view body, size_t size, const body_logging& options) {
        if (size == 0 || !LOG_TRACE_ENABLED()) return;
        LOG_TRACE("Body: {} bytes", size);
        if (options.enabled && !body.empty()) {
            LOG_TRACE("  {}", printable(body, options.max_length));
        }
    }

    void headers::log(const char* scope, const body_logging& options) const {
        if (!LOG_DEBUG_ENABLED()) return;
        LOG_DEBUG("[{}] Headers:", scope);
        log_headers(options);
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

}
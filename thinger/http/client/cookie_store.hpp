#ifndef THINGER_HTTP_COOKIE_STORE_HPP
#define THINGER_HTTP_COOKIE_STORE_HPP

#include <unordered_map>
#include <optional>
#include "cookie.hpp"
#include "../common/headers.hpp"

namespace thinger::http {

    class cookie_store {
    public:
        cookie_store();
        virtual ~cookie_store();

        // Update from Set-Cookie headers
        bool update_from_headers(const headers& hdrs);

        // Get Cookie header string for requests
        [[nodiscard]] std::string get_cookie_string() const;

        // Cookie management
        void set_cookie(const cookie& c);
        void set_cookie(const std::string& name, const std::string& value);
        [[nodiscard]] std::optional<cookie> get_cookie(const std::string& name) const;
        [[nodiscard]] bool has_cookie(const std::string& name) const;
        void remove_cookie(const std::string& name);
        void clear();

        // Size info
        [[nodiscard]] size_t size() const;
        [[nodiscard]] bool empty() const;

    private:
        std::unordered_map<std::string, cookie> cookies_;
    };

}

#endif
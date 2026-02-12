#ifndef THINGER_HTTP_COOKIE_HPP
#define THINGER_HTTP_COOKIE_HPP

#include <string>
#include <cstdint>
#include <optional>

namespace thinger::http{

    enum class same_site_policy {
        none,
        lax,
        strict
    };

    class cookie {
    public:
        // constructors
        cookie() = default;
        cookie(std::string name, std::string value);
        virtual ~cookie() = default;

        // parsing
        static cookie parse(const std::string& cookie_string);

        // setters
        cookie& set_name(std::string name);
        cookie& set_value(std::string value);
        cookie& set_path(std::string path);
        cookie& set_domain(std::string domain);
        cookie& set_expires(int64_t expires);
        cookie& set_max_age(std::optional<int64_t> max_age);
        cookie& set_secure(bool secure);
        cookie& set_http_only(bool http_only);
        cookie& set_same_site(same_site_policy policy);

        // getters
        [[nodiscard]] const std::string& get_name() const;
        [[nodiscard]] const std::string& get_value() const;
        [[nodiscard]] const std::string& get_path() const;
        [[nodiscard]] const std::string& get_domain() const;
        [[nodiscard]] int64_t get_expires() const;
        [[nodiscard]] std::optional<int64_t> get_max_age() const;
        [[nodiscard]] bool is_secure() const;
        [[nodiscard]] bool is_http_only() const;
        [[nodiscard]] same_site_policy get_same_site() const;

        // validity
        [[nodiscard]] bool is_valid() const;
        [[nodiscard]] bool is_expired() const;

        // serialization
        [[nodiscard]] std::string to_string() const;

    private:
        std::string name_;
        std::string value_;
        std::string path_;
        std::string domain_;
        int64_t expires_ = 0;
        std::optional<int64_t> max_age_;
        bool secure_ = false;
        bool http_only_ = false;
        same_site_policy same_site_ = same_site_policy::lax;

        // helper for parsing
        static std::string trim(const std::string& str);
        static bool iequals(const std::string& a, const std::string& b);
    };

}

#endif
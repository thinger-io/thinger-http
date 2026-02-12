#include "cookie.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <ctime>

namespace thinger::http {

    // Helper to trim whitespace
    std::string cookie::trim(const std::string& str) {
        const auto start = str.find_first_not_of(" \t");
        if (start == std::string::npos) return "";
        const auto end = str.find_last_not_of(" \t");
        return str.substr(start, end - start + 1);
    }

    // Case-insensitive string comparison
    bool cookie::iequals(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        return std::equal(a.begin(), a.end(), b.begin(), [](char c1, char c2) {
            return std::tolower(static_cast<unsigned char>(c1)) ==
                   std::tolower(static_cast<unsigned char>(c2));
        });
    }

    // Parse HTTP date format (RFC 7231)
    static int64_t parse_http_date(const std::string& date_str) {
        std::tm tm = {};
        std::istringstream ss(date_str);

        // Try different date formats
        // Format 1: "Wdy, DD Mon YYYY HH:MM:SS GMT" (RFC 1123)
        ss >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S");
        if (!ss.fail()) {
            return static_cast<int64_t>(std::mktime(&tm));
        }

        // Format 2: "Wdy, DD-Mon-YY HH:MM:SS GMT" (RFC 850)
        ss.clear();
        ss.str(date_str);
        ss >> std::get_time(&tm, "%a, %d-%b-%y %H:%M:%S");
        if (!ss.fail()) {
            return static_cast<int64_t>(std::mktime(&tm));
        }

        // Format 3: "Wdy Mon DD HH:MM:SS YYYY" (asctime)
        ss.clear();
        ss.str(date_str);
        ss >> std::get_time(&tm, "%a %b %d %H:%M:%S %Y");
        if (!ss.fail()) {
            return static_cast<int64_t>(std::mktime(&tm));
        }

        return 0;
    }

    cookie::cookie(std::string name, std::string value)
        : name_(std::move(name)), value_(std::move(value)) {}

    cookie cookie::parse(const std::string& cookie_string) {
        cookie result;

        if (cookie_string.empty()) {
            return result;
        }

        // Split by semicolons
        std::vector<std::string> parts;
        std::string current;
        for (char c : cookie_string) {
            if (c == ';') {
                if (!current.empty()) {
                    parts.push_back(trim(current));
                    current.clear();
                }
            } else {
                current += c;
            }
        }
        if (!current.empty()) {
            parts.push_back(trim(current));
        }

        if (parts.empty()) {
            return result;
        }

        // First part is name=value
        const auto& name_value = parts[0];
        auto eq_pos = name_value.find('=');
        if (eq_pos != std::string::npos) {
            result.name_ = trim(name_value.substr(0, eq_pos));
            result.value_ = trim(name_value.substr(eq_pos + 1));
        } else {
            // Invalid cookie format - no = in first part
            return result;
        }

        // Parse remaining attributes
        for (size_t i = 1; i < parts.size(); ++i) {
            const auto& part = parts[i];
            eq_pos = part.find('=');

            if (eq_pos != std::string::npos) {
                std::string attr_name = trim(part.substr(0, eq_pos));
                std::string attr_value = trim(part.substr(eq_pos + 1));

                if (iequals(attr_name, "Path")) {
                    result.path_ = attr_value;
                } else if (iequals(attr_name, "Domain")) {
                    result.domain_ = attr_value;
                } else if (iequals(attr_name, "Expires")) {
                    result.expires_ = parse_http_date(attr_value);
                } else if (iequals(attr_name, "Max-Age")) {
                    try {
                        int64_t max_age = std::stoll(attr_value);
                        result.max_age_ = max_age;
                        // Also compute expires from max-age
                        auto now = std::chrono::system_clock::now();
                        auto expires_time = now + std::chrono::seconds(max_age);
                        result.expires_ = std::chrono::system_clock::to_time_t(expires_time);
                    } catch (...) {
                        // Invalid max-age, ignore
                    }
                } else if (iequals(attr_name, "SameSite")) {
                    if (iequals(attr_value, "Strict")) {
                        result.same_site_ = same_site_policy::strict;
                    } else if (iequals(attr_value, "Lax")) {
                        result.same_site_ = same_site_policy::lax;
                    } else if (iequals(attr_value, "None")) {
                        result.same_site_ = same_site_policy::none;
                    }
                }
            } else {
                // Flag attributes (no value)
                std::string attr_name = trim(part);
                if (iequals(attr_name, "Secure")) {
                    result.secure_ = true;
                } else if (iequals(attr_name, "HttpOnly")) {
                    result.http_only_ = true;
                }
            }
        }

        return result;
    }

    // Setters
    cookie& cookie::set_name(std::string name) {
        name_ = std::move(name);
        return *this;
    }

    cookie& cookie::set_value(std::string value) {
        value_ = std::move(value);
        return *this;
    }

    cookie& cookie::set_path(std::string path) {
        path_ = std::move(path);
        return *this;
    }

    cookie& cookie::set_domain(std::string domain) {
        domain_ = std::move(domain);
        return *this;
    }

    cookie& cookie::set_expires(int64_t expires) {
        expires_ = expires;
        return *this;
    }

    cookie& cookie::set_max_age(std::optional<int64_t> max_age) {
        max_age_ = max_age;
        return *this;
    }

    cookie& cookie::set_secure(bool secure) {
        secure_ = secure;
        return *this;
    }

    cookie& cookie::set_http_only(bool http_only) {
        http_only_ = http_only;
        return *this;
    }

    cookie& cookie::set_same_site(same_site_policy policy) {
        same_site_ = policy;
        return *this;
    }

    // Getters
    const std::string& cookie::get_name() const {
        return name_;
    }

    const std::string& cookie::get_value() const {
        return value_;
    }

    const std::string& cookie::get_path() const {
        return path_;
    }

    const std::string& cookie::get_domain() const {
        return domain_;
    }

    int64_t cookie::get_expires() const {
        return expires_;
    }

    std::optional<int64_t> cookie::get_max_age() const {
        return max_age_;
    }

    bool cookie::is_secure() const {
        return secure_;
    }

    bool cookie::is_http_only() const {
        return http_only_;
    }

    same_site_policy cookie::get_same_site() const {
        return same_site_;
    }

    bool cookie::is_valid() const {
        return !name_.empty();
    }

    bool cookie::is_expired() const {
        // Max-Age=0 or negative means cookie is expired (delete immediately)
        if (max_age_.has_value() && max_age_.value() <= 0) {
            return true;
        }
        // Session cookie (no expiry set) never expires
        if (expires_ == 0) {
            return false;
        }
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        return expires_ < now;
    }

    std::string cookie::to_string() const {
        std::ostringstream oss;
        oss << name_ << "=" << value_;

        if (!path_.empty()) {
            oss << "; Path=" << path_;
        }
        if (!domain_.empty()) {
            oss << "; Domain=" << domain_;
        }
        if (max_age_.has_value()) {
            oss << "; Max-Age=" << max_age_.value();
        } else if (expires_ > 0) {
            std::time_t t = static_cast<std::time_t>(expires_);
            std::tm* tm = std::gmtime(&t);
            char buf[64];
            std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", tm);
            oss << "; Expires=" << buf;
        }
        if (secure_) {
            oss << "; Secure";
        }
        if (http_only_) {
            oss << "; HttpOnly";
        }
        switch (same_site_) {
            case same_site_policy::strict:
                oss << "; SameSite=Strict";
                break;
            case same_site_policy::lax:
                oss << "; SameSite=Lax";
                break;
            case same_site_policy::none:
                oss << "; SameSite=None";
                break;
        }

        return oss.str();
    }

}

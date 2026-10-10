#include "virtual_host.hpp"
#include "request.hpp"
#include "response.hpp"
#include "../../util/ascii.hpp"
#include <boost/algorithm/string.hpp>
#include <filesystem>

namespace thinger::http {

namespace {

    bool is_identifier_start(char c) {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
    }

    bool is_identifier_char(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }

    // Convert a host pattern to a regex: "*" -> one label, ":name" -> one label captured
    // as `name`, ":name(regex)" -> custom capture; everything else is matched literally
    std::string host_pattern_to_regex(const std::string& pattern, std::vector<std::string>& parameters) {
        static const std::string special = ".^$|()[]{}*+?\\";
        static const std::string label = "([^.]+)";
        std::string regex;

        for (size_t i = 0; i < pattern.size(); ++i) {
            char c = pattern[i];
            if (c == '*') {
                regex += label;
                parameters.emplace_back();
            } else if (c == ':' && i + 1 < pattern.size() && is_identifier_start(pattern[i + 1])) {
                size_t end = i + 1;
                while (end < pattern.size() && is_identifier_char(pattern[end])) ++end;
                parameters.push_back(pattern.substr(i + 1, end - i - 1));

                if (end < pattern.size() && pattern[end] == '(') {
                    // custom regex up to the matching parenthesis
                    size_t close = end;
                    for (int depth = 0; close < pattern.size(); ++close) {
                        if (pattern[close] == '(') depth++;
                        else if (pattern[close] == ')' && --depth == 0) break;
                    }
                    regex += "(" + pattern.substr(end + 1, close - end - 1) + ")";
                    end = close + 1;
                } else {
                    regex += label;
                }
                i = end - 1;
            } else {
                if (special.find(c) != std::string::npos) regex += '\\';
                regex += c;
            }
        }
        return regex;
    }

}

virtual_host::virtual_host() : name_("*") {}

virtual_host::virtual_host(const std::string& name) : name_(name) {
    ::thinger::util::ascii::to_lower(name_);
    if (is_host_pattern(name_)) {
        pattern_ = std::regex(host_pattern_to_regex(name_, parameters_), std::regex::icase);
    }
}

virtual_host::virtual_host(const std::string& name, const std::regex& pattern)
    : name_(name), pattern_(pattern) {
    parameters_.resize(pattern.mark_count());
}

bool virtual_host::is_host_pattern(const std::string& name) {
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '*') return true;
        if (name[i] == ':' && i + 1 < name.size() && is_identifier_start(name[i + 1])) return true;
    }
    return false;
}

std::string virtual_host::normalize_host(std::string_view host) {
    std::string result;
    if (host.starts_with('[')) {
        // IPv6 literal, with an optional port after the closing bracket
        auto close = host.find(']');
        result = host.substr(0, close == std::string_view::npos ? host.size() : close + 1);
    } else {
        result = host.substr(0, host.find(':'));
    }
    boost::algorithm::trim(result);
    ::thinger::util::ascii::to_lower(result);
    if (result.ends_with('.')) result.pop_back();
    return result;
}

bool virtual_host::matches(const std::string& host, std::smatch& matches) const {
    return pattern_ && std::regex_match(host, matches, *pattern_);
}

void virtual_host::schema_component(const std::string& name, nlohmann::json schema) {
    router_.add_schema_component(name, std::move(schema));
}

// Static file serving
void virtual_host::serve_static(const std::string& url_prefix,
                                const std::string& directory,
                                const std::string& fallback) {
    namespace fs = std::filesystem;

    // Normalize route: avoid double slash when prefix is "/"
    std::string route = url_prefix;
    if (!route.empty() && route.back() == '/') route.pop_back();
    route += "/:path(.*)";

    auto& static_route = get(route, [directory, fallback](request& req, response& res) {
        std::string path = req["path"];

        auto canonical_dir = fs::canonical(directory);
        bool has_fallback = !fallback.empty();

        // Empty path means root request — try fallback file directly
        if (path.empty()) {
            if (has_fallback) {
                auto fallback_file = canonical_dir / fallback;
                if (fs::exists(fallback_file) && fs::is_regular_file(fallback_file)) {
                    res.send_file(fallback_file);
                    return;
                }
            }
            res.error(http_response::status::not_found, "Not found");
            return;
        }

        // Construct full file path
        fs::path file_path = fs::path(directory) / path;
        auto canonical_file = fs::weakly_canonical(file_path);

        // Security: ensure the resolved path is within the directory
        if (!canonical_file.string().starts_with(canonical_dir.string())) {
            res.error(http_response::status::forbidden, "Access denied");
            return;
        }

        // Serve file if it exists
        if (fs::exists(canonical_file)) {
            if (fs::is_regular_file(canonical_file)) {
                res.send_file(canonical_file);
                return;
            }
            if (fs::is_directory(canonical_file) && has_fallback) {
                auto fallback_file = canonical_file / fallback;
                if (fs::exists(fallback_file) && fs::is_regular_file(fallback_file)) {
                    res.send_file(fallback_file);
                    return;
                }
            }
        }

        // SPA fallback: serve root fallback file for non-existent paths
        if (has_fallback) {
            auto fallback_file = canonical_dir / fallback;
            if (fs::exists(fallback_file) && fs::is_regular_file(fallback_file)) {
                res.send_file(fallback_file);
                return;
            }
        }

        res.error(http_response::status::not_found, "Not found");
    });
    static_route.hidden();
}

} // namespace thinger::http

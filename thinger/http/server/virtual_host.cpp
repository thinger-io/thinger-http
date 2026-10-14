#include "virtual_host.hpp"
#include "request.hpp"
#include "response.hpp"
#include "body_decoders.hpp"
#include "mime_types.hpp"
#include "file_server/file_info.hpp"
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

namespace {

    // Whether `file` is `directory` or inside it (both canonical), by whole path components:
    // a sibling sharing its name as a prefix ("/srv/www-secret" next to "/srv/www") is outside
    bool is_within(const std::filesystem::path& file, const std::filesystem::path& directory) {
        auto relative = file.lexically_relative(directory);
        return !relative.empty() && *relative.begin() != "..";
    }

    // A precompressed sibling of a static file: its suffix, and its content coding
    struct precompressed_coding {
        const char* suffix;
        const char* coding;
    };
    constexpr precompressed_coding brotli_sibling{".br", "br"};
    constexpr precompressed_coding gzip_sibling{".gz", "gzip"};

    // Precompressed siblings a request accepts, in the order to look for them (how many):
    // br and gzip by the quality its Accept-Encoding gives them, br first at equal quality
    // (it is smaller); none without Accept-Encoding, nor one with a lower quality than
    // identity (as response compression)
    size_t accepted_siblings(const http_request& request, precompressed_coding (&siblings)[2]) {
        auto values = request.get_headers_with_key("Accept-Encoding");
        if (values.empty()) return 0;
        std::string accept_encoding;
        for (const auto& value : values) {
            if (!accept_encoding.empty()) accept_encoding += ", ";
            accept_encoding += value;
        }
        auto accepted = parse_accept_encoding(accept_encoding);
        bool br = accepted.br > 0 && accepted.br >= accepted.identity;
        bool gzip = accepted.gzip > 0 && accepted.gzip >= accepted.identity;
        bool brotli_first = accepted.br >= accepted.gzip;
        size_t count = 0;
        if (br && brotli_first) siblings[count++] = brotli_sibling;
        if (gzip) siblings[count++] = gzip_sibling;
        if (br && !brotli_first) siblings[count++] = brotli_sibling;
        return count;
    }

}

void virtual_host::send_static_file(request& req, response& res, const std::filesystem::path& file,
                                    const std::filesystem::path& root, bool precompressed) {
    namespace fs = std::filesystem;
    precompressed_coding siblings[2];
    size_t count = precompressed ? accepted_siblings(*req.get_http_request(), siblings) : 0;
    if (count == 0) {
        res.send_file(file);
        return;
    }
    if (!res.ensure_not_responded()) return;

    // One stat for the file (send_file does not repeat it), and one per sibling looked for
    auto original = file_info::of(file);
    if (!original || !original->regular) {
        res.send_file(file);
        return;
    }
    const auto& content_type = mime_types::extension_to_type(file.extension().string());
    for (size_t i = 0; i < count; ++i) {
        auto sibling = file;
        sibling += siblings[i].suffix;
        // (a symlink only while it resolves inside the directory, as the files)
        auto info = file_info::of(sibling, false);
        if (info && info->symlink) {
            std::error_code error;
            auto target = fs::canonical(sibling, error);
            info = !error && is_within(target, root) ? file_info::of(target) : std::nullopt;
        }
        // A sibling older than the file is stale: compressed from a previous version
        if (!info || !info->regular || info->mtime_ns < original->mtime_ns) continue;
        // Its own entity tag, telling the coding (as Apache does): another representation
        info->etag.insert(info->etag.size() - 1, std::string("-") + siblings[i].coding);
        res.send_file(sibling, *info, content_type, siblings[i].coding, false);
        return;
    }
    res.send_file(file, *original, content_type, {}, false);
}

// Static file serving
void virtual_host::serve_static(const std::string& url_prefix,
                                const std::string& directory,
                                const std::string& fallback) {
    serve_static(url_prefix, directory, static_options{.fallback = fallback});
}

void virtual_host::serve_static(const std::string& url_prefix,
                                const std::string& directory,
                                static_options options) {
    namespace fs = std::filesystem;

    // Normalize route: avoid double slash when prefix is "/"
    std::string route = url_prefix;
    if (!route.empty() && route.back() == '/') route.pop_back();
    route += "/:path(.*)";

    auto handler = [directory, options = std::move(options)](request& req, response& res) {
        std::string path = req["path"];
        const auto& fallback = options.fallback;

        auto canonical_dir = fs::canonical(directory);
        bool has_fallback = !fallback.empty();

        auto send = [&](const fs::path& file) {
            if (!options.cache_control.empty()) res.header(header::cache_control, options.cache_control);
            send_static_file(req, res, file, canonical_dir, options.precompressed);
        };

        // Empty path means root request — try fallback file directly
        if (path.empty()) {
            if (has_fallback) {
                auto fallback_file = canonical_dir / fallback;
                if (fs::exists(fallback_file) && fs::is_regular_file(fallback_file)) {
                    send(fallback_file);
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
        if (!is_within(canonical_file, canonical_dir)) {
            res.error(http_response::status::forbidden, "Access denied");
            return;
        }

        // Serve file if it exists
        if (fs::exists(canonical_file)) {
            if (fs::is_regular_file(canonical_file)) {
                send(canonical_file);
                return;
            }
            if (fs::is_directory(canonical_file) && has_fallback) {
                auto fallback_file = canonical_file / fallback;
                if (fs::exists(fallback_file) && fs::is_regular_file(fallback_file)) {
                    send(fallback_file);
                    return;
                }
            }
        }

        // SPA fallback: serve root fallback file for non-existent paths
        if (has_fallback) {
            auto fallback_file = canonical_dir / fallback;
            if (fs::exists(fallback_file) && fs::is_regular_file(fallback_file)) {
                send(fallback_file);
                return;
            }
        }

        res.error(http_response::status::not_found, "Not found");
    };
    // (HEAD requests reach it too, unless a HEAD route matches them)
    get(route, std::move(handler)).hidden();
}

} // namespace thinger::http

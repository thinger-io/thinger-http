#include "static_files.hpp"
#include "file_info.hpp"
#include "file_response.hpp"
#include "../mime_types.hpp"
#include "../request.hpp"
#include "../response.hpp"
#include "../response_compression.hpp"
#include "../../util/url.hpp"
#include <algorithm>
#include <optional>
#include <string_view>
#include <system_error>

namespace thinger::http::file_server {

namespace {

    namespace fs = std::filesystem;

    // Whether `file` is `directory` or inside it (both canonical), by whole path components:
    // a sibling sharing its name as a prefix ("/srv/www-secret" next to "/srv/www") is outside
    bool is_within(const fs::path& file, const fs::path& directory) {
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
        auto accepted = compression::accepted_by(request);
        bool br = accepted.br > 0 && accepted.br >= accepted.identity;
        bool gzip = accepted.gzip > 0 && accepted.gzip >= accepted.identity;
        bool brotli_first = accepted.br >= accepted.gzip;
        size_t count = 0;
        if (br && brotli_first) siblings[count++] = brotli_sibling;
        if (gzip) siblings[count++] = gzip_sibling;
        if (br && !brotli_first) siblings[count++] = brotli_sibling;
        return count;
    }

    // Metadata of a precompressed sibling of `original`, if it can be sent: a regular file
    // (a symlink only while it resolves inside `root`, as the files) not older than it
    std::optional<file_info> sibling_info(const fs::path& sibling, const fs::path& root, const file_info& original,
                                          const char* coding) {
        auto info = file_info::of(sibling, false);
        if (info && info->symlink) {
            std::error_code error;
            auto target = fs::canonical(sibling, error);
            info = !error && is_within(target, root) ? file_info::of(target) : std::nullopt;
        }
        // A sibling older than the file is stale: compressed from a previous version
        if (!info || !info->regular || info->mtime_ns < original.mtime_ns) return std::nullopt;
        // Its own entity tag, telling the coding (as Apache does): another representation
        info->etag.insert(info->etag.size() - 1, std::string("-") + coding);
        return info;
    }

    // Send a static `file` (canonical, inside the canonical directory `root`), or its
    // precompressed sibling if `precompressed` and the request accepts it
    void send_static_file(request& req, response& res, const fs::path& file, const fs::path& root,
                          bool precompressed) {
        precompressed_coding siblings[2];
        size_t count = precompressed ? accepted_siblings(*req.get_http_request(), siblings) : 0;
        // Once answered, send_file() reports it without looking for the file
        if (count == 0 || res.has_responded()) {
            res.send_file(file);
            return;
        }

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
            if (auto info = sibling_info(sibling, root, *original, siblings[i].coding)) {
                send(res, {sibling, *info, content_type, siblings[i].coding});
                return;
            }
        }
        send(res, {file, *original, content_type});
    }

    // Whether a decoded path has a dotfile segment (as Node's send): longer than one
    // character and starting with ".", other than ".well-known" (RFC 8615) and ".." (left to
    // the containment check, as send refuses it before). Segments are split as
    // std::filesystem splits them.
    bool is_dotfile_path(std::string_view path) {
#ifdef _WIN32
        constexpr std::string_view separators = "/\\";
#else
        constexpr std::string_view separators = "/";
#endif
        size_t start = 0;
        while (start < path.size()) {
            auto end = std::min(path.find_first_of(separators, start), path.size());
            auto segment = path.substr(start, end - start);
            if (segment.size() > 1 && segment[0] == '.' && segment != ".." && segment != ".well-known") return true;
            start = end + 1;
        }
        return false;
    }

    // Percent-decodes a raw path parameter in place, with "+" literal: false for a bad escape
    // or a decoded NUL
    bool decode_path(std::string& path) {
        if (path.find('%') == std::string::npos) return true;
        std::string escaped;
        escaped.reserve(path.size());
        for (char c : path) {
            if (c == '+') escaped.append("%2B");
            else escaped.push_back(c);
        }
        return util::url::url_decode(escaped, path) && path.find('\0') == std::string::npos;
    }

    // The fallback file of a directory, if it has one
    std::optional<fs::path> fallback_of(const fs::path& directory, const std::string& fallback) {
        if (fallback.empty()) return std::nullopt;
        auto file = directory / fallback;
        if (!fs::is_regular_file(file)) return std::nullopt;
        return file;
    }

    void send_file(request& req, response& res, const fs::path& file, const fs::path& root,
                   const static_options& options) {
        if (!options.cache_control.empty()) res.header(header::cache_control, options.cache_control);
        send_static_file(req, res, file, root, options.precompressed);
    }

    // Answers with the file a decoded path names, or the index of the directory it names: false
    // if the path has no file, leaving the request unanswered
    bool serve_path(request& req, response& res, const fs::path& directory, const std::string& path,
                    const fs::path& root, const static_options& options) {
        std::error_code error;
        auto file = fs::weakly_canonical(directory / path, error);
        if (error) {
            // A name too long to exist is a path without a file (as in Node's send); any other
            // failure of the file system is one of the server
            if (options.fallthrough && error == std::errc::filename_too_long) return false;
            throw fs::filesystem_error("weakly_canonical", directory / path, error);
        }
        if (!is_within(file, root)) {
            res.error(http_response::status::forbidden, "Access denied");
            return true;
        }
        auto status = fs::status(file);
        if (fs::is_regular_file(status)) {
            send_file(req, res, file, root, options);
            return true;
        }
        if (fs::is_directory(status)) {
            if (auto index = fallback_of(file, options.fallback)) {
                send_file(req, res, *index, root, options);
                return true;
            }
        }
        return false;
    }

}

bool serve(request& req, response& res, const fs::path& directory, std::string_view raw_path,
           const static_options& options) {
    // The path is raw: percent-decoded here as a path, where "+" is literal (as Node's send does),
    // so escaped first for url_decode, which reads it as a form space. A decoded "/" separates
    // directories; a NUL could not name a file
    std::string path(raw_path);
    if (!decode_path(path)) {
        res.error(http_response::status::bad_request, "Invalid path");
        return true;
    }
    // Dotfiles (".env", ".git/"), judged by the path alone: denied, or ignored as a path
    // without a file, unless allowed
    bool dotfile = options.dotfiles != dotfiles_policy::allow && is_dotfile_path(path);
    if (dotfile && options.dotfiles == dotfiles_policy::deny) {
        res.error(http_response::status::forbidden, "Access denied");
        return true;
    }
    auto root = fs::canonical(directory);

    // An empty path is the root: its fallback, if any
    if (!path.empty() && !dotfile && serve_path(req, res, directory, path, root, options)) return true;

    // The fallback of the root for the paths without a file (single page applications), which
    // with fallthrough are for the caller: there it is only the index of the root
    if (!options.fallthrough || path.empty()) {
        if (auto index = fallback_of(root, options.fallback)) {
            send_file(req, res, *index, root, options);
            return true;
        }
    }
    if (options.fallthrough) return false;
    res.error(http_response::status::not_found, "Not found");
    return true;
}

} // namespace thinger::http::file_server

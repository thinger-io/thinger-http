#include "static_files.hpp"
#include "file_info.hpp"
#include "file_response.hpp"
#include "../mime_types.hpp"
#include "../request.hpp"
#include "../response.hpp"
#include "../response_compression.hpp"
#include "../virtual_host.hpp"
#include <optional>

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

    // The fallback file of a directory, if it has one
    std::optional<fs::path> fallback_of(const fs::path& directory, const std::string& fallback) {
        if (fallback.empty()) return std::nullopt;
        auto file = directory / fallback;
        if (!fs::is_regular_file(file)) return std::nullopt;
        return file;
    }

}

void serve(request& req, response& res, const fs::path& directory, const static_options& options) {
    std::string path = req["path"];
    auto root = fs::canonical(directory);
    auto send = [&](const fs::path& file) {
        if (!options.cache_control.empty()) res.header(header::cache_control, options.cache_control);
        send_static_file(req, res, file, root, options.precompressed);
    };

    // An empty path is the root: its fallback, if any
    if (!path.empty()) {
        auto file = fs::weakly_canonical(directory / path);
        if (!is_within(file, root)) {
            res.error(http_response::status::forbidden, "Access denied");
            return;
        }
        auto status = fs::status(file);
        if (fs::is_regular_file(status)) {
            send(file);
            return;
        }
        if (fs::is_directory(status)) {
            if (auto index = fallback_of(file, options.fallback)) {
                send(*index);
                return;
            }
        }
    }

    // The fallback of the root for the paths without a file (single page applications)
    if (auto index = fallback_of(root, options.fallback)) {
        send(*index);
        return;
    }
    res.error(http_response::status::not_found, "Not found");
}

} // namespace thinger::http::file_server

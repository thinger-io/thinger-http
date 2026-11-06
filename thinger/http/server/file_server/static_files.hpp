#ifndef THINGER_HTTP_SERVER_STATIC_FILES_HPP
#define THINGER_HTTP_SERVER_STATIC_FILES_HPP

#include <filesystem>
#include <string>
#include <string_view>

namespace thinger::http {

class request;
class response;

// What is done with a dotfile path (see static_options::dotfiles)
enum class dotfiles_policy {
    ignore, // as a path without a file: the root fallback, or 404 (with fallthrough, it passes)
    allow,  // served as any other file
    deny    // 403
};

// How the files of a directory are served, by virtual_host::serve_static() or file_server::serve()
struct static_options {
    // File served for a directory (its own one), and for the paths without a file (the one
    // of the root directory, for single page applications); empty: 404 for both. With
    // fallthrough, only for a directory (the root too).
    std::string fallback = "index.html";
    // Cache-Control of the files. By default, caches keep them but revalidate them on every
    // use: answered with 304 Not Modified (no body) while they do not change. Assets with a
    // hash in their names can be reused without asking: "public, max-age=31536000, immutable".
    std::string cache_control = "no-cache";
    // Precompressed files (as nginx gzip_static and brotli_static): a file is sent as its
    // "<file>.br" or "<file>.gz" sibling, if there is one not older than it and the client
    // accepts br or gzip (br first at equal quality), with that Content-Encoding and the
    // media type of the file. A build can then serve Brotli without compressing at runtime.
    bool precompressed = true;
    // Dotfile paths (".env", ".git/config", ".htpasswd"): those with a segment of the decoded
    // path under the prefix longer than one character that starts with "." (except
    // ".well-known", always served). Judged by the path alone, whether or not the file
    // exists. Ignored by default, as Express's serve-static does.
    dotfiles_policy dotfiles = dotfiles_policy::ignore;
    // A path without a file to serve (it does not exist, a directory without its fallback, what
    // is neither a file nor a directory, or an ignored dotfile) is not answered here: it goes to
    // the not-found handler of the host, or its 404. The 400 and 403 of a rejected path are
    // still answered, unlike Express's serve-static, where this is on by default and they pass.
    bool fallthrough = false;
};

namespace file_server {

    // Answer a request with a file of `directory`, from a handler of your own (a not-found
    // handler, a route with other parameters), as virtual_host::serve_static() does: with the
    // same options and guarantees (containment, dotfiles, validators and 304, byte ranges,
    // precompressed siblings, Cache-Control).
    //
    // `path` is the file to serve relative to the directory, raw as it comes in the URI and
    // without its query: it is percent-decoded here, so the caller must not decode it ("+" is
    // literal; a bad escape or a decoded NUL gets 400). Empty, it is the directory itself. The
    // caller removes its prefix with the "/" after it: a path starting with "/" is absolute, not
    // relative to the directory, and gets 403 outside it. No request parameter is read.
    //
    // Returns whether it answered. Always, unless static_options::fallthrough leaves a path
    // without a file for the caller: false, with the response untouched, for the caller to
    // answer it. A rejected path (400, 403) is answered, so true.
    //
    // It does not look at the method: serve_static() only gets GET and HEAD, while a not-found
    // handler gets every method, and must check it first. A failure of the file system (the
    // directory does not exist, no permission) is thrown, which the server answers with 500.
    bool serve(request& req, response& res, const std::filesystem::path& directory, std::string_view path,
               const static_options& options);

}

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_STATIC_FILES_HPP

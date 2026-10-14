#ifndef THINGER_HTTP_SERVER_STATIC_FILES_HPP
#define THINGER_HTTP_SERVER_STATIC_FILES_HPP

#include <filesystem>

namespace thinger::http {

class request;
class response;
struct static_options;

namespace file_server {

    // Answer a request for a static file of `directory` (see virtual_host::serve_static): the
    // file at its "path" parameter, the fallback of a directory or of the root, or an error
    void serve(request& req, response& res, const std::filesystem::path& directory, const static_options& options);

}

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_STATIC_FILES_HPP

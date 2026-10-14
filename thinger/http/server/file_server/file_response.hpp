#ifndef THINGER_HTTP_SERVER_FILE_RESPONSE_HPP
#define THINGER_HTTP_SERVER_FILE_RESPONSE_HPP

#include "file_info.hpp"
#include <filesystem>
#include <string>
#include <string_view>

namespace thinger::http {

class response;

namespace file_server {

    // A representation of a regular file sent as a response: the file itself, or a
    // precompressed sibling of it (see static_options::precompressed)
    struct representation {
        const std::filesystem::path& path;
        const file_info& info;
        const std::string& content_type;
        // Content coding of a precompressed sibling (e.g. "gzip" for the .gz one), sent as its
        // Content-Encoding: never compressed again. Empty for the file itself.
        std::string_view encoding;
        bool force_download = false;
    };

    // Answer with a file representation, as response::send_file describes: the whole file,
    // a byte range of it, or 304 if the client has it already
    void send(response& res, const representation& file);

}

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_FILE_RESPONSE_HPP

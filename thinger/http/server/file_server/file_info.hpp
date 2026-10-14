#ifndef THINGER_HTTP_SERVER_FILE_INFO_HPP
#define THINGER_HTTP_SERVER_FILE_INFO_HPP

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace thinger::http {

class http_request;

// Metadata of a file sent as a response (see response::send_file), read with a single stat(),
// and its validators for conditional requests (RFC 9110, 8.8), derived from its size and
// modification time (the content is not hashed)
struct file_info {
    bool regular = false;           // a regular file (not a directory, a device...)
    bool symlink = false;           // a symbolic link (only if read without following them)
    uint64_t size = 0;
    std::time_t mtime = 0;          // modification time, in seconds since the epoch
    uint64_t mtime_ns = 0;          // modification time, in nanoseconds since the epoch
    // Weak (the same file may be sent compressed or not): W/"<size>-<mtime in ns>", in hex
    std::string etag;
    // Modification time as an HTTP-date (IMF-fixdate)
    std::string last_modified;

    // Metadata of `path`, following symlinks (or else of a symlink itself); nullopt if it
    // cannot be read (e.g. it does not exist)
    static std::optional<file_info> of(const std::filesystem::path& path, bool follow_symlinks = true);

    // Whether a GET or HEAD request is answered with 304 Not Modified (RFC 9110, 13.2.2):
    // If-None-Match is "*" or lists the entity tag (weak comparison); without If-None-Match,
    // the file was not modified after If-Modified-Since (with second resolution; ignored if
    // it is not a valid date)
    bool not_modified(const http_request& request) const;
};

// Whether an If-None-Match value is "*" or lists `etag`, compared weakly (ignoring "W/")
bool etag_matches(std::string_view if_none_match, std::string_view etag);

// Bytes [first, first + length) of a resource
struct byte_range {
    uint64_t first = 0;
    uint64_t length = 0;
};

// Range of a Range header (RFC 9110, 14.2) for a resource of `size` bytes, if it asks for
// a single range: "bytes=first-last", "bytes=first-" or "bytes=-suffix_length". A range of
// length 0 if it cannot be satisfied (it starts past the end); nullopt to send the whole
// resource instead (another unit, several ranges, or an invalid one).
std::optional<byte_range> parse_byte_range(std::string_view range, uint64_t size);

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_FILE_INFO_HPP

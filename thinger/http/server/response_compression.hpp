#ifndef THINGER_HTTP_SERVER_RESPONSE_COMPRESSION_HPP
#define THINGER_HTTP_SERVER_RESPONSE_COMPRESSION_HPP

#include <cstddef>
#include <string_view>

namespace thinger::http {

class http_request;
class http_response;

// Qualities an Accept-Encoding header (RFC 9110, 12.5.3; empty if missing) gives the content
// codings, in thousandths: by name (case-insensitive, x-gzip is gzip) or else through "*";
// 0 if not acceptable, -1 if not listed (identity is still acceptable then, as the last
// choice). An invalid weight drops the coding from the list.
struct accepted_codings {
    int gzip;
    int deflate;
    int br;
    int identity;
};
accepted_codings parse_accept_encoding(std::string_view accept_encoding);

// Content coding to compress a response with, as the Accept-Encoding header asks (see
// parse_accept_encoding): gzip or deflate, by the quality the client gives them; gzip at
// equal quality, as Express. Identity if the client accepts neither, or gives identity a
// higher quality.
enum class content_coding { identity, gzip, deflate };
content_coding negotiate_content_coding(std::string_view accept_encoding);

// Compression of the responses sent whole (see response)
namespace compression {

    // Responses of at least this size, of a text-based type, are compressed for the clients
    // accepting it, unless Cache-Control says no-transform
    inline constexpr size_t threshold = 200;

    // Qualities the Accept-Encoding headers of a request give (they make one list)
    accepted_codings accepted_by(const http_request& request);

    // Whether a body of `size` bytes and `content_type` is compressed for the clients
    // accepting it, given the headers of `message`
    bool compressible(const http_response& message, size_t size, std::string_view content_type);

    // Whether the content of `message` is compressed for the clients accepting it
    bool compressible(const http_response& message);

    // Add `field` to the Vary header (as the vary module of Express): appended to the one
    // there, unless it has it or is "*"
    void add_vary(http_response& message, std::string_view field);

    // Compress a compressible message if the client accepts it, adding "Vary: Accept-Encoding"
    // either way (as the compression middleware of Express)
    void compress(http_response& message, const http_request& request);

}

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_RESPONSE_COMPRESSION_HPP

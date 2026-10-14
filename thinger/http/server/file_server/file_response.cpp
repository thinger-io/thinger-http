#include "file_response.hpp"
#include "../response.hpp"
#include "../response_compression.hpp"
#include <fstream>
#include <optional>

namespace thinger::http::file_server {

namespace {

    // Headers of the 200 and 304 answers alike: the validators, and how to cache them.
    // Without a Cache-Control of the handler, caches store the file but revalidate it on
    // every use (else they could reuse it for a while, guessing from Last-Modified).
    void add_validators(http_response& message, const representation& file) {
        if (!message.has_header(header::cache_control)) message.add_header(header::cache_control, "no-cache");
        message.set_header("ETag", file.info.etag);
        message.set_header("Last-Modified", file.info.last_modified);
        // A precompressed file is sent depending on Accept-Encoding, and a text file may be
        // sent compressed: another representation either way
        if (!file.encoding.empty() || compression::compressible(message, file.info.size, file.content_type)) {
            compression::add_vary(message, "Accept-Encoding");
        }
    }

    void add_content_headers(http_response& message, const representation& file) {
        message.set_status(http_response::status::ok);
        message.set_content_type(file.content_type);
        if (!file.encoding.empty()) message.add_header("Content-Encoding", std::string(file.encoding));
        message.add_header("Accept-Ranges", "bytes");
        if (file.force_download) {
            message.add_header("Content-Disposition", "attachment; filename=\"" + file.path.filename().string() + "\"");
        }
    }

    // The single byte range of a GET request (RFC 9110, 14.2), unless If-Range asks for the
    // whole file once it changed: only the date can match, as the entity tag is weak. The
    // range of a precompressed file is one of its bytes, as sent (as nginx does).
    std::optional<byte_range> requested_range(const http_request& request, const file_info& file) {
        const auto& range = request.get_header("Range");
        if (range.empty()) return std::nullopt;
        const auto& if_range = request.get_header("If-Range");
        if (!if_range.empty() && if_range != file.last_modified) return std::nullopt;
        return parse_byte_range(range, file.size);
    }

    void make_unsatisfiable(http_response& message, uint64_t size, const error_formatter* formatter) {
        message.set_status(http_response::status::range_not_satisfiable);
        message.add_header("Content-Range", "bytes */" + std::to_string(size));
        format_error(formatter, {http_response::status::range_not_satisfiable, "Range not satisfiable", nullptr},
                     message);
    }

    bool read_file(const std::filesystem::path& path, uint64_t first, uint64_t length, std::string& content) {
        std::ifstream stream(path, std::ios::binary);
        content.assign(length, '\0');
        return stream && stream.seekg(static_cast<std::streamoff>(first)) &&
               stream.read(content.data(), static_cast<std::streamsize>(length));
    }

}

void send(response& res, const representation& file) {
    const auto& request = res.request_message();
    auto verb = request.get_method();

    auto message = res.new_response();
    add_validators(*message, file);

    // Conditional request for a file the client has: no body, nor the length of the file
    if ((verb == method::GET || verb == method::HEAD) && file.info.not_modified(request)) {
        message->set_status(http_response::status::not_modified);
        res.send_message(std::move(message), false);
        return;
    }

    add_content_headers(*message, file);

    // HEAD: the headers of the GET answer (with the length of the file, as it is), no body
    if (verb == method::HEAD) {
        message->set_content_length(file.info.size);
        res.send_message(std::move(message), false);
        return;
    }

    auto range = verb == method::GET ? requested_range(request, file.info) : std::nullopt;
    if (range && range->length == 0) {
        auto unsatisfiable = res.new_response();
        make_unsatisfiable(*unsatisfiable, file.info.size, res.formatter());
        res.send_message(std::move(unsatisfiable));
        return;
    }

    uint64_t first = range ? range->first : 0;
    uint64_t length = range ? range->length : file.info.size;
    std::string content;
    if (!read_file(file.path, first, length, content)) {
        res.error(http_response::status::internal_server_error, "Failed to read file");
        return;
    }
    message->set_content(std::move(content));

    // A range is sent as is: compressing it would change the bytes it refers to
    if (range) {
        message->set_status(http_response::status::partial_content);
        message->add_header("Content-Range", "bytes " + std::to_string(first) + "-" +
                                             std::to_string(first + length - 1) + "/" + std::to_string(file.info.size));
        res.send_message(std::move(message), false);
        return;
    }
    res.send_message(std::move(message), file.encoding.empty());
}

} // namespace thinger::http::file_server

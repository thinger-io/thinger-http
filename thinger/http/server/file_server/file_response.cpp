#include "file_response.hpp"
#include "../response.hpp"
#include "../server_connection.hpp"
#include "../detail/spawn_handler.hpp"
#include "../../common/http_request.hpp"
#include "../../common/syntax.hpp"
#include "../response_compression.hpp"
#include "../../../asio/sockets/socket.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>

namespace thinger::http::file_server {

namespace {

    // Bodies longer than this are streamed, a piece at a time at the pace of the client,
    // instead of read whole: what a response holds does not grow with the file (nginx and
    // Node's send, behind express.static, never read a file whole either)
    constexpr uint64_t stream_threshold = 1024 * 1024;

    // The most bytes of a streamed body read and written at once: the pieces of Node's file
    // streams (their default highWaterMark)
    constexpr uint64_t stream_piece = 64 * 1024;

    // Files longer than this are not compressed on the fly, which needs their body whole:
    // they are sent as they are (or a precompressed sibling of them)
    constexpr uint64_t compression_cap = 4 * 1024 * 1024;

    // Whether the file is compressed on the fly for the clients accepting it (see
    // compression::compressible), given the headers of `message`
    bool compressed_on_the_fly(const http_response& message, const representation& file) {
        return file.encoding.empty() && file.info.size <= compression_cap &&
               compression::compressible(message, file.info.size, file.content_type);
    }

    // Headers of the 200 and 304 answers alike: the validators, and how to cache them.
    // Without a Cache-Control of the handler, caches store the file but revalidate it on
    // every use (else they could reuse it for a while, guessing from Last-Modified).
    void add_validators(http_response& message, const representation& file) {
        if (!message.has_header(header::cache_control)) message.add_header(header::cache_control, "no-cache");
        message.set_header("ETag", file.info.etag);
        message.set_header("Last-Modified", file.info.last_modified);
        // A precompressed file is sent depending on Accept-Encoding, and a text file may be
        // sent compressed: another representation either way
        if (!file.encoding.empty() || compressed_on_the_fly(message, file)) {
            compression::add_vary(message, "Accept-Encoding");
        }
    }

    // The continuation bytes that follow a UTF-8 lead byte (0 for any other byte)
    int utf8_continuation_bytes(unsigned char lead) {
        if (lead >= 0xf0 && lead < 0xf8) return 3;
        if (lead >= 0xe0 && lead < 0xf0) return 2;
        if (lead >= 0xc0 && lead < 0xe0) return 1;
        return 0;
    }

    // The name as the value of filename* (RFC 8187), after its "UTF-8''"
    void append_ext_value(std::string& value, std::string_view name) {
        static constexpr char hex[] = "0123456789ABCDEF";
        for (unsigned char c : name) {
            // attr-char: a tchar but '*', '\'' and '%'
            if (syntax::is_token_char(c) && c != '*' && c != '\'' && c != '%') {
                value += static_cast<char>(c);
            } else {
                value += '%';
                value += hex[c >> 4];
                value += hex[c & 0x0F];
            }
        }
    }

    // Content-Disposition of a download named `name`, as Express's content-disposition
    // package writes it (RFC 6266): an ASCII filename (each other character, or control byte,
    // as '?'), and the name in UTF-8 as filename* (RFC 8187) when the ASCII one is not it, or
    // could be read percent-decoded
    std::string content_disposition(std::string_view name) {
        auto is_hex = [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; };
        std::string fallback;
        bool extended = false;
        int continuation = 0;  // bytes left of the UTF-8 character written as '?'
        for (size_t i = 0; i < name.size(); i++) {
            auto c = static_cast<unsigned char>(name[i]);
            if (c < 0x20 || c > 0x7e) {
                extended = true;
                if (continuation > 0 && (c & 0xc0) == 0x80) {
                    continuation--;
                    continue;
                }
                continuation = utf8_continuation_bytes(c);
                fallback += '?';
                continue;
            }
            continuation = 0;
            if (c == '%' && i + 2 < name.size() && is_hex(name[i + 1]) && is_hex(name[i + 2])) extended = true;
            if (c == '"' || c == '\\') fallback += '\\';
            fallback += static_cast<char>(c);
        }
        std::string value = "attachment; filename=\"" + fallback + "\"";
        if (!extended) return value;
        value += "; filename*=UTF-8''";
        append_ext_value(value, name);
        return value;
    }

    void add_content_headers(http_response& message, const representation& file) {
        message.set_status(http_response::status::ok);
        message.set_content_type(file.content_type);
        if (!file.encoding.empty()) message.add_header("Content-Encoding", std::string(file.encoding));
        message.add_header("Accept-Ranges", "bytes");
        if (file.force_download) {
            message.add_header("Content-Disposition", content_disposition(file.path.filename().string()));
        }
    }

    // The single byte range of a GET request (RFC 9110, 14.2), unless it is answered 304
    // (`not_modified`), or If-Range asks for the whole file once it changed: only the date can
    // match, as the entity tag is weak. The range of a precompressed file is one of its bytes,
    // as sent (as nginx does).
    std::optional<byte_range> requested_range(const http_request& request, const file_info& file, bool not_modified) {
        if (request.get_method() != method::GET || not_modified) return std::nullopt;
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

    std::string content_range(uint64_t first, uint64_t length, uint64_t size) {
        return "bytes " + std::to_string(first) + "-" + std::to_string(first + length - 1) + "/" + std::to_string(size);
    }

    bool read_file(const std::filesystem::path& path, uint64_t first, uint64_t length, std::string& content) {
        std::ifstream stream(path, std::ios::binary);
        content.assign(length, '\0');
        return stream && stream.seekg(static_cast<std::streamoff>(first)) &&
               stream.read(content.data(), static_cast<std::streamsize>(length));
    }

    // Write `length` bytes of `file`, from where it is, as the body of the stream started on
    // `res`: a piece at a time, each once the previous one is written. A file shorter than
    // announced, or a client gone, leaves the body incomplete (closing the connection). It
    // holds the `connection`: once its read loop stops (the connection closes after this
    // response), only the frame being written would, and the next piece would find it gone.
    // Its write deadline still closes it, failing the write waited for.
    awaitable<void> write_file(response res, std::shared_ptr<server_connection> connection, std::ifstream file,
                               uint64_t length) {
        while (length > 0) {
            auto size = std::min(length, stream_piece);
            std::string piece(size, '\0');
            bool written = false;
            if (file.read(piece.data(), static_cast<std::streamsize>(size))) {
                written = co_await res.write_stream(std::move(piece));
            }
            if (!written) {
                res.abort_stream();
                co_return;
            }
            length -= size;
        }
        co_await res.end_stream();
    }

    // Stream bytes [first, first + length) of the file to `connection` (the one of `res`), with
    // the headers of `draft` (the response's) and the status of a range or the whole file
    void stream(response& res, http_response& draft, const representation& file, bool range, uint64_t first,
                uint64_t length, std::shared_ptr<server_connection> connection) {
        // Opened before any header is set: the error answered instead keeps them
        std::ifstream content(file.path, std::ios::binary);
        if (!content || !content.seekg(static_cast<std::streamoff>(first))) {
            res.error(http_response::status::internal_server_error, "Failed to read file");
            return;
        }
        add_validators(draft, file);
        add_content_headers(draft, file);
        http_response head;
        head.set_status(range ? http_response::status::partial_content : http_response::status::ok);
        if (range) head.add_header("Content-Range", content_range(first, length, file.info.size));
        if (!res.start_stream(head, length)) return;
        auto& io_context = connection->get_socket()->get_io_context();
        detail::spawn_handler(io_context, "file stream",
                              write_file(res, std::move(connection), std::move(content), length));
    }

    // Whether a long body, with a connection to stream it to, is streamed: unless it is
    // compressed on the fly, which needs it whole (the whole file, for a client accepting a
    // coding), given the headers of `draft`
    bool streamed(const http_response& draft, const http_request& request, const representation& file, bool range) {
        return range || !compressed_on_the_fly(draft, file) ||
               compression::negotiated(request) == content_coding::identity;
    }

    // Fill `message` with the answer of the file read whole (`content`, bytes [first,
    // first + length) of it), or its headers only (a 304, or HEAD): whether it may be compressed
    bool fill_read(http_response& message, const representation& file, method verb, bool not_modified,
                   bool range, uint64_t first, uint64_t length, std::string content) {
        add_validators(message, file);

        if (not_modified) {
            message.set_status(http_response::status::not_modified);
            return false;
        }

        add_content_headers(message, file);

        // HEAD: the headers of the GET answer (with the length of the file, as it is), no body
        if (verb == method::HEAD) {
            message.set_content_length(file.info.size);
            return false;
        }

        message.set_content(std::move(content));

        // A range is sent as is: compressing it would change the bytes it refers to
        if (range) {
            message.set_status(http_response::status::partial_content);
            message.add_header("Content-Range", content_range(first, length, file.info.size));
            return false;
        }
        // compression::compress() checks by itself whether the body is compressible: only the
        // conditions it does not know are given here (one reading of the headers per answer)
        return file.encoding.empty() && file.info.size <= compression_cap;
    }

}

void send(response& res, const representation& file) {
    const auto& request = res.request_message();
    auto verb = request.get_method();
    // Conditional request for a file the client has: no body, nor the length of the file
    bool not_modified = (verb == method::GET || verb == method::HEAD) && file.info.not_modified(request);

    // The body (a byte range of it), read before the message takes the status and headers set:
    // an error answered instead keeps them
    auto range = requested_range(request, file.info, not_modified);
    if (range && range->length == 0) {
        auto unsatisfiable = res.new_response();
        if (!unsatisfiable) return;
        make_unsatisfiable(*unsatisfiable, file.info.size, res.formatter());
        res.send_message(std::move(unsatisfiable));
        return;
    }
    const auto [first, length] = range.value_or(byte_range{0, file.info.size});
    bool with_body = !not_modified && verb != method::HEAD;

    // A long body is streamed (see streamed()). A request dispatched in memory has no
    // connection to stream to: read whole
    auto connection = with_body && length > stream_threshold ? res.get_connection() : nullptr;
    if (connection && streamed(res.draft(), request, file, range.has_value())) {
        stream(res, res.draft(), file, range.has_value(), first, length, std::move(connection));
        return;
    }

    std::string content;
    if (with_body && !read_file(file.path, first, length, content)) {
        res.error(http_response::status::internal_server_error, "Failed to read file");
        return;
    }

    auto message = res.new_response();
    if (!message) return;
    bool compress = fill_read(*message, file, verb, not_modified, range.has_value(), first, length, std::move(content));
    res.send_message(std::move(message), compress);
}

} // namespace thinger::http::file_server

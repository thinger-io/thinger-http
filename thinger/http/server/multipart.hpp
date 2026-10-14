#ifndef THINGER_HTTP_SERVER_MULTIPART_HPP
#define THINGER_HTTP_SERVER_MULTIPART_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include "../common/multipart_parser.hpp"
#include "../../util/types.hpp"

namespace thinger::http {

    class request;

    /**
     * Reads a multipart body (multipart/form-data, or any other multipart type) part by part
     * as it arrives, like Go's multipart.Reader: the headers of each part, then its content in
     * pieces, never buffering more than its read buffer and the header section of a part.
     *
     *   http::multipart_reader reader(req);
     *   while (const http::multipart_part* part = co_await reader.next()) {
     *       for (auto data = co_await reader.read(); !data.empty(); data = co_await reader.read()) { ... }
     *   }
     *   if (reader.failed()) res.error(reader.status(), reader.message());
     *
     * The body is read with request::read_some(), within the maximum body size of the route
     * (body_too_large), or from req.body() if the server already read it (routes that are not
     * deferred). A body with a Content-Encoding is not decoded: not_multipart (415).
     */
    class multipart_reader {
    public:
        // Bytes read from the body at once
        static constexpr size_t default_buffer_size = 64 * 1024;

        /// Reader of the body of `req`, which must outlive it
        explicit multipart_reader(request& req, multipart_limits limits = {}, size_t buffer_size = default_buffer_size);

        /// The next part, skipping what is left of the current one; null at the end of the
        /// body, on error (see failed()), or if the read was cancelled. Valid until the next call.
        thinger::awaitable<const multipart_part*> next();

        /// The next piece of the content of the current part; empty at its end, on error or if
        /// the read was cancelled. Valid until the next call.
        thinger::awaitable<std::string_view> read();

        /// Append the rest of the content of the current part to `value` (bounded by the
        /// limits of the part); false on error or if the read was cancelled
        thinger::awaitable<bool> read_all(std::string& value);

        /// Media type of the body, lowercase ("multipart/form-data"), empty if not multipart
        const std::string& media_type() const { return media_type_; }

        /// The current part, null before the first one and after the last
        const multipart_part* part() const { return in_part_ ? &parser_->part() : nullptr; }

        /// The whole body was read, up to its closing delimiter (and its epilogue)
        bool done() const { return done_; }
        bool failed() const { return error_ != multipart_error::none; }
        multipart_error error() const { return error_; }

        /// Status and message answering the error (400, 413, 415 or 500)
        http_response::status status() const { return multipart_error_status(error_); }
        const char* message() const { return multipart_error_message(error_); }

    private:
        request& req_;
        std::optional<multipart_parser> parser_;
        std::string media_type_;
        std::unique_ptr<uint8_t[]> buffer_;
        size_t buffer_size_;
        std::string_view input_;
        multipart_error error_ = multipart_error::none;
        bool in_memory_ = false;    // the body was read by the server: parse req.body()
        bool finished_ = false;     // the whole body was read
        bool in_part_ = false;
        bool done_ = false;

        void fail(multipart_error error);

        /// The next event of the parser, reading the body as needed; need_more if a read was
        /// cancelled
        thinger::awaitable<multipart_parser::event> advance();

        /// Read more of the body into the input; false if the read was cancelled
        thinger::awaitable<bool> fill();
    };

    /**
     * A file of a multipart form, stored in a temporary file that is deleted with this object
     * (at the end of the request handler, usually) unless it is moved to its destination with
     * move_to() or kept with release().
     */
    class multipart_file {
    public:
        multipart_file(multipart_part part, std::filesystem::path path, size_t size);
        ~multipart_file();

        multipart_file(multipart_file&& other) noexcept;
        multipart_file& operator=(multipart_file&& other) noexcept;
        multipart_file(const multipart_file&) = delete;
        multipart_file& operator=(const multipart_file&) = delete;

        /// Field name
        const std::string& name() const { return part_.name; }
        /// Filename as the client sent it: never use it as a path (see safe_filename())
        const std::string& filename() const { return part_.filename; }
        /// The filename reduced to a name to store it with (see multipart_part::safe_filename())
        std::string safe_filename() const { return part_.safe_filename(); }
        /// Content-Type of the part ("text/plain" if it had none), as the client says
        const std::string& content_type() const { return part_.content_type; }
        /// Every header of the part
        const multipart_part& part() const { return part_; }

        /// Bytes of content
        size_t size() const { return size_; }

        /// The file with the content: the temporary file (readable by its owner only), or
        /// wherever move_to() moved it
        const std::filesystem::path& path() const { return path_; }

        /// Move the file to `destination` (renamed, or copied if it is in another file
        /// system), replacing any file there: it is no longer deleted. False on error.
        bool move_to(const std::filesystem::path& destination, std::error_code& error);
        bool move_to(const std::filesystem::path& destination);

        /// Keep the temporary file (no longer deleted), returning its path
        std::filesystem::path release();

    private:
        multipart_part part_;
        std::filesystem::path path_;
        size_t size_ = 0;
        bool owned_ = true;     // the temporary file is deleted with this object

        void remove();
    };

    // How request::read_multipart() reads a form
    struct multipart_options {
        multipart_limits limits;
        // Directory of the temporary files (the system one, std::filesystem::temp_directory_path(), if empty)
        std::filesystem::path temp_directory;
        // Handler of the file parts, instead of temporary files: it reads the content with
        // reader.read() (what it leaves unread is skipped) and stores it wherever it wants.
        // The form then has no files. What it throws ends the request (500).
        std::function<thinger::awaitable<void>(const multipart_part& part, multipart_reader& reader)> on_file;
    };

    class multipart_form;

    /// Read the multipart/form-data body of a request (see request::read_multipart())
    thinger::awaitable<multipart_form> read_multipart(request& req, multipart_options options = {});

    /**
     * A multipart/form-data body read whole (request::read_multipart()): its fields in memory
     * and its files in temporary files, deleted with the form. Parts without a name, or with a
     * disposition other than form-data, are skipped (as Go and busboy do).
     */
    class multipart_form {
    public:
        multipart_form() = default;
        multipart_form(multipart_form&&) noexcept = default;
        multipart_form& operator=(multipart_form&&) noexcept = default;

        /// Parts without filename: their names and values, in order
        const std::vector<std::pair<std::string, std::string>>& fields() const { return fields_; }

        /// Value of the first field with this name, null if none
        const std::string* field(std::string_view name) const;

        /// Value of the first field with this name, or `default_value`
        std::string field(std::string_view name, std::string_view default_value) const;

        /// Parts with a filename, in order
        std::vector<multipart_file>& files() { return files_; }
        const std::vector<multipart_file>& files() const { return files_; }

        /// The first file with this field name, null if none
        multipart_file* file(std::string_view name);

        /// Whether the whole body was read; if not, the error tells why, and the form is empty
        bool ok() const { return error_ == multipart_error::none; }
        explicit operator bool() const { return ok(); }
        multipart_error error() const { return error_; }
        http_response::status status() const { return multipart_error_status(error_); }
        const char* message() const { return multipart_error_message(error_); }

    private:
        std::vector<std::pair<std::string, std::string>> fields_;
        std::vector<multipart_file> files_;
        multipart_error error_ = multipart_error::none;

        friend thinger::awaitable<multipart_form> read_multipart(request& req, multipart_options options);
    };

}

#endif

#include "multipart.hpp"
#include "request.hpp"
#include "body_decoders.hpp"
#include "../../util/ascii.hpp"
#include "../../util/logger.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace thinger::http {

    // --- multipart_reader ---

    multipart_reader::multipart_reader(request& req, multipart_limits limits, size_t buffer_size)
        : req_(req), buffer_size_(std::max<size_t>(buffer_size, 1)) {
        auto http_request = req.get_http_request();
        if (!http_request) {
            fail(multipart_error::not_multipart);
            return;
        }

        // A single Content-Type of a multipart type
        const std::string* content_type = nullptr;
        for (const auto& [key, value] : http_request->get_headers()) {
            if (!headers::is_header(key, header::content_type)) continue;
            if (content_type && value != *content_type) {
                fail(multipart_error::not_multipart);
                return;
            }
            content_type = &value;
        }
        auto type = content_type ? media_type::parse(*content_type) : std::nullopt;
        if (!type || !type->type.starts_with("multipart/")) {
            fail(multipart_error::not_multipart);
            return;
        }
        media_type_ = type->type;

        // A compressed body would have to be decoded first (the server decodes the bodies it
        // reads, removing their Content-Encoding)
        if (http_request->has_header("Content-Encoding") &&
            !::thinger::util::ascii::iequals(http_request->get_header("Content-Encoding"), "identity")) {
            fail(multipart_error::not_multipart);
            return;
        }

        // Exactly one boundary: a repeated one is ambiguous
        std::string_view boundary;
        size_t boundaries = 0;
        for (const auto& [name, value] : type->parameters) {
            if (name != "boundary") continue;
            boundary = value;
            ++boundaries;
        }
        if (boundaries != 1) {
            fail(multipart_error::invalid_boundary);
            return;
        }
        parser_.emplace(boundary, limits);
        if (parser_->failed()) {
            fail(parser_->error());
            return;
        }

        // Body already read by the server (routes that are not deferred), or still to read
        if (req.get_body_error() != body_error::none) {
            fail(req.get_body_error() == body_error::too_large ? multipart_error::body_too_large
                                                               : multipart_error::invalid_body);
            return;
        }
        in_memory_ = !req.has_pending_body();
        if (!in_memory_) buffer_ = std::make_unique<uint8_t[]>(buffer_size_);
    }

    void multipart_reader::fail(multipart_error error) {
        if (error_ == multipart_error::none) error_ = error;
        in_part_ = false;
    }

    thinger::awaitable<bool> multipart_reader::fill() {
        if (in_memory_) {
            input_ = req_.get_http_request()->get_body();
            finished_ = true;
            co_return true;
        }
        size_t bytes = co_await req_.read_some(buffer_.get(), buffer_size_);
        if (bytes > 0) {
            input_ = std::string_view(reinterpret_cast<const char*>(buffer_.get()), bytes);
            co_return true;
        }
        auto error = req_.get_body_error();
        if (error == body_error::too_large) {
            fail(multipart_error::body_too_large);
        } else if (error != body_error::none) {
            fail(multipart_error::invalid_body);
        } else if (req_.has_pending_body()) {
            co_return false;    // cancelled: the body is still there
        } else {
            finished_ = true;
        }
        co_return true;
    }

    thinger::awaitable<multipart_parser::event> multipart_reader::advance() {
        using event = multipart_parser::event;
        for (;;) {
            if (error_ != multipart_error::none) co_return event::error;
            auto result = parser_->parse(input_);
            if (result == event::error) {
                fail(parser_->error());
                co_return result;
            }
            // The end once the epilogue is read too, so the whole body is read (and checked)
            if (result != event::need_more && (result != event::end || finished_)) co_return result;
            if (finished_) {
                result = parser_->finish();
                if (result == event::error) fail(parser_->error());
                co_return result;
            }
            if (!co_await fill()) co_return event::need_more;
        }
    }

    thinger::awaitable<const multipart_part*> multipart_reader::next() {
        using event = multipart_parser::event;
        if (!parser_ || error_ != multipart_error::none) co_return nullptr;
        for (;;) {
            switch (co_await advance()) {
                case event::part:
                    in_part_ = true;
                    co_return &parser_->part();
                case event::data:
                    continue;   // the rest of the current part, skipped
                case event::part_end:
                    in_part_ = false;
                    continue;
                case event::end:
                    in_part_ = false;
                    done_ = true;
                    co_return nullptr;
                default:
                    co_return nullptr;
            }
        }
    }

    thinger::awaitable<std::string_view> multipart_reader::read() {
        using event = multipart_parser::event;
        if (!in_part_ || error_ != multipart_error::none) co_return std::string_view{};
        auto result = co_await advance();
        if (result == event::data) co_return parser_->data();
        if (result == event::part_end) in_part_ = false;
        co_return std::string_view{};
    }

    thinger::awaitable<bool> multipart_reader::read_all(std::string& value) {
        while (in_part_) {
            auto data = co_await read();
            if (data.empty()) break;
            value.append(data);
        }
        co_return !in_part_ && error_ == multipart_error::none;
    }

    // --- multipart_file ---

    multipart_file::multipart_file(multipart_part part, std::filesystem::path path, size_t size)
        : part_(std::move(part)), path_(std::move(path)), size_(size) {}

    multipart_file::~multipart_file() {
        remove();
    }

    multipart_file::multipart_file(multipart_file&& other) noexcept
        : part_(std::move(other.part_)), path_(std::move(other.path_)), size_(other.size_), owned_(other.owned_) {
        other.owned_ = false;
    }

    multipart_file& multipart_file::operator=(multipart_file&& other) noexcept {
        if (this != &other) {
            remove();
            part_ = std::move(other.part_);
            path_ = std::move(other.path_);
            size_ = other.size_;
            owned_ = other.owned_;
            other.owned_ = false;
        }
        return *this;
    }

    void multipart_file::remove() {
        if (!owned_ || path_.empty()) return;
        owned_ = false;
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    bool multipart_file::move_to(const std::filesystem::path& destination, std::error_code& error) {
        error.clear();
        std::filesystem::rename(path_, destination, error);
        if (error == std::errc::cross_device_link) {
            error.clear();
            std::filesystem::copy_file(path_, destination, std::filesystem::copy_options::overwrite_existing, error);
            if (error) return false;
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        } else if (error) {
            return false;
        }
        path_ = destination;
        owned_ = false;
        return true;
    }

    bool multipart_file::move_to(const std::filesystem::path& destination) {
        std::error_code error;
        return move_to(destination, error);
    }

    std::filesystem::path multipart_file::release() {
        owned_ = false;
        return path_;
    }

    // --- multipart_form ---

    const std::string* multipart_form::field(std::string_view name) const {
        for (const auto& [key, value] : fields_) {
            if (key == name) return &value;
        }
        return nullptr;
    }

    std::string multipart_form::field(std::string_view name, std::string_view default_value) const {
        auto value = field(name);
        return value ? *value : std::string(default_value);
    }

    multipart_file* multipart_form::file(std::string_view name) {
        for (auto& file : files_) {
            if (file.name() == name) return &file;
        }
        return nullptr;
    }

    namespace {

        // A temporary file being written, deleted unless released
        class temporary_file {
        public:
            explicit temporary_file(const std::filesystem::path& directory) {
                std::string pattern = (directory / "thinger-upload-XXXXXX").string();
                // (readable by its owner only, created exclusively)
                fd_ = ::mkstemp(pattern.data());
                if (fd_ >= 0) path_ = std::move(pattern);
            }

            ~temporary_file() {
                close();
                if (!path_.empty()) ::unlink(path_.c_str());
            }

            temporary_file(const temporary_file&) = delete;
            temporary_file& operator=(const temporary_file&) = delete;

            bool is_open() const { return fd_ >= 0; }

            bool write(std::string_view data) {
                while (!data.empty()) {
                    auto written = ::write(fd_, data.data(), data.size());
                    if (written < 0) {
                        if (errno == EINTR) continue;
                        return false;
                    }
                    data.remove_prefix(static_cast<size_t>(written));
                }
                return true;
            }

            bool close() {
                if (fd_ < 0) return true;
                bool closed = ::close(fd_) == 0;
                fd_ = -1;
                return closed;
            }

            // The file, no longer deleted
            std::string release() {
                return std::exchange(path_, {});
            }

        private:
            int fd_ = -1;
            std::string path_;
        };

    }

    thinger::awaitable<multipart_form> read_multipart(request& req, multipart_options options) {
        multipart_form form;
        multipart_reader reader(req, options.limits);
        if (!reader.failed() && reader.media_type() != "multipart/form-data") {
            form.error_ = multipart_error::not_multipart;
            co_return form;
        }

        std::filesystem::path directory = options.temp_directory;
        if (directory.empty()) {
            std::error_code error;
            directory = std::filesystem::temp_directory_path(error);
            if (error) directory = "/tmp";
        }

        multipart_error failure = multipart_error::none;
        while (const multipart_part* part = co_await reader.next()) {
            // Parts that are not form fields are skipped, as Go and busboy do
            if (part->disposition != "form-data" || part->name.empty()) continue;

            if (!part->is_file()) {
                std::string value;
                if (!co_await reader.read_all(value)) break;
                form.fields_.emplace_back(part->name, std::move(value));
                continue;
            }

            if (options.on_file) {
                co_await options.on_file(*part, reader);
                continue;
            }

            // (blocking writes, as Go's ParseMultipartForm: there is no portable asynchronous
            // file I/O, and the parts arrive slower than a disk takes them)
            temporary_file file(directory);
            if (!file.is_open()) {
                LOG_ERROR("Cannot create a temporary file for an upload in {}: {}", directory.string(), std::strerror(errno));
                failure = multipart_error::file_failed;
                break;
            }
            size_t size = 0;
            bool written = true;
            for (auto data = co_await reader.read(); !data.empty(); data = co_await reader.read()) {
                if (!file.write(data)) {
                    written = false;
                    break;
                }
                size += data.size();
            }
            if (!written || !file.close()) {
                LOG_ERROR("Cannot write the temporary file of an upload: {}", std::strerror(errno));
                failure = multipart_error::file_failed;
                break;
            }
            if (reader.failed()) break;
            form.files_.emplace_back(*part, file.release(), size);
        }

        if (failure == multipart_error::none && !reader.done()) {
            // An error, or a read cancelled: the body was not read whole
            failure = reader.failed() ? reader.error() : multipart_error::truncated;
        }
        if (failure != multipart_error::none) {
            form.error_ = failure;
            form.fields_.clear();
            form.files_.clear();
        }
        co_return form;
    }

}

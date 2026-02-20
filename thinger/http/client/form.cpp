#include "form.hpp"
#include "../server/mime_types.hpp"
#include "../util/url.hpp"
#include <fstream>
#include <sstream>
#include <random>
#include <algorithm>

namespace thinger::http {

// ============================================
// Field methods
// ============================================

form& form::field(std::string name, std::string value) {
    fields_.emplace_back(std::move(name), std::move(value));
    return *this;
}

form& form::fields(std::initializer_list<std::pair<std::string, std::string>> pairs) {
    for (const auto& [name, value] : pairs) {
        fields_.emplace_back(name, value);
    }
    return *this;
}

// ============================================
// File methods
// ============================================

form& form::file(std::string name, const std::filesystem::path& path) {
    return file(std::move(name), path, mime_type(path));
}

form& form::file(std::string name, const std::filesystem::path& path,
                 std::string content_type) {
    // Read file content
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return *this;
    }

    std::ostringstream oss;
    oss << ifs.rdbuf();

    files_.push_back({
        std::move(name),
        path.filename().string(),
        std::move(content_type),
        oss.str()
    });

    return *this;
}

form& form::file(std::string name, std::string content,
                 std::string filename, std::string content_type) {
    files_.push_back({
        std::move(name),
        std::move(filename),
        std::move(content_type),
        std::move(content)
    });
    return *this;
}

form& form::file(std::string name, const void* data, size_t size,
                 std::string filename, std::string content_type) {
    files_.push_back({
        std::move(name),
        std::move(filename),
        std::move(content_type),
        std::string(static_cast<const char*>(data), size)
    });
    return *this;
}

// ============================================
// Content-Type and body generation
// ============================================

std::string form::content_type() const {
    if (is_multipart()) {
        return "multipart/form-data; boundary=" + get_boundary();
    }
    return "application/x-www-form-urlencoded";
}

std::string form::body() const {
    if (is_multipart()) {
        return build_multipart();
    }
    return build_urlencoded();
}

std::string form::build_urlencoded() const {
    std::string result;
    for (const auto& [name, value] : fields_) {
        if (!result.empty()) {
            result += '&';
        }
        result += url_encode(name) + '=' + url_encode(value);
    }
    return result;
}

std::string form::build_multipart() const {
    const std::string& boundary = get_boundary();
    std::ostringstream oss;

    // Add text fields
    for (const auto& [name, value] : fields_) {
        oss << "--" << boundary << "\r\n";
        oss << "Content-Disposition: form-data; name=\"" << name << "\"\r\n";
        oss << "\r\n";
        oss << value << "\r\n";
    }

    // Add files
    for (const auto& file : files_) {
        oss << "--" << boundary << "\r\n";
        oss << "Content-Disposition: form-data; name=\"" << file.name << "\"; "
            << "filename=\"" << file.filename << "\"\r\n";
        oss << "Content-Type: " << file.content_type << "\r\n";
        oss << "\r\n";
        oss << file.content << "\r\n";
    }

    // Closing boundary
    oss << "--" << boundary << "--\r\n";

    return oss.str();
}

const std::string& form::get_boundary() const {
    if (boundary_.empty()) {
        boundary_ = generate_boundary();
    }
    return boundary_;
}

std::string form::generate_boundary() {
    static const char chars[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, sizeof(chars) - 2);

    std::string boundary = "----ThingerFormBoundary";
    for (int i = 0; i < 16; ++i) {
        boundary += chars[dis(gen)];
    }

    return boundary;
}

// ============================================
// URL encoding/decoding (delegates to util::url)
// ============================================

std::string form::url_encode(const std::string& str) {
    // application/x-www-form-urlencoded uses '+' for space instead of '%20'
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(str.size());
    for (unsigned char c : str) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            result += static_cast<char>(c);
        } else if (c == ' ') {
            result += '+';
        } else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 0x0F];
        }
    }
    return result;
}

std::string form::url_decode(const std::string& str) {
    return util::url::url_decode(str);
}

// ============================================
// MIME type detection
// ============================================

std::string form::mime_type(const std::filesystem::path& path) {
    // Use the library's comprehensive MIME type lookup
    const std::string& type = mime_types::extension_to_type(path.extension().string());

    // mime_types returns text/plain for unknown, but for form uploads
    // application/octet-stream is more appropriate
    if (type == mime_types::text_plain && !path.extension().empty()) {
        return mime_types::application_octect_stream;
    }

    return type;
}

} // namespace thinger::http

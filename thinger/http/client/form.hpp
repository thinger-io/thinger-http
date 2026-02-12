#ifndef THINGER_HTTP_CLIENT_FORM_HPP
#define THINGER_HTTP_CLIENT_FORM_HPP

#include <string>
#include <vector>
#include <filesystem>
#include <optional>

namespace thinger::http {

/**
 * HTTP form builder for POST requests.
 * Supports both URL-encoded and multipart form data.
 *
 * Usage:
 *   // Simple form (auto URL-encoded)
 *   http::form form;
 *   form.field("username", "john")
 *       .field("password", "secret");
 *   auto res = client.post(url, form);
 *
 *   // With files (auto multipart)
 *   http::form form;
 *   form.field("name", "John")
 *       .file("avatar", "/path/to/photo.jpg")
 *       .file("data", buffer, "data.bin", "application/octet-stream");
 *   auto res = client.post(url, form);
 */
class form {
public:
    form() = default;

    // ============================================
    // Field methods (chainable)
    // ============================================

    /**
     * Add a text field to the form.
     */
    form& field(std::string name, std::string value);

    /**
     * Add multiple fields at once.
     */
    form& fields(std::initializer_list<std::pair<std::string, std::string>> pairs);

    // ============================================
    // File methods (chainable)
    // ============================================

    /**
     * Add a file from filesystem path.
     * Content-type is auto-detected from extension.
     */
    form& file(std::string name, const std::filesystem::path& path);

    /**
     * Add a file from filesystem path with explicit content type.
     */
    form& file(std::string name, const std::filesystem::path& path,
               std::string content_type);

    /**
     * Add a file from memory buffer.
     */
    form& file(std::string name, std::string content,
               std::string filename,
               std::string content_type = "application/octet-stream");

    /**
     * Add a file from memory buffer (binary data).
     */
    form& file(std::string name, const void* data, size_t size,
               std::string filename,
               std::string content_type = "application/octet-stream");

    // ============================================
    // Query methods
    // ============================================

    /**
     * Returns true if form has files (will use multipart encoding).
     */
    bool is_multipart() const { return !files_.empty(); }

    /**
     * Returns true if form is empty.
     */
    bool empty() const { return fields_.empty() && files_.empty(); }

    /**
     * Get the appropriate Content-Type header value.
     */
    std::string content_type() const;

    /**
     * Build the request body.
     */
    std::string body() const;

    // ============================================
    // Static utilities
    // ============================================

    /**
     * URL-encode a string.
     */
    static std::string url_encode(const std::string& str);

    /**
     * URL-decode a string.
     */
    static std::string url_decode(const std::string& str);

    /**
     * Guess MIME type from file extension.
     */
    static std::string mime_type(const std::filesystem::path& path);

private:
    struct file_entry {
        std::string name;           // Form field name
        std::string filename;       // Original filename
        std::string content_type;   // MIME type
        std::string content;        // File content
    };

    std::vector<std::pair<std::string, std::string>> fields_;
    std::vector<file_entry> files_;
    mutable std::string boundary_;  // Generated on first use

    std::string build_urlencoded() const;
    std::string build_multipart() const;
    const std::string& get_boundary() const;
    static std::string generate_boundary();
};

} // namespace thinger::http

#endif

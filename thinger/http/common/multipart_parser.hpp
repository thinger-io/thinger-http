#ifndef THINGER_HTTP_COMMON_MULTIPART_PARSER_HPP
#define THINGER_HTTP_COMMON_MULTIPART_PARSER_HPP

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "http_response.hpp"

namespace thinger::http {

    // Limits of a multipart body, against bodies that are small for the maximum body size
    // but expensive to process: many parts, large header sections, fields kept in memory.
    // Content sizes count the bytes of the parts, without delimiters nor headers; the whole
    // body is bounded by the maximum body size of the route.
    struct multipart_limits {
        // Parts in the body, fields and files (Go: 1000)
        size_t max_parts = 1000;
        // Parts with a filename (busboy and Go: unlimited)
        size_t max_files = std::numeric_limits<size_t>::max();
        // Header section of each part, line endings included (busboy: 16 KB)
        size_t max_header_size = 16 * 1024;
        // Filename of a part, in bytes, as sent (decoded from filename* if present): no
        // longer than a file name can be in most file systems
        size_t max_filename_size = 255;
        // Content of each part without filename (busboy: 1 MB)
        size_t max_field_size = 1024 * 1024;
        // Content of all the parts without filename, which are usually kept in memory
        size_t max_fields_size = 8 * 1024 * 1024;
        // Content of each part with a filename (busboy and Go: unlimited)
        size_t max_file_size = std::numeric_limits<size_t>::max();
    };

    // Why a multipart body failed
    enum class multipart_error {
        none,
        not_multipart,          // 415: the Content-Type is not multipart (or the body has a Content-Encoding)
        invalid_boundary,       // 400: the boundary parameter is missing or invalid
        malformed,              // 400: the body does not follow the multipart syntax
        truncated,              // 400: the body ended before its closing delimiter
        invalid_body,           // 400: the request body could not be read (framing, connection closed)
        too_many_parts,         // 413
        too_many_files,         // 413
        header_too_large,       // 413: the header section of a part
        filename_too_large,     // 413
        field_too_large,        // 413: a part without filename
        fields_too_large,       // 413: all the parts without filename
        file_too_large,         // 413: a part with a filename
        body_too_large,         // 413: over the maximum body size of the route
        file_failed             // 500: a temporary file could not be written
    };

    // Status answering an error (400, 413, 415 or 500), and a message for the client
    http_response::status multipart_error_status(multipart_error error);
    const char* multipart_error_message(multipart_error error);

    // A part of a multipart body: its header section, and what its Content-Disposition and
    // Content-Type say (RFC 7578)
    struct multipart_part {
        // Header fields as received (names keep their case; values without surrounding whitespace)
        std::vector<std::pair<std::string, std::string>> headers;

        // Disposition type, lowercase ("form-data"); empty without Content-Disposition
        std::string disposition;
        // Field name (the name parameter), empty if none
        std::string name;
        // Filename as the client sent it, from filename* (RFC 5987, if its charset is UTF-8,
        // ISO-8859-1 or US-ASCII) or else filename. It may be a path ("../../etc/passwd",
        // "C:\\Users\\me\\photo.jpg") or have control characters: never use it as a path,
        // see safe_filename()
        std::string filename;
        // Whether the part has a filename parameter (even an empty one, as browsers send for
        // a file input without a file): a file, not a field
        bool has_filename = false;
        // Content-Type of the part, "text/plain" if it has none (RFC 7578, 4.4)
        std::string content_type = "text/plain";

        bool is_file() const { return has_filename; }

        // The value of a header (the first one with that name, case-insensitive), empty if missing
        std::string header(std::string_view name) const;

        // The filename reduced to a name to store it with: what follows its last '/' or
        // '\', without control characters; empty for "." and ".." (and an empty filename).
        // Still, it can collide with existing files, start with a dot, or be long.
        std::string safe_filename() const { return sanitize_filename(filename); }

        static std::string sanitize_filename(std::string_view filename);
    };

    /**
     * Incremental parser of a multipart body (RFC 2046, section 5.1; RFC 7578 for
     * multipart/form-data). It is fed the body in pieces of any size and returns the events
     * they make, without keeping more than the header section of a part and a partial
     * delimiter: the content of the parts is handed over as it is found, in pieces pointing
     * into the input (or into the parser, for bytes held back as a possible delimiter).
     *
     * The result does not depend on how the body is split. It is strict as Go's
     * mime/multipart: CRLF line endings only, transport padding (spaces and tabs) after
     * delimiters, a closing delimiter required, preamble and epilogue ignored, no header
     * folding. A part can start with its delimiter right after its header section (an empty
     * part), as Go reads it. The boundary inside the content of a part is not a delimiter
     * unless it starts a line and is followed by "--", whitespace or CRLF.
     */
    class multipart_parser {
    public:
        enum class event {
            need_more,      // all the input was consumed: feed more, or finish()
            part,           // a part starts: its header section is in part()
            data,           // a piece of the content of the current part, in data()
            part_end,       // the current part ended
            end,            // the closing delimiter was read (the rest of the body is the epilogue)
            error           // the body is invalid, or over a limit: see error()
        };

        // RFC 2046: 1 to 70 characters, digits, letters and '()+_,-./:=? (and spaces, but not last)
        static bool valid_boundary(std::string_view boundary);

        // A parser for a body with this boundary (as the boundary parameter of the
        // Content-Type gives it). With an invalid boundary, it fails at once (invalid_boundary).
        explicit multipart_parser(std::string_view boundary, multipart_limits limits = {});

        /// Parse `input` up to the next event, removing what it consumed from its front.
        /// After `end` (and `error`) the same event is returned again, and the rest of the
        /// input (the epilogue) is consumed.
        event parse(std::string_view& input);

        /// The body ended: `end` if its closing delimiter was read, else `error` (truncated)
        event finish();

        /// Content of a `data` event, valid until the next call
        std::string_view data() const { return data_; }

        /// The current part, from its `part` event to the next one
        const multipart_part& part() const { return part_; }

        multipart_error error() const { return error_; }
        bool done() const { return state_ == state::done; }
        bool failed() const { return state_ == state::error; }

        /// Parts started, and bytes of content of the current part
        size_t parts() const { return parts_; }
        size_t part_size() const { return part_size_; }

    private:
        enum class state {
            preamble,           // before the first delimiter
            delimiter_line,     // transport padding after a delimiter, then CRLF
            delimiter_lf,
            headers,            // header section of a part
            content,            // content of a part
            close_line,         // transport padding after the closing delimiter, then CRLF or the end
            close_lf,
            done,               // the epilogue
            error
        };

        enum class match { mismatch, partial, delimiter, close };

        multipart_limits limits_;
        // CRLF "--" boundary "--": the delimiter, and the two dashes of the closing one
        std::string delimiter_;
        size_t boundary_end_ = 0;       // length of CRLF "--" boundary
        state state_ = state::preamble;
        multipart_error error_ = multipart_error::none;

        // Bytes of delimiter_ matched by the input already consumed (held back), from held_from_:
        // the parts and the body start as if after a CRLF, which is not content
        size_t matched_ = 0;
        size_t held_from_ = 0;

        // Header section of the next part: the line being read, its fields, and its size
        std::string line_;
        std::vector<std::pair<std::string, std::string>> headers_;
        size_t header_size_ = 0;

        multipart_part part_;
        std::string_view data_;
        size_t parts_ = 0;
        size_t files_ = 0;
        size_t part_size_ = 0;
        size_t fields_size_ = 0;

        event fail(multipart_error error);
        void start_matching();
        match match_delimiter(size_t& matched, std::string_view input, size_t& used) const;
        event scan(std::string_view& input);
        event on_delimiter(match found);
        event header_line(std::string_view line);
        event header_section_end();
        event content(std::string_view data);
    };

}

#endif

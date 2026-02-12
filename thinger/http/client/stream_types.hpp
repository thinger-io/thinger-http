#ifndef THINGER_HTTP_CLIENT_STREAM_TYPES_HPP
#define THINGER_HTTP_CLIENT_STREAM_TYPES_HPP

#include <string>
#include <string_view>
#include <functional>
#include <map>

namespace thinger::http {

/**
 * Information passed to stream callbacks for each chunk of data.
 */
struct stream_info {
    std::string_view data;      // Current chunk data
    size_t downloaded;          // Total bytes downloaded so far
    size_t total;               // Total expected size (0 if unknown, e.g., chunked)
    int status_code;            // HTTP status code
};

/**
 * Result of a streaming operation.
 */
struct stream_result {
    int status_code = 0;
    std::string error;                              // Empty if no connection error
    std::map<std::string, std::string> headers;     // Response headers
    size_t bytes_transferred = 0;

    /**
     * Returns true if the request succeeded (no error and 2xx status).
     */
    bool ok() const {
        return error.empty() && status_code >= 200 && status_code < 300;
    }

    /**
     * Conversion to bool for if(result) checks.
     */
    explicit operator bool() const { return ok(); }

    /**
     * Returns true if the request completed (even if status is not 2xx).
     * Use this to distinguish between network errors and HTTP errors.
     */
    bool completed() const { return error.empty() && status_code > 0; }

    /**
     * Returns true if there was a network/connection error.
     */
    bool has_network_error() const { return !error.empty(); }

    /**
     * Returns true if the server returned an error status (4xx or 5xx).
     */
    bool has_http_error() const {
        return error.empty() && status_code >= 400;
    }
};

/**
 * Callback for streaming data.
 * Called for each chunk of data received.
 * Return true to continue, false to abort the download.
 */
using stream_callback = std::function<bool(const stream_info&)>;

/**
 * Callback for download progress.
 * @param downloaded Bytes downloaded so far
 * @param total Total bytes expected (0 if unknown)
 */
using progress_callback = std::function<void(size_t downloaded, size_t total)>;

} // namespace thinger::http

#endif // THINGER_HTTP_CLIENT_STREAM_TYPES_HPP

#ifndef HTTP_STREAM_HPP
#define HTTP_STREAM_HPP

#include <atomic>
#include <queue>
#include <memory>
#include <functional>
#include <string>
#include <utility>
#include "../common/http_frame.hpp"

namespace thinger::asio {
    class socket;
}

namespace thinger::http {

    typedef uint32_t stream_id;

    // Receives a connection taken over from the HTTP server: the socket, and any bytes the
    // server had already read past the request that took it over
    using takeover_handler = std::function<void(std::shared_ptr<asio::socket>, std::string buffered)>;

    /**
     * A HTTP stream represents a communication channel inside a single HTTP connection. As a
     * single connection can be used for multiple HTTP request, each request will generate a new
     * HTTP stream. In HTTP 1.1, all requests in a connection must be answered in order, even if
     * the responses are generated in a different order. In HTTP 2.0, it is possible to answer
     * asynchronously to each single stream within a connection using a stream identifier.
     * TODO check HTTP 2.0 support.
     */
    class http_stream {

    private:
        /**
         * Unique identifier within a http connection, generate for each request
         */
        stream_id stream_id_;

        /**
         * Queue for each HTTP frame composing a response. A response can be composed on several frames
         * i.e., while sending large files
         */
        std::queue<std::shared_ptr<http_frame>> queue_;

        /**
         * Set when the response takes over the connection: receives the socket, and the
         * bytes read past the request, once the response is written and the read loop
         * stopped at this stream
         */
        takeover_handler takeover_;
        std::string takeover_buffer_;
        bool takes_over_ = false;
        bool takeover_response_written_ = false;
        bool takeover_reader_stopped_ = false;

        /**
         * Frames posted to the connection thread and not queued yet (frames are queued in
         * call order: none runs inline while one is posted)
         */
        std::atomic<unsigned> posted_frames_{0};

        /**
         * Whether the response has started: a frame was queued, or a takeover requested
         */
        bool responded_ = false;

        bool keep_alive_;

    public:
        http_stream(stream_id stream_id, bool keep_alive) : stream_id_(stream_id), keep_alive_(keep_alive) {}

        virtual ~http_stream() {}

    public:

        size_t get_queue_size() const;

        bool empty_queue() const;

        std::shared_ptr<http_frame> current_frame() const;

        void pop_frame();

        void add_frame(std::shared_ptr<http_frame> frame);

        size_t get_queued_frames() const;

        bool responded() const {
            return responded_;
        }

        /**
         * Take over the connection once the response of this stream is written
         */
        void set_takeover(takeover_handler handler) {
            takeover_ = std::move(handler);
            takes_over_ = true;
            responded_ = true;
        }

        bool takes_over() const {
            return takes_over_;
        }

        /**
         * What the takeover waits for, in either order: the response written, and the read
         * loop stopped (with the bytes it read past the request)
         */
        void takeover_response_written() {
            takeover_response_written_ = true;
        }

        void takeover_reader_stopped(std::string buffered) {
            takeover_buffer_ = std::move(buffered);
            takeover_reader_stopped_ = true;
        }

        /**
         * Handler of the takeover once both are done, and the bytes read past the request;
         * the handler is empty before, and after it was released once
         */
        std::pair<takeover_handler, std::string> release_takeover() {
            if (!takeover_response_written_ || !takeover_reader_stopped_) return {};
            return {std::exchange(takeover_, nullptr), std::move(takeover_buffer_)};
        }

        void frame_posted() {
            ++posted_frames_;
        }

        void posted_frame_queued() {
            --posted_frames_;
        }

        bool has_posted_frames() const {
            return posted_frames_ > 0;
        }

        stream_id id() const;

        bool keep_alive() const {
            return keep_alive_;
        }

        void set_keep_alive(bool keep_alive) {
            keep_alive_ = keep_alive;
        }
    };

}

#endif
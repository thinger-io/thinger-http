#ifndef HTTP_STREAM_HPP
#define HTTP_STREAM_HPP

#include <queue>
#include <memory>
#include <functional>
#include "../common/http_frame.hpp"

namespace thinger::http {

    typedef uint32_t stream_id;

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
         * Callback to be able to register a function when the stream was completed, i.e., completed a
         * response to a given query.
         */
        std::function<void()> stream_callback_;

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

        void on_completed(std::function<void()> callback);

        void completed();

        stream_id id() const;

        bool keep_alive() const {
            return keep_alive_;
        }
    };

}

#endif
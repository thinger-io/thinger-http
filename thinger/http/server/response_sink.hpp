#ifndef THINGER_HTTP_SERVER_RESPONSE_SINK_HPP
#define THINGER_HTTP_SERVER_RESPONSE_SINK_HPP

#include "../common/http_response.hpp"
#include "server_connection.hpp"
#include "http_stream.hpp"
#include <memory>
#include <string>

namespace thinger::http {

// Where a response is written: a connection, or memory for requests dispatched in memory.
// Shared by every copy of a response, which writes it once: whole (send), in chunks
// (begin, append..., finish), or followed by a connection takeover (take_over).
class response_sink {
public:
    virtual ~response_sink() = default;

    // Whole response
    virtual void send(std::shared_ptr<http_response> response) = 0;

    // Chunked response: the headers, then the data, then the end. They return false if
    // the response can no longer be written (e.g. the connection is gone)
    virtual bool begin(std::shared_ptr<http_response> headers) = 0;
    virtual bool append(const std::string& data) = 0;
    virtual bool finish() = 0;

    // Whether the connection can be taken over (WebSocket, SSE, response::take_over)
    virtual bool can_take_over() const { return false; }

    // Send the response, then hand the connection to `handler` (see response::take_over)
    virtual void take_over(std::shared_ptr<http_response> /*response*/, takeover_handler /*handler*/) {}

    // Connection the response is written to, null if none
    virtual std::shared_ptr<server_connection> connection() const { return nullptr; }
};

// Response written to a stream of a server connection
class connection_sink : public response_sink {
public:
    connection_sink(const std::shared_ptr<server_connection>& connection, const std::shared_ptr<http_stream>& stream)
        : connection_(connection), stream_(stream) {}

    void send(std::shared_ptr<http_response> response) override;
    bool begin(std::shared_ptr<http_response> headers) override;
    bool append(const std::string& data) override;
    bool finish() override;
    bool can_take_over() const override { return true; }
    void take_over(std::shared_ptr<http_response> response, takeover_handler handler) override;
    std::shared_ptr<server_connection> connection() const override { return connection_.lock(); }

private:
    // Queue a frame on the stream; false if the connection is gone
    bool write(std::shared_ptr<http_frame> frame);

    std::weak_ptr<server_connection> connection_;
    std::weak_ptr<http_stream> stream_;
};

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_RESPONSE_SINK_HPP

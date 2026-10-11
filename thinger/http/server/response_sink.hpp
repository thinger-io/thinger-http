#ifndef THINGER_HTTP_SERVER_RESPONSE_SINK_HPP
#define THINGER_HTTP_SERVER_RESPONSE_SINK_HPP

#include "../common/http_response.hpp"
#include "../../util/types.hpp"
#include "server_connection.hpp"
#include "http_stream.hpp"
#include <memory>
#include <string>

namespace thinger::http {

class request;

// How the body of a streamed response is delimited (see response::start_stream)
enum class body_framing {
    none,       // no body (a response to HEAD, 1xx, 204, 304): the head alone
    length,     // Content-Length, already set in the head
    chunked,    // chunked transfer coding
    close       // closing the connection at its end (HTTP/1.0 clients)
};

// Where a response is written: a connection, or memory for requests dispatched in memory.
// Shared by every copy of a response, which writes it once: whole (send), in chunks
// (begin, append..., finish), streamed with flow control (begin_stream, write_stream...,
// end_stream or abort_stream), or followed by a connection takeover (take_over).
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

    // Streamed response: the head (its framing headers are set by the sink), then the
    // body, each write completing once its data is written (false if it could not be),
    // then the end; or an abort, ending the response so the client sees it incomplete.
    // By default they are the chunked response above, collected at once.
    virtual bool begin_stream(std::shared_ptr<http_response> head, body_framing /*framing*/) {
        return begin(std::move(head));
    }
    virtual awaitable<bool> write_stream(std::string data) {
        co_return append(data);
    }
    virtual awaitable<bool> end_stream() {
        co_return finish();
    }
    virtual void abort_stream() {
        finish();
    }

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

    // For a request read from a connection: the connection and stream it came from (copied as
    // the request holds them, without locking them)
    explicit connection_sink(const request& req);

    void send(std::shared_ptr<http_response> response) override;
    bool begin(std::shared_ptr<http_response> headers) override;
    bool append(const std::string& data) override;
    bool finish() override;
    bool begin_stream(std::shared_ptr<http_response> head, body_framing framing) override;
    awaitable<bool> write_stream(std::string data) override;
    awaitable<bool> end_stream() override;
    void abort_stream() override;
    bool can_take_over() const override { return true; }
    void take_over(std::shared_ptr<http_response> response, takeover_handler handler) override;
    std::shared_ptr<server_connection> connection() const override { return connection_.lock(); }

private:
    // Queue a frame on the stream; false if the connection is gone
    bool write(std::shared_ptr<http_frame> frame);

    // Queue a frame and wait until it is written; false if it could not be
    awaitable<bool> write_and_wait(std::shared_ptr<http_frame> frame);

    // Close the connection once this response is written
    void close_after_response();

    std::weak_ptr<server_connection> connection_;
    std::weak_ptr<http_stream> stream_;

    // Framing of a streamed response
    body_framing framing_ = body_framing::none;
};

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_RESPONSE_SINK_HPP

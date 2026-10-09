#include "response_sink.hpp"
#include "../common/http_data.hpp"
#include "../data/out_chunk.hpp"
#include "../../util/logger.hpp"

namespace thinger::http {

bool connection_sink::write(std::shared_ptr<http_frame> frame) {
    auto connection = connection_.lock();
    auto stream = stream_.lock();
    if (!connection || !stream) {
        LOG_DEBUG("Connection lost while writing a response");
        return false;
    }
    connection->handle_stream(stream, std::move(frame));
    return true;
}

void connection_sink::send(std::shared_ptr<http_response> response) {
    write(std::move(response));
}

bool connection_sink::begin(std::shared_ptr<http_response> headers) {
    headers->add_header("Transfer-Encoding", "chunked");
    headers->add_header("X-Content-Type-Options", "nosniff");
    headers->set_last_frame(false);   // the stream stays open for the chunks
    return write(std::move(headers));
}

bool connection_sink::append(const std::string& data) {
    auto chunk = std::make_shared<http_data>(std::make_shared<data::out_chunk>(data));
    chunk->set_last_frame(false);
    return write(std::move(chunk));
}

bool connection_sink::finish() {
    // Final zero-length chunk
    auto chunk = std::make_shared<http_data>(std::make_shared<data::out_chunk>());
    chunk->set_last_frame(true);
    return write(std::move(chunk));
}

void connection_sink::take_over(std::shared_ptr<http_response> response, takeover_handler handler) {
    auto connection = connection_.lock();
    auto stream = stream_.lock();
    if (!connection || !stream) {
        LOG_ERROR("Cannot take over a closed connection");
        return;
    }

    // Stop the connection from reading further requests, then hand the socket over
    // once this response has been written
    connection->begin_takeover(std::move(handler));
    stream->on_completed([connection]() {
        connection->takeover_response_sent();
    });
    connection->handle_stream(stream, std::move(response));
}

} // namespace thinger::http

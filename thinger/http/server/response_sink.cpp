#include "response_sink.hpp"
#include "request.hpp"
#include "../common/http_data.hpp"
#include "../data/out_chunk.hpp"
#include "../data/out_string.hpp"
#include "../../asio/async_event.hpp"
#include "../../util/logger.hpp"
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>

namespace thinger::http {

connection_sink::connection_sink(const request& req)
    : connection_(req.source_ ? req.source_->connection() : std::weak_ptr<server_connection>{}),
      stream_(req.http_stream_) {}

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

// Streamed response: each frame is awaited until the connection writes it, so the body
// goes at the pace of the client
bool connection_sink::begin_stream(std::shared_ptr<http_response> head, body_framing framing) {
    framing_ = framing;
    if (framing == body_framing::chunked) {
        head->set_header(header::transfer_encoding, "chunked");
    } else if (framing == body_framing::close) {
        must_close(detail::close_reason::framing_by_close);
    }
    head->set_last_frame(false);   // the stream stays open for the body and its end
    return write(std::move(head));
}

awaitable<bool> connection_sink::write_stream(std::string data) {
    std::shared_ptr<data::out_data> body;
    if (framing_ == body_framing::chunked) {
        body = std::make_shared<data::out_chunk>(std::move(data));
    } else {
        auto raw = std::make_shared<data::out_string>();
        raw->get_string() = std::move(data);
        body = std::move(raw);
    }
    auto frame = std::make_shared<http_data>(std::move(body));
    frame->set_last_frame(false);
    co_return co_await write_and_wait(std::move(frame));
}

awaitable<bool> connection_sink::end_stream() {
    // The last chunk of a chunked body; nothing for the others, whose end is known
    auto frame = framing_ == body_framing::chunked ? std::make_shared<http_data>(std::make_shared<data::out_chunk>())
                                                   : std::make_shared<http_data>();
    frame->set_last_frame(true);
    co_return co_await write_and_wait(std::move(frame));
}

void connection_sink::abort_stream() {
    // End the stream without ending the body (no last chunk, or fewer bytes than its
    // Content-Length) and close the connection: the client sees the body incomplete
    auto connection = connection_.lock();
    if (auto stream = stream_.lock(); connection && stream && stream->observer()) stream->observer()->abort();
    must_close(detail::close_reason::aborted);
    auto frame = std::make_shared<http_data>();
    frame->set_last_frame(true);
    write(std::move(frame));
}

void connection_sink::must_close(detail::close_reason reason) {
    auto connection = connection_.lock();
    auto stream = stream_.lock();
    if (!connection || !stream) return;
    // On the connection thread, which owns the stream, and before any frame queued after this
    // call (frames from other threads are posted too)
    boost::asio::dispatch(connection->get_socket()->get_io_context(), [stream, reason] {
        stream->must_close(reason);
    });
}

awaitable<bool> connection_sink::write_and_wait(std::shared_ptr<http_frame> frame) {
    // The writer waits on an event that the connection sets once it wrote (or dropped) the
    // frame: the wait belongs to the io_context like any other operation, so it can be
    // cancelled (e.g. the frame waits behind another response that never ends) and is
    // released with the io_context
    struct waiter {
        explicit waiter(const boost::asio::any_io_executor& executor) : executor(executor), done(executor) {}
        boost::asio::any_io_executor executor;
        asio::async_event done;
        bool written = false;
    };
    auto state = std::make_shared<waiter>(co_await boost::asio::this_coro::executor);
    frame->on_written([weak = std::weak_ptr<waiter>(state)](bool written) {
        auto waiting = weak.lock();
        if (!waiting) return;   // the writer is gone
        // on the executor of the writer, which owns the event (never inline within the
        // connection write loop)
        boost::asio::post(waiting->executor, [weak, written] {
            auto state = weak.lock();
            if (!state) return;
            state->written = written;
            state->done.set();
        });
    });
    // A frame that cannot be queued is dropped: it reports false
    write(std::move(frame));
    if (!co_await state->done.wait()) co_return false;   // the wait was cancelled
    co_return state->written;
}

void connection_sink::observe(std::shared_ptr<response_observer> observer) {
    // The connection tells it, as it writes the frames of the stream (on the connection
    // thread, as this call). Not once the connection is gone: it no longer detaches the
    // stream, which the request may keep (see http_stream::detach())
    auto connection = connection_.lock();
    if (auto stream = stream_.lock(); connection && stream) stream->set_observer(std::move(observer));
}

void connection_sink::take_over(std::shared_ptr<http_response> response, takeover_handler handler,
                                body_framing framing) {
    auto connection = connection_.lock();
    auto stream = stream_.lock();
    if (!connection || !stream) {
        LOG_WARNING("connection closed before it could be taken over: the takeover handler is not called");
        return;
    }

    // Its head tells the client that the body ends when the connection closes; the
    // connection is still handed over
    if (framing == body_framing::close) must_close(detail::close_reason::framing_by_close);

    // The connection stops reading requests after this one, and hands the socket over
    // once this response has been written
    connection->take_over(stream, std::move(response), std::move(handler));
}

} // namespace thinger::http

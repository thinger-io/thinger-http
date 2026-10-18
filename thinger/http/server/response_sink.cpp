#include "response_sink.hpp"
#include "request.hpp"
#include "../common/http_data.hpp"
#include "../data/out_chunk.hpp"
#include "../data/out_string.hpp"
#include "../../util/logger.hpp"
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

namespace thinger::http {

connection_sink::connection_sink(const request& req)
    : connection_(req.http_connection_), stream_(req.http_stream_) {}

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
        head->set_keep_alive(false);
        close_after_response();
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
    if (auto stream = stream_.lock(); stream && stream->observer()) stream->observer()->abort();
    close_after_response();
    auto frame = std::make_shared<http_data>();
    frame->set_last_frame(true);
    write(std::move(frame));
}

void connection_sink::close_after_response() {
    auto connection = connection_.lock();
    auto stream = stream_.lock();
    if (!connection || !stream) return;
    // Before any frame queued after this call (frames from other threads are posted too)
    boost::asio::dispatch(connection->get_socket()->get_io_context(), [stream] {
        stream->set_keep_alive(false);
    });
}

awaitable<bool> connection_sink::write_and_wait(std::shared_ptr<http_frame> frame) {
    // The writer waits on a timer that the connection makes expire once it wrote (or dropped)
    // the frame: the wait belongs to the io_context like any other operation, so it can be
    // cancelled (e.g. the frame waits behind another response that never ends) and is
    // released with the io_context
    struct waiter {
        explicit waiter(const boost::asio::any_io_executor& executor)
            : timer(executor, boost::asio::steady_timer::time_point::max()) {}
        boost::asio::steady_timer timer;
        bool done = false;
        bool written = false;
    };
    auto state = std::make_shared<waiter>(co_await boost::asio::this_coro::executor);
    frame->on_written([weak = std::weak_ptr<waiter>(state)](bool written) {
        auto waiting = weak.lock();
        if (!waiting) return;   // the writer is gone
        // on the executor of the writer, which owns the timer (never inline within the
        // connection write loop)
        boost::asio::post(waiting->timer.get_executor(), [weak, written] {
            auto state = weak.lock();
            if (!state) return;
            state->done = true;
            state->written = written;
            state->timer.expires_at(boost::asio::steady_timer::time_point::min());
        });
    });
    // A frame that cannot be queued is dropped: it reports false
    write(std::move(frame));
    while (!state->done) {
        auto [ec] = co_await state->timer.async_wait(use_nothrow_awaitable);
        if (!state->done && ec == boost::asio::error::operation_aborted) co_return false;
    }
    co_return state->written;
}

void connection_sink::observe(std::shared_ptr<response_observer> observer) {
    // The connection tells it, as it writes the frames of the stream (on the connection
    // thread, as this call)
    if (auto stream = stream_.lock()) stream->set_observer(std::move(observer));
}

void connection_sink::take_over(std::shared_ptr<http_response> response, takeover_handler handler) {
    auto connection = connection_.lock();
    auto stream = stream_.lock();
    if (!connection || !stream) {
        LOG_WARNING("connection closed before it could be taken over: the takeover handler is not called");
        return;
    }

    // The connection stops reading requests after this one, and hands the socket over
    // once this response has been written
    connection->take_over(stream, std::move(response), std::move(handler));
}

} // namespace thinger::http

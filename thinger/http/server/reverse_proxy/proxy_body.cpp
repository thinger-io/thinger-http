// The request sent by the reverse proxy to the target: its head, and its body streamed
#include "proxy_exchange.hpp"
#include "../body_reader.hpp"
#include <charconv>

namespace thinger::http::proxy_detail {

awaitable<io_result> proxy_exchange::read_target(uint8_t* buffer, size_t size) {
    auto result = co_await target_socket_->read_some(buffer, size);
    if (!std::get<0>(result)) touch();
    co_return result;
}

awaitable<bool> proxy_exchange::write_target(const std::vector<boost::asio::const_buffer>& buffers) {
    auto [ec, bytes] = co_await target_socket_->write(buffers);
    if (ec) co_return false;
    touch();
    co_return true;
}

void proxy_exchange::request_done() {
    request_sent_ = true;
    if (request_sent_signal_) request_sent_signal_->cancel();
}

void proxy_exchange::close_client_after_response() {
    if (auto stream = req_.get_http_stream()) stream->set_keep_alive(false);
    incoming_->set_keep_alive(false);
}

void proxy_exchange::reject_large_body() {
    close_client_after_response();
    fail(http_response::status::payload_too_large, "Payload Too Large", "request body too large");
}

awaitable<bool> proxy_exchange::send_head() {
    std::vector<boost::asio::const_buffer> buffers{boost::asio::buffer(request_head_)};
    if (body_buffered_) buffers.push_back(boost::asio::buffer(incoming_->get_body()));
    bool written = co_await write_target(buffers);
    if (body_buffered_) result_.request_bytes = incoming_->get_body().size();
    co_return written;
}

void proxy_exchange::frame_part(std::vector<boost::asio::const_buffer>& buffers, std::string& size_line,
                                const uint8_t* data, size_t size) const {
    static const std::string crlf = "\r\n";
    buffers.clear();
    if (body_mode_ == body_mode::chunked) {
        char hex[20];
        auto [end, ec] = std::to_chars(hex, hex + sizeof(hex), size, 16);
        size_line.assign(hex, end);
        size_line += crlf;
        buffers.push_back(boost::asio::buffer(size_line));
    }
    buffers.push_back(boost::asio::buffer(data, size));
    if (body_mode_ == body_mode::chunked) buffers.push_back(boost::asio::buffer(crlf));
}

awaitable<void> proxy_exchange::stream_body() {
    // The body goes at the pace of the target: each part is read from the client once the
    // target took the previous one
    std::vector<uint8_t> buffer(buffer_size);
    std::vector<boost::asio::const_buffer> buffers;
    std::string size_line;
    bool target_open = true;
    while (target_open) {
        size_t bytes = co_await req_.read_some(buffer.data(), buffer.size());
        if (bytes == 0) break;
        touch();
        result_.request_bytes += bytes;
        if (options_.max_body_size && result_.request_bytes > options_.max_body_size) {
            reject_large_body();
            stopped_ = stop_reason::request_failed;
            close_target();
            co_return;
        }
        frame_part(buffers, size_line, buffer.data(), bytes);
        target_open = co_await write_target(buffers);
    }

    // Cancelled once the response head arrived: the rest of the body is not relayed
    auto state = co_await boost::asio::this_coro::cancellation_state;
    if (state.cancelled() != boost::asio::cancellation_type::none) co_return;

    // A body the client did not send whole (or malformed, too large): the server answers
    // it and closes the connection; the target never gets its end
    if (req_.get_body_error() != body_error::none) {
        result_.error = "invalid or incomplete request body";
        LOG_WARNING("reverse proxy {}: {}", describe(), result_.error);
        stopped_ = stop_reason::request_failed;
        close_target();
        co_return;
    }

    if (target_open && body_mode_ == body_mode::chunked) {
        static const std::string last_chunk = "0\r\n\r\n";
        target_open = co_await write_target({boost::asio::buffer(last_chunk)});
    }
    // If the target stopped reading it (it may have answered early), what is left of
    // the body is not read: the server discards it, or closes the connection
    if (target_open && !req_.has_pending_body()) request_done();

    // The response ends the exchange, not the body
    co_await wait_forever();
}

}

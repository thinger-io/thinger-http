#include "server_connection.hpp"
#include "request.hpp"
#include "../../util/logger.hpp"

namespace thinger::http {

std::atomic<unsigned long> server_connection::connections(0);

server_connection::server_connection(std::shared_ptr<asio::socket> socket)
    : socket_(std::move(socket))
    , timeout_timer_(socket_->get_io_context()) {
    ++connections;
    LOG_DEBUG("created http server connection total: {}", static_cast<unsigned>(connections));
}

server_connection::~server_connection() {
    --connections;
    LOG_DEBUG("releasing http server connection. total: {}", static_cast<unsigned>(connections));
}

void server_connection::start(std::chrono::seconds timeout) {
    if (running_) return;
    running_ = true;
    timeout_ = timeout;

    // Start timeout timer
    reset_timeout();

    // Spawn the read loop coroutine
    co_spawn(socket_->get_io_context(),
        [self = shared_from_this()]() -> awaitable<void> {
            co_await self->read_loop();
        },
        detached);
}

void server_connection::reset_timeout() {
    timeout_timer_.expires_after(timeout_);
    timeout_timer_.async_wait([this, self = shared_from_this()](const boost::system::error_code& ec) {
        if (ec) return; // Timer was cancelled

        LOG_DEBUG("http server connection timed out after {} seconds", timeout_.count());
        close();
    });
}

void server_connection::close() {
    running_ = false;
    timeout_timer_.cancel();
    socket_->close();
}

awaitable<void> server_connection::read_loop() {
    auto self = shared_from_this();

    // Parse headers only; body reading is managed by the handler layer
    request_parser_.set_headers_only(true);

    size_t buffered = 0; // bytes of valid data in buffer_

    try {
        while (running_ && socket_->is_open()) {
            // If no buffered data, read from socket
            if (buffered == 0) {
                auto [ec, bytes] = co_await socket_->read_some(buffer_, MAX_BUFFER_SIZE);
                if (ec) break;
                reset_timeout();
                buffered = bytes;
            }

            // Parse available data
            uint8_t* begin = buffer_;
            uint8_t* end = buffer_ + buffered;
            boost::tribool result = request_parser_.parse(begin, end);
            size_t unconsumed = static_cast<size_t>(end - begin);

            if (result) {
                // Successfully parsed headers
                auto http_req = request_parser_.consume_request();
                http_req->set_ssl(socket_->is_secure());

                size_t content_length = http_req->get_content_length();

                auto stream = std::make_shared<http_stream>(++request_id_, http_req->keep_alive());

                // Add to queue for pipelining
                {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    request_queue_.push(stream);
                }

                // Log the request
                http_req->log("SERVER REQUEST", 0);

                // Create request and store read-ahead data
                auto req = std::make_shared<request>(self, stream, http_req);
                if (unconsumed > 0) {
                    req->set_read_ahead(begin, unconsumed);
                }

                // Dispatch to handler (awaitable — handler decides body reading strategy)
                if (handler_) {
                    co_await handler_(req);
                    reset_timeout();
                }

                // After handler completes, compute leftover for pipelining.
                // The request tracks how many read-ahead bytes remain unconsumed.
                // Any leftover read-ahead bytes are pipelined data for the next request.
                size_t remaining_ahead = req->read_ahead_available();
                if (remaining_ahead > 0) {
                    // Copy residual read-ahead back to buffer_ for the next iteration
                    size_t ahead_start = unconsumed - remaining_ahead;
                    std::memmove(buffer_, begin + ahead_start, remaining_ahead);
                    buffered = remaining_ahead;
                } else {
                    buffered = 0;
                }

                // If not keep-alive, stop reading after this request
                if (!stream->keep_alive()) {
                    break;
                }
            } else if (!result) {
                // Bad request
                LOG_ERROR("invalid http request");
                auto stream = std::make_shared<http_stream>(++request_id_, false);
                {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    request_queue_.push(stream);
                }
                handle_stock_error(stream, http_response::status::bad_request);
                break;
            } else {
                // Indeterminate — all data consumed by parser, need more
                buffered = 0;
            }
        }
    } catch (const boost::system::system_error& e) {
        if (e.code() != boost::asio::error::operation_aborted &&
            e.code() != boost::asio::error::eof) {
            LOG_ERROR("Error in server connection read loop: {}", e.what());
        }
    }

    // Connection ended
    running_ = false;
    timeout_timer_.cancel();
}

awaitable<void> server_connection::write_frame(std::shared_ptr<http_stream> stream,
                                                std::shared_ptr<http_frame> frame) {
    // Log response
    frame->log("SERVER RESPONSE", 0);

    // Write frame to socket
    co_await frame->to_socket(socket_);

    // Reset timeout on activity
    reset_timeout();

    // Check if stream is complete
    if (frame->end_stream()) {
        stream->completed();

        if (!stream->keep_alive()) {
            close();
        } else {
            // Remove completed stream from queue
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!request_queue_.empty()) {
                request_queue_.pop();
            }
        }
    }
}

void server_connection::process_output_queue() {
    if (writing_) return;

    std::shared_ptr<http_stream> stream;
    std::shared_ptr<http_frame> frame;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (request_queue_.empty()) return;

        stream = request_queue_.front();
        if (stream->empty_queue()) return;

        frame = stream->current_frame();
        stream->pop_frame();
    }

    writing_ = true;

    co_spawn(socket_->get_io_context(),
        [this, self = shared_from_this(), stream, frame]() -> awaitable<void> {
            try {
                co_await write_frame(stream, frame);
            } catch (const boost::system::system_error& e) {
                LOG_ERROR("Error writing frame: {}", e.what());
                close();
            }
            writing_ = false;

            // Process more frames if available
            process_output_queue();
        },
        detached);
}

void server_connection::handle_stream(std::shared_ptr<http_stream> stream,
                                       std::shared_ptr<http_frame> frame) {
    boost::asio::dispatch(socket_->get_io_context(),
        [this, self = shared_from_this(), stream, frame] {
            stream->add_frame(frame);

            std::shared_ptr<http_stream> front_stream;
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                if (request_queue_.empty()) {
                    LOG_ERROR("trying to send response without a pending request!");
                    return;
                }
                front_stream = request_queue_.front();
            }

            // Only process if this is the front stream (for pipelining order)
            if (front_stream->id() == stream->id()) {
                process_output_queue();
            }
        });
}

void server_connection::handle_stock_error(std::shared_ptr<http_stream> stream,
                                            http_response::status status) {
    auto http_error = http_response::stock_http_reply(status);
    http_error->set_keep_alive(stream->keep_alive());
    handle_stream(stream, http_error);
}

std::shared_ptr<asio::socket> server_connection::release_socket() {
    running_ = false;
    socket_->cancel();
    timeout_timer_.cancel();
    return socket_;
}

void server_connection::release() {
    boost::asio::dispatch(socket_->get_io_context(),
        [this, self = shared_from_this()] {
            close();
        });
}

std::shared_ptr<asio::socket> server_connection::get_socket() {
    return socket_;
}

void server_connection::update_connection_timeout(std::chrono::seconds timeout) {
    boost::asio::dispatch(socket_->get_io_context(),
        [this, self = shared_from_this(), timeout] {
            timeout_ = timeout;
            reset_timeout();
        });
}

}

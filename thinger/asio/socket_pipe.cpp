#include "socket_pipe.hpp"
#include "../util/logger.hpp"

#include <vector>

namespace thinger::asio {

socket_pipe::socket_pipe(std::shared_ptr<socket> source, std::shared_ptr<socket> target)
    : source_(std::move(source)), target_(std::move(target)) {
}

socket_pipe::~socket_pipe() {
    try {
        if (on_end_) on_end_();
    } catch (...) {}
}

awaitable<void> socket_pipe::run() {
    auto self = shared_from_this();
    co_await (
        forward(source_, target_, bytes_s2t_) ||
        forward(target_, source_, bytes_t2s_)
    );
    cancel();
}

void socket_pipe::start() {
    auto self = shared_from_this();
    co_spawn(source_->get_io_context(), [self]() -> awaitable<void> {
        co_await self->run();
    }, detached);
}

void socket_pipe::cancel() {
    if (cancelled_.exchange(true)) return;
    source_->close();
    target_->close();
}

void socket_pipe::set_on_end(std::function<void()> listener) {
    on_end_ = std::move(listener);
}

size_t socket_pipe::bytes_source_to_target() const {
    return bytes_s2t_.load();
}

size_t socket_pipe::bytes_target_to_source() const {
    return bytes_t2s_.load();
}

std::shared_ptr<socket> socket_pipe::get_source() const {
    return source_;
}

std::shared_ptr<socket> socket_pipe::get_target() const {
    return target_;
}

awaitable<void> socket_pipe::forward(
    std::shared_ptr<socket> from,
    std::shared_ptr<socket> to,
    std::atomic<size_t>& bytes_transferred)
{
    try {
        std::vector<uint8_t> buffer(BUFFER_SIZE);
        while (!cancelled_) {
            size_t n = co_await from->read_some(buffer.data(), BUFFER_SIZE);
            if (n == 0) break;
            co_await to->write(buffer.data(), n);
            bytes_transferred.fetch_add(n, std::memory_order_relaxed);
        }
    } catch (const boost::system::system_error& e) {
        if (e.code() != boost::asio::error::eof &&
            e.code() != boost::asio::error::operation_aborted) {
            LOG_WARNING("socket_pipe forward error: {}", e.what());
        }
    } catch (const std::exception& e) {
        LOG_WARNING("socket_pipe forward error: {}", e.what());
    }
    // Close both sockets to interrupt the other direction
    cancel();
}

} // namespace thinger::asio

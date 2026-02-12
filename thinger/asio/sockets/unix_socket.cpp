#include "unix_socket.hpp"
#include "../../util/logger.hpp"
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/read_until.hpp>

namespace thinger::asio {

unix_socket::unix_socket(const std::string &context, boost::asio::io_context &io_context)
    : socket(context, io_context), socket_(io_context) {
}

unix_socket::unix_socket(const std::string &context, boost::asio::local::stream_protocol::socket&& sock)
    : socket(context, static_cast<boost::asio::io_context&>(sock.get_executor().context()))
    , socket_(std::move(sock)) {
}

unix_socket::~unix_socket() {
    unix_socket::close();
}

void unix_socket::close() {
    try {
        if (socket_.is_open()) {
            boost::system::error_code ec;
            socket_.close(ec);
        }
    } catch (...) {
    }
}

void unix_socket::cancel() {
    socket_.cancel();
}

awaitable<void> unix_socket::connect(const std::string &path, std::chrono::seconds timeout) {
    close();

    // Setup timeout timer
    boost::asio::steady_timer timer(io_context_);
    timer.expires_after(timeout);

    bool timed_out = false;

    // Start timeout
    auto timeout_coro = [&]() -> awaitable<void> {
        auto [ec] = co_await timer.async_wait(use_nothrow_awaitable);
        if (!ec) {
            timed_out = true;
            socket_.cancel();
        }
    };

    // Start connect
    auto connect_coro = [&]() -> awaitable<void> {
        auto [ec] = co_await socket_.async_connect(
            boost::asio::local::stream_protocol::endpoint(path),
            use_nothrow_awaitable);
        timer.cancel();
        if (ec) {
            if (timed_out) {
                throw boost::system::system_error(boost::asio::error::timed_out);
            }
            throw boost::system::system_error(ec);
        }
    };

    co_spawn(io_context_, timeout_coro(), detached);
    co_await connect_coro();
}

awaitable<void> unix_socket::connect(
    const std::string &host,
    const std::string &port,
    std::chrono::seconds timeout)
{
    LOG_WARNING("calling connect to a unix socket over host/port");
    co_await connect(host, timeout);
}

std::string unix_socket::get_remote_ip() const {
    boost::system::error_code ec;
    auto remote_ep = socket_.remote_endpoint(ec);
    if (!ec) {
        return remote_ep.path();
    }
    return "";
}

std::string unix_socket::get_local_port() const {
    return "0";
}

std::string unix_socket::get_remote_port() const {
    return "0";
}

awaitable<size_t> unix_socket::read_some(uint8_t buffer[], size_t max_size) {
    co_return co_await socket_.async_read_some(
        boost::asio::buffer(buffer, max_size),
        use_awaitable);
}

awaitable<size_t> unix_socket::read(uint8_t buffer[], size_t size) {
    co_return co_await boost::asio::async_read(
        socket_,
        boost::asio::buffer(buffer, size),
        boost::asio::transfer_exactly(size),
        use_awaitable);
}

awaitable<size_t> unix_socket::read(boost::asio::streambuf& buffer, size_t size) {
    co_return co_await boost::asio::async_read(
        socket_,
        buffer,
        boost::asio::transfer_exactly(size),
        use_awaitable);
}

awaitable<size_t> unix_socket::read_until(boost::asio::streambuf& buffer, std::string_view delim) {
    co_return co_await boost::asio::async_read_until(
        socket_,
        buffer,
        std::string(delim),
        use_awaitable);
}

awaitable<size_t> unix_socket::write(const uint8_t buffer[], size_t size) {
    co_return co_await boost::asio::async_write(
        socket_,
        boost::asio::buffer(buffer, size),
        use_awaitable);
}

awaitable<size_t> unix_socket::write(std::string_view str) {
    co_return co_await boost::asio::async_write(
        socket_,
        boost::asio::buffer(str.data(), str.size()),
        use_awaitable);
}

awaitable<size_t> unix_socket::write(const std::vector<boost::asio::const_buffer>& buffers) {
    co_return co_await boost::asio::async_write(
        socket_,
        buffers,
        use_awaitable);
}

awaitable<void> unix_socket::wait(boost::asio::socket_base::wait_type type) {
    co_await socket_.async_wait(type, use_awaitable);
}

bool unix_socket::is_open() const {
    return socket_.is_open();
}

bool unix_socket::is_secure() const {
    return false;
}

size_t unix_socket::available() const {
    boost::system::error_code ec;
    auto size = socket_.available(ec);
    if (ec) {
        LOG_ERROR("error while getting socket available bytes ({}): {}", size, ec.message());
    }
    return size;
}

}

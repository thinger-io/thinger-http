#include <iostream>
#include "tcp_socket.hpp"

namespace thinger::asio {

tcp_socket::tcp_socket(const std::string& context, boost::asio::io_context& io_context)
    : socket(context, io_context), socket_(io_context) {
}

tcp_socket::tcp_socket(const std::string& context, const std::shared_ptr<tcp_socket>& tcp_socket)
    : socket(context, tcp_socket->get_io_context()), socket_(std::move(tcp_socket->get_socket())) {
}

tcp_socket::tcp_socket(const std::string& context, boost::asio::ip::tcp::socket&& sock)
    : socket(context, static_cast<boost::asio::io_context&>(sock.get_executor().context())), socket_(std::move(sock)) {
}

tcp_socket::~tcp_socket() {
    LOG_TRACE("releasing tcp connection");
    close();
}

void tcp_socket::close() {
    boost::system::error_code ec;
    if (socket_.is_open()) {
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    }
    socket_.close(ec);
    LOG_TRACE("closing tcp socket result: {}", ec.message());
}

void tcp_socket::cancel() {
    socket_.cancel();
}

awaitable<void> tcp_socket::connect(
    const std::string& host,
    const std::string& port,
    std::chrono::seconds timeout)
{
    close();

    // Resolve host
    boost::asio::ip::tcp::resolver resolver(io_context_);
    auto [ec_resolve, endpoints] = co_await resolver.async_resolve(
        host, port, use_nothrow_awaitable);

    if (ec_resolve) {
        throw boost::system::system_error(ec_resolve);
    }

    // Setup timeout timer
    boost::asio::steady_timer timer(io_context_);
    timer.expires_after(timeout);

    // Race between connect and timeout
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
        auto [ec, endpoint] = co_await boost::asio::async_connect(
            socket_, endpoints, use_nothrow_awaitable);
        timer.cancel();
        if (ec) {
            if (timed_out) {
                throw boost::system::system_error(boost::asio::error::timed_out);
            }
            throw boost::system::system_error(ec);
        }
    };

    // Wait for connect (timeout will cancel if needed)
    co_spawn(io_context_, timeout_coro(), detached);
    co_await connect_coro();

    // Run handshake if required (for SSL sockets)
    if (requires_handshake()) {
        co_await handshake(host);
    }
}

boost::asio::ip::tcp::socket& tcp_socket::get_socket() {
    return socket_;
}

std::string tcp_socket::get_remote_ip() const {
    boost::system::error_code ec;
    auto remote_ep = socket_.remote_endpoint(ec);
    if (!ec) {
        return remote_ep.address().to_string();
    }
    return "0.0.0.0";
}

std::string tcp_socket::get_local_port() const {
    boost::system::error_code ec;
    auto local_ep = socket_.local_endpoint(ec);
    if (!ec) {
        return std::to_string(local_ep.port());
    }
    return "0";
}

std::string tcp_socket::get_remote_port() const {
    boost::system::error_code ec;
    auto remote_ep = socket_.remote_endpoint(ec);
    if (!ec) {
        return std::to_string(remote_ep.port());
    }
    return "0";
}

awaitable<size_t> tcp_socket::read_some(uint8_t* buffer, size_t max_size) {
    co_return co_await socket_.async_read_some(
        boost::asio::buffer(buffer, max_size),
        use_awaitable);
}

awaitable<size_t> tcp_socket::read(uint8_t* buffer, size_t size) {
    co_return co_await boost::asio::async_read(
        socket_,
        boost::asio::buffer(buffer, size),
        boost::asio::transfer_exactly(size),
        use_awaitable);
}

awaitable<size_t> tcp_socket::read(boost::asio::streambuf& buffer, size_t size) {
    co_return co_await boost::asio::async_read(
        socket_,
        buffer,
        boost::asio::transfer_exactly(size),
        use_awaitable);
}

awaitable<size_t> tcp_socket::read_until(boost::asio::streambuf& buffer, std::string_view delim) {
    co_return co_await boost::asio::async_read_until(
        socket_,
        buffer,
        std::string(delim),
        use_awaitable);
}

awaitable<size_t> tcp_socket::write(const uint8_t* buffer, size_t size) {
    co_return co_await boost::asio::async_write(
        socket_,
        boost::asio::buffer(buffer, size),
        use_awaitable);
}

awaitable<size_t> tcp_socket::write(std::string_view str) {
    co_return co_await boost::asio::async_write(
        socket_,
        boost::asio::buffer(str.data(), str.size()),
        use_awaitable);
}

awaitable<size_t> tcp_socket::write(const std::vector<boost::asio::const_buffer>& buffers) {
    co_return co_await boost::asio::async_write(
        socket_,
        buffers,
        use_awaitable);
}

awaitable<void> tcp_socket::wait(boost::asio::socket_base::wait_type type) {
    co_await socket_.async_wait(type, use_awaitable);
}

void tcp_socket::enable_tcp_no_delay() {
    socket_.set_option(boost::asio::ip::tcp::no_delay(true));
}

void tcp_socket::disable_tcp_no_delay() {
    socket_.set_option(boost::asio::ip::tcp::no_delay(false));
}

bool tcp_socket::is_open() const {
    return socket_.is_open();
}

bool tcp_socket::is_secure() const {
    return false;
}

size_t tcp_socket::available() const {
    boost::system::error_code ec;
    auto size = socket_.available(ec);
    if (ec) {
        LOG_ERROR("error while getting socket available bytes ({}): {}", size, ec.message());
    }
    return size;
}

}

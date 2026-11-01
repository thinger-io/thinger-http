#include <algorithm>
#include <charconv>
#include <iostream>
#include "tcp_socket.hpp"
#include "../with_timeout.hpp"

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

bool tcp_socket::shutdown_send() {
    boost::system::error_code ec;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);
    return !ec;
}

void tcp_socket::cancel() {
    socket_.cancel();
}

awaitable<boost::system::error_code> tcp_socket::connect(
    const std::string& host,
    const std::string& port,
    std::chrono::seconds timeout)
{
    // One deadline for the whole call: each phase (resolve, connect, handshake) gets the time
    // left until it, so a peer cannot stretch the call by answering just in time to each
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto time_left = [&deadline] {
        return std::max(deadline - std::chrono::steady_clock::now(), std::chrono::steady_clock::duration::zero());
    };
    // A phase did not end in time: the socket is closed, and the error is timed_out, or
    // operation_aborted when the caller cancelled the call (the timer may then win the race)
    auto expired = [this](const boost::asio::cancellation_state& state) {
        close();
        return state.cancelled() != boost::asio::cancellation_type::none
            ? boost::system::error_code{boost::asio::error::operation_aborted}
            : boost::system::error_code{boost::asio::error::timed_out};
    };

    close();

    // Resolve the host, unless it is already an address with a numeric port. asio runs
    // getaddrinfo on a thread of its own and cannot interrupt it: a resolve that hangs ends
    // the call only once getaddrinfo returns (its own timeouts), with timed_out
    using results_type = boost::asio::ip::tcp::resolver::results_type;
    results_type endpoints;
    boost::system::error_code address_ec;
    auto address = boost::asio::ip::make_address(host, address_ec);
    uint16_t port_number = 0;
    auto [port_end, port_ec] = std::from_chars(port.data(), port.data() + port.size(), port_number);
    if (!address_ec && port_ec == std::errc{} && port_end == port.data() + port.size()) {
        endpoints = results_type::create(boost::asio::ip::tcp::endpoint(address, port_number), host, port);
    } else {
        boost::asio::ip::tcp::resolver resolver(io_context_);
        auto resolved = co_await with_timeout(resolver.async_resolve(host, port, use_nothrow_awaitable), time_left());
        if (!resolved) co_return expired(co_await boost::asio::this_coro::cancellation_state);
        auto [resolve_ec, results] = std::move(*resolved);
        if (resolve_ec) co_return resolve_ec;
        endpoints = std::move(results);
    }

    // Connect: a connect not done in time is cancelled
    auto connected = co_await with_timeout(
        boost::asio::async_connect(socket_, endpoints, use_nothrow_awaitable), time_left());
    if (!connected) co_return expired(co_await boost::asio::this_coro::cancellation_state);
    auto connect_ec = std::get<0>(*connected);
    if (connect_ec) {
        co_return connect_ec;
    }

    // Run the client handshake if required (for SSL sockets), with the time left: a peer that
    // accepts and stays silent, or trickles its answer, must not hold the call
    if (requires_handshake()) {
        auto handshaken = co_await with_timeout(handshake(host), time_left());
        if (!handshaken) co_return expired(co_await boost::asio::this_coro::cancellation_state);
        if (*handshaken) {
            close();
            co_return *handshaken;
        }
    }

    co_return boost::system::error_code{};
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

awaitable<io_result> tcp_socket::read_some(uint8_t* buffer, size_t max_size) {
    co_return co_await socket_.async_read_some(
        boost::asio::buffer(buffer, max_size),
        use_nothrow_awaitable);
}

awaitable<io_result> tcp_socket::read(uint8_t* buffer, size_t size) {
    co_return co_await boost::asio::async_read(
        socket_,
        boost::asio::buffer(buffer, size),
        boost::asio::transfer_exactly(size),
        use_nothrow_awaitable);
}

awaitable<io_result> tcp_socket::read(boost::asio::streambuf& buffer, size_t size) {
    co_return co_await boost::asio::async_read(
        socket_,
        buffer,
        boost::asio::transfer_exactly(size),
        use_nothrow_awaitable);
}

awaitable<io_result> tcp_socket::read_until(boost::asio::streambuf& buffer, std::string_view delim) {
    co_return co_await boost::asio::async_read_until(
        socket_,
        buffer,
        std::string(delim),
        use_nothrow_awaitable);
}

awaitable<io_result> tcp_socket::write(const uint8_t* buffer, size_t size) {
    co_return co_await boost::asio::async_write(
        socket_,
        boost::asio::buffer(buffer, size),
        use_nothrow_awaitable);
}

awaitable<io_result> tcp_socket::write(std::string_view str) {
    co_return co_await boost::asio::async_write(
        socket_,
        boost::asio::buffer(str.data(), str.size()),
        use_nothrow_awaitable);
}

awaitable<io_result> tcp_socket::write(const std::vector<boost::asio::const_buffer>& buffers) {
    co_return co_await boost::asio::async_write(
        socket_,
        buffers,
        use_nothrow_awaitable);
}

size_t tcp_socket::write_now(const std::vector<boost::asio::const_buffer>& buffers, boost::system::error_code& ec) {
    return send_now(socket_.native_handle(), buffers, ec);
}

awaitable<boost::system::error_code> tcp_socket::wait(boost::asio::socket_base::wait_type type) {
    auto [ec] = co_await socket_.async_wait(type, use_nothrow_awaitable);
    co_return ec;
}

awaitable<boost::system::error_code> tcp_socket::peer_closed() {
    co_return peek_closed(socket_.native_handle());
}

bool tcp_socket::enable_tcp_no_delay() {
    boost::system::error_code ec;
    socket_.set_option(boost::asio::ip::tcp::no_delay(true), ec);
    if (ec) LOG_DEBUG("cannot enable tcp_no_delay: {}", ec.message());
    return !ec;
}

bool tcp_socket::disable_tcp_no_delay() {
    boost::system::error_code ec;
    socket_.set_option(boost::asio::ip::tcp::no_delay(false), ec);
    if (ec) LOG_DEBUG("cannot disable tcp_no_delay: {}", ec.message());
    return !ec;
}

bool tcp_socket::is_open() const {
    return socket_.is_open();
}

bool tcp_socket::is_secure() const {
    return false;
}

bool tcp_socket::readable_now() const {
    return socket_.is_open() && poll_readable(const_cast<boost::asio::ip::tcp::socket&>(socket_).native_handle());
}

size_t tcp_socket::send_backlog() const {
    return socket_.is_open() ? output_queue(const_cast<boost::asio::ip::tcp::socket&>(socket_).native_handle()) : 0;
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

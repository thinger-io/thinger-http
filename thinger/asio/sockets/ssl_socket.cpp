#include "ssl_socket.hpp"
#include <algorithm>
#include <array>
#include <cstring>

namespace thinger::asio {

ssl_socket::ssl_socket(const std::string& context, boost::asio::io_context& io_context,
                       const std::shared_ptr<boost::asio::ssl::context>& ssl_context)
    : tcp_socket(context, io_context)
    , ssl_stream_(socket_, *ssl_context)
    , ssl_context_(ssl_context) {
}

ssl_socket::ssl_socket(const std::string& context, const std::shared_ptr<tcp_socket>& socket,
                       const std::shared_ptr<boost::asio::ssl::context>& ssl_context)
    : tcp_socket(context, socket)
    , ssl_stream_(socket_, *ssl_context)
    , ssl_context_(ssl_context) {
}

ssl_socket::~ssl_socket() {
    LOG_TRACE("releasing ssl connection");
}

void ssl_socket::close() {
    // close underlying TCP socket
    tcp_socket::close();
    read_ahead_.clear();

    // clear ssl session to allow reusing socket (if necessary)
    // From SSL_clear: If a session is still open, it is considered bad and will be removed
    // from the session cache, as required by RFC2246
    SSL_clear(ssl_stream_.native_handle());
}

bool ssl_socket::requires_handshake() const {
    return true;
}

awaitable<boost::system::error_code> ssl_socket::handshake(const std::string& host) {
    if (!host.empty()) {
        // add support for SNI
        if (!SSL_set_tlsext_host_name(ssl_stream_.native_handle(), host.c_str())) {
            LOG_ERROR("SSL_set_tlsext_host_name failed. SNI will fail");
        }
        // client handshake
        auto [ec] = co_await ssl_stream_.async_handshake(
            boost::asio::ssl::stream_base::client,
            use_nothrow_awaitable);
        co_return ec;
    } else {
        // server handshake
        auto [ec] = co_await ssl_stream_.async_handshake(
            boost::asio::ssl::stream_base::server,
            use_nothrow_awaitable);
        co_return ec;
    }
}

size_t ssl_socket::take_read_ahead(uint8_t buffer[], size_t size) {
    auto taken = std::min(size, read_ahead_.size());
    std::memcpy(buffer, read_ahead_.data(), taken);
    read_ahead_.erase(0, taken);
    return taken;
}

void ssl_socket::take_read_ahead(boost::asio::streambuf& buffer, size_t max_size) {
    auto taken = std::min(max_size, read_ahead_.size());
    buffer.commit(boost::asio::buffer_copy(buffer.prepare(taken), boost::asio::buffer(read_ahead_, taken)));
    read_ahead_.erase(0, taken);
}

awaitable<io_result> ssl_socket::read_some(uint8_t buffer[], size_t max_size) {
    if (!read_ahead_.empty()) {
        co_return io_result{boost::system::error_code{}, take_read_ahead(buffer, max_size)};
    }
    co_return co_await ssl_stream_.async_read_some(
        boost::asio::buffer(buffer, max_size),
        use_nothrow_awaitable);
}

awaitable<io_result> ssl_socket::read(uint8_t buffer[], size_t size) {
    auto taken = take_read_ahead(buffer, size);
    if (taken == size) co_return io_result{boost::system::error_code{}, size};
    auto [ec, bytes] = co_await boost::asio::async_read(
        ssl_stream_,
        boost::asio::buffer(buffer + taken, size - taken),
        boost::asio::transfer_exactly(size - taken),
        use_nothrow_awaitable);
    co_return io_result{ec, taken + bytes};
}

awaitable<io_result> ssl_socket::read(boost::asio::streambuf& buffer, size_t size) {
    auto before = buffer.size();
    take_read_ahead(buffer, size);
    auto taken = buffer.size() - before;
    if (taken == size) co_return io_result{boost::system::error_code{}, size};
    auto [ec, bytes] = co_await boost::asio::async_read(
        ssl_stream_,
        buffer,
        boost::asio::transfer_exactly(size - taken),
        use_nothrow_awaitable);
    co_return io_result{ec, taken + bytes};
}

awaitable<io_result> ssl_socket::read_until(boost::asio::streambuf& buffer, std::string_view delim) {
    // read_until() looks for the delimiter in what the buffer already holds first
    take_read_ahead(buffer, read_ahead_.size());
    co_return co_await boost::asio::async_read_until(
        ssl_stream_,
        buffer,
        std::string(delim),
        use_nothrow_awaitable);
}

awaitable<io_result> ssl_socket::write(const uint8_t buffer[], size_t size) {
    co_return co_await boost::asio::async_write(
        ssl_stream_,
        boost::asio::buffer(buffer, size),
        use_nothrow_awaitable);
}

awaitable<io_result> ssl_socket::write(std::string_view str) {
    co_return co_await boost::asio::async_write(
        ssl_stream_,
        boost::asio::buffer(str.data(), str.size()),
        use_nothrow_awaitable);
}

awaitable<io_result> ssl_socket::write(const std::vector<boost::asio::const_buffer>& buffers) {
    co_return co_await boost::asio::async_write(
        ssl_stream_,
        buffers,
        use_nothrow_awaitable);
}

awaitable<boost::system::error_code> ssl_socket::wait(boost::asio::socket_base::wait_type type) {
    if (type == boost::asio::socket_base::wait_read && !read_ahead_.empty()) {
        co_return boost::system::error_code{};
    }
    co_return co_await tcp_socket::wait(type);
}

awaitable<bool> ssl_socket::peer_closed() {
    if (!read_ahead_.empty()) co_return false;

    // The raw socket holds TLS records: a close_notify alert (sent before the FIN, so the
    // socket is not empty) cannot be told apart from data without decrypting it, as TLS
    // 1.3 encrypts alerts too. Read through the TLS stream and keep any data for the next
    // read: the end of the stream (close_notify, FIN) or an error means the peer is gone.
    std::array<uint8_t, 4096> data;
    auto [ec, bytes] = co_await ssl_stream_.async_read_some(boost::asio::buffer(data), use_nothrow_awaitable);
    if (bytes > 0) {
        read_ahead_.append(reinterpret_cast<const char*>(data.data()), bytes);
        co_return false;
    }
    co_return true;
}

bool ssl_socket::is_secure() const {
    return true;
}

}

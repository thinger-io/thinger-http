#include "ssl_socket.hpp"

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

awaitable<io_result> ssl_socket::read_some(uint8_t buffer[], size_t max_size) {
    co_return co_await ssl_stream_.async_read_some(
        boost::asio::buffer(buffer, max_size),
        use_nothrow_awaitable);
}

awaitable<io_result> ssl_socket::read(uint8_t buffer[], size_t size) {
    co_return co_await boost::asio::async_read(
        ssl_stream_,
        boost::asio::buffer(buffer, size),
        boost::asio::transfer_exactly(size),
        use_nothrow_awaitable);
}

awaitable<io_result> ssl_socket::read(boost::asio::streambuf& buffer, size_t size) {
    co_return co_await boost::asio::async_read(
        ssl_stream_,
        buffer,
        boost::asio::transfer_exactly(size),
        use_nothrow_awaitable);
}

awaitable<io_result> ssl_socket::read_until(boost::asio::streambuf& buffer, std::string_view delim) {
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

bool ssl_socket::is_secure() const {
    return true;
}

}

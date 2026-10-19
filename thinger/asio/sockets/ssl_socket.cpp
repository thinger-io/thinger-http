#include "ssl_socket.hpp"
#include <algorithm>
#include <array>
#include <cstring>

#include <boost/asio/ssl/host_name_verification.hpp>
#include <openssl/x509.h>

namespace thinger::asio {

namespace {

    // A certificate, DER encoded
    std::string to_der(X509* certificate) {
        int size = i2d_X509(certificate, nullptr);
        if (size <= 0) return {};
        std::string der(static_cast<size_t>(size), '\0');
        auto* data = reinterpret_cast<unsigned char*>(der.data());
        i2d_X509(certificate, &data);
        return der;
    }

    // An earlier session resumed without a client certificate, where the host name (SNI)
    // requires one: no certificate is asked for when a session is resumed, and the mode of the
    // host is set once OpenSSL decided to resume it
    bool resumed_without_certificate(SSL* ssl) {
        return SSL_session_reused(ssl) && (SSL_get_verify_mode(ssl) & SSL_VERIFY_FAIL_IF_NO_PEER_CERT) &&
               SSL_get0_peer_certificate(ssl) == nullptr;
    }

}

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
        // verify the server certificate hostname when verification is enabled on the
        // context (verify_peer); the callback is not consulted when verify_mode is verify_none
        boost::system::error_code verify_ec;
        ssl_stream_.set_verify_callback(boost::asio::ssl::host_name_verification(host), verify_ec);
        if (verify_ec) {
            LOG_ERROR("failed to enable host name verification: {}", verify_ec.message());
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
        if (!ec && resumed_without_certificate(ssl_stream_.native_handle())) {
            close();
            co_return boost::asio::error::access_denied;
        }
        co_return ec;
    }
}

void ssl_socket::set_client_verify(std::shared_ptr<const client_verify_policy> policy) {
    client_verify_ = std::move(policy);
    if (client_verify_) client_verify_->apply(ssl_stream_.native_handle());
}

X509* ssl_socket::peer_certificate() const {
    return SSL_get0_peer_certificate(const_cast<ssl_socket*>(this)->ssl_stream_.native_handle());
}

STACK_OF(X509)* ssl_socket::peer_chain() const {
    return SSL_get_peer_cert_chain(const_cast<ssl_socket*>(this)->ssl_stream_.native_handle());
}

std::optional<tls_session> ssl_socket::tls() const {
    SSL* ssl = const_cast<ssl_socket*>(this)->ssl_stream_.native_handle();
    if (!SSL_is_init_finished(ssl)) return std::nullopt;
    tls_session session;
    session.version = SSL_get_version(ssl);
    if (const SSL_CIPHER* cipher = SSL_get_current_cipher(ssl)) session.cipher = SSL_CIPHER_get_name(cipher);
    if (const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name)) session.server_name = name;
    session.resumed = SSL_session_reused(ssl) == 1;
    session.verify_result = SSL_get_verify_result(ssl);
    X509* leaf = SSL_get0_peer_certificate(ssl);
    if (!leaf) return session;
    session.peer_chain_der.push_back(to_der(leaf));
    // The chain of the client has no leaf; the one of the server does
    STACK_OF(X509)* chain = SSL_get_peer_cert_chain(ssl);
    for (int i = 0; i < sk_X509_num(chain); ++i) {
        X509* certificate = sk_X509_value(chain, i);
        if (i == 0 && X509_cmp(certificate, leaf) == 0) continue;
        session.peer_chain_der.push_back(to_der(certificate));
    }
    return session;
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

bool ssl_socket::readable_now() const {
    if (!read_ahead_.empty()) return true;
    if (SSL_pending(const_cast<ssl_socket*>(this)->ssl_stream_.native_handle()) > 0) return true;
    return tcp_socket::readable_now();
}

bool ssl_socket::is_secure() const {
    return true;
}

}

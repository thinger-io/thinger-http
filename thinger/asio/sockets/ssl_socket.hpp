#ifndef THINGER_ASIO_SSL_SOCKET_HPP
#define THINGER_ASIO_SSL_SOCKET_HPP

#include "tcp_socket.hpp"
#include "../ssl/client_verify.hpp"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

namespace thinger::asio {

class ssl_socket : public tcp_socket {
public:
    // constructors and destructors
    ssl_socket(const std::string& context, boost::asio::io_context& io_context,
               const std::shared_ptr<boost::asio::ssl::context>& ssl_context);
    ssl_socket(const std::string& context, const std::shared_ptr<tcp_socket>& socket,
               const std::shared_ptr<boost::asio::ssl::context>& ssl_context);
    ~ssl_socket() override;

    // socket control
    void close() override;
    bool requires_handshake() const override;
    // Without a host, the server handshake: it fails if an earlier session is resumed
    // without a client certificate where the host name requires one (see
    // certificate_manager::set_client_verify()), as no certificate is asked for then
    awaitable<boost::system::error_code> handshake(const std::string& host = "") override;

    // read operations
    awaitable<io_result> read_some(uint8_t buffer[], size_t max_size) override;
    awaitable<io_result> read(uint8_t buffer[], size_t size) override;
    awaitable<io_result> read(boost::asio::streambuf& buffer, size_t size) override;
    awaitable<io_result> read_until(boost::asio::streambuf& buffer, std::string_view delim) override;

    // wait
    awaitable<boost::system::error_code> wait(boost::asio::socket_base::wait_type type) override;
    awaitable<bool> peer_closed() override;

    // write operations
    awaitable<io_result> write(const uint8_t buffer[], size_t size) override;
    awaitable<io_result> write(std::string_view str) override;
    awaitable<io_result> write(const std::vector<boost::asio::const_buffer>& buffers) override;
    // Writes go through the TLS stream: never written without waiting
    size_t write_now(const std::vector<boost::asio::const_buffer>& buffers, boost::system::error_code& ec) override {
        return socket::write_now(buffers, ec);
    }

    // some getters to check the state
    bool is_secure() const override;
    // Decrypted data waiting to be read counts too
    bool readable_now() const override;
    // Once the handshake is done (none before, or once closed)
    std::optional<tls_session> tls() const override;

    // The certificate of the peer (its leaf), without copying it: null if it sent none (or
    // before the handshake). Owned by the connection: not to be used once the socket is closed
    // or destroyed (X509_up_ref() it to keep it)
    X509* peer_certificate() const;
    // The certificates the peer sent, without copying them, as OpenSSL keeps them: on the
    // server side without the leaf (see peer_certificate()), on the client side with it first.
    // Null if none; owned by the connection, as peer_certificate()
    STACK_OF(X509)* peer_chain() const;

    // OpenSSL handle of the TLS stream, e.g. to set the name a client verifies in the
    // certificate of the server before the handshake
    SSL* native_handle() { return ssl_stream_.native_handle(); }

    // Ask the client for its certificate with `policy` (see client_verify_policy), kept as long
    // as the socket; before the server handshake
    void set_client_verify(std::shared_ptr<const client_verify_policy> policy);

private:
    // Move up to `size` bytes of read_ahead_ to `buffer`; how many were moved
    size_t take_read_ahead(uint8_t buffer[], size_t size);
    // Move read_ahead_ to `buffer`
    void take_read_ahead(boost::asio::streambuf& buffer, size_t max_size);

    boost::asio::ssl::stream<boost::asio::ip::tcp::socket&> ssl_stream_;
    // Data already decrypted by peer_closed(), served before reading from the stream
    std::string read_ahead_;
    std::shared_ptr<boost::asio::ssl::context> ssl_context_;
    // Client certificates asked for (server side), referred to by the SSL
    std::shared_ptr<const client_verify_policy> client_verify_;
};

}

#endif

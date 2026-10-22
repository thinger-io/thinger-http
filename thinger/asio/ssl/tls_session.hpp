#ifndef THINGER_ASIO_SSL_TLS_SESSION_HPP
#define THINGER_ASIO_SSL_TLS_SESSION_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace thinger::asio {

// Client certificates on a TLS server
enum class client_verify : std::uint8_t {
    none,       // no CertificateRequest
    optional,   // CertificateRequest; a client without a certificate goes on
    required,   // CertificateRequest; the handshake fails without one (TLS 1.3: certificate_required)
};

// What a TLS server asks of its clients' certificates (see tcp_socket_server::enable_ssl())
struct client_verify_options {
    client_verify mode = client_verify::none;
    // CAs (PEM, one or more certificates) OpenSSL validates the chain of the client against
    // during the handshake: a certificate presented that does not validate fails the handshake,
    // in optional mode too. Empty: deferred validation, TLS accepts any well-formed chain (the
    // client still proves it holds the key of the leaf) and the application validates it from
    // tls_session::peer_chain_der (or ssl_socket::peer_certificate() and peer_chain())
    std::string trusted_cas;
    // Intermediate CAs allowed between the leaf and a trusted CA, as OpenSSL counts them
    // (SSL_set_verify_depth): only with trusted_cas
    int max_depth = 4;
    // Bytes of the certificate chain a client may send (SSL_set_max_cert_list): the memory a
    // client may use before it is authenticated
    std::size_t max_cert_list = std::size_t{16} * 1024;
};

// What the application may read of a TLS connection once its handshake is done (see
// socket::tls())
struct tls_session {
    std::string version;        // e.g. "TLSv1.3"
    std::string cipher;         // e.g. "TLS_AES_128_GCM_SHA256"
    std::string server_name;    // the name the client sent (SNI), "" if none
    bool resumed = false;       // an earlier session resumed (no certificate exchanged then)
    // Certificates of the peer (DER), its leaf first; empty when it sent none. A resumed
    // session keeps the leaf only: OpenSSL does not keep the rest of the chain in a ticket
    std::vector<std::string> peer_chain_der;
    // Result of the validation of the peer's chain (X509_V_OK, 0, if valid): meaningful only
    // when OpenSSL validated it (with trusted CAs), as with deferred validation it was
    // validated against no CA at all
    long verify_result = 0;
};

} // namespace thinger::asio

#endif // THINGER_ASIO_SSL_TLS_SESSION_HPP

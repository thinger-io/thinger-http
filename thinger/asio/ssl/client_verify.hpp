#ifndef THINGER_ASIO_SSL_CLIENT_VERIFY_HPP
#define THINGER_ASIO_SSL_CLIENT_VERIFY_HPP

#include "tls_session.hpp"
#include <array>
#include <memory>
#include <openssl/ssl.h>

namespace thinger::asio {

// The client certificate policy of a TLS listener (see client_verify_options), set on the SSL
// of each of its connections before the handshake, never on the context it listens with: that
// context may be shared with other listeners (e.g. the default certificate of the
// certificate_manager), which keep their own policies.
//
// Each connection gets a session id context of its own policy (a digest of its options), so a
// session is only resumed on a listener with the same policy: a session of a listener without
// trusted CAs is never resumed on one validating against them. The certificate_manager reads it
// from the SSL to keep it once the SNI callback switches the context (see sni_callback()).
class client_verify_policy {
public:
    // The policy of `options`: none (logged) if its trusted CAs cannot be read, or none
    // given in them
    static std::shared_ptr<const client_verify_policy> create(const client_verify_options& options);

    // What a listener without a policy uses for the hosts that ask for client certificates
    // (see certificate_manager::set_client_verify()): deferred validation, default limits
    static const client_verify_policy& deferred();

    ~client_verify_policy();
    client_verify_policy(const client_verify_policy&) = delete;
    client_verify_policy& operator=(const client_verify_policy&) = delete;

    const client_verify_options& options() const { return options_; }

    // Set the policy on the SSL of a connection before its handshake (nothing for mode none).
    // The SSL refers to it: it must outlive the handshake
    void apply(SSL* ssl) const;

    // Ask for a certificate in `mode` with this policy's CAs and limits: on the SSL being
    // set up, or from the SNI callback, once the context was switched (which drops the CAs the
    // SSL validated against)
    void set_mode(SSL* ssl, client_verify mode) const;

    // The policy set on an SSL with apply(), if any
    static const client_verify_policy* of(const SSL* ssl);

private:
    explicit client_verify_policy(client_verify_options options);

    client_verify_options options_;
    // The trusted CAs; none for deferred validation
    X509_STORE* store_ = nullptr;
    std::array<unsigned char, SSL_MAX_SID_CTX_LENGTH> session_id_context_{};
};

} // namespace thinger::asio

#endif // THINGER_ASIO_SSL_CLIENT_VERIFY_HPP

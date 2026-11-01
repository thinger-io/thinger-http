#ifndef THINGER_ASIO_SSL_CERTIFICATE_MANAGER_HPP
#define THINGER_ASIO_SSL_CERTIFICATE_MANAGER_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <set>
#include <mutex>
#include <optional>
#include <string_view>
#include <boost/asio/ssl/context.hpp>
#include <boost/system/error_code.hpp>
#include <openssl/ssl.h>
#include "tls_session.hpp"

namespace thinger::asio {

namespace detail {
    // Values by host name, exact or wildcard (see certificate_manager.cpp)
    template<typename T>
    class host_table;
}

// Versions of the TLS protocol
enum class tls_version : uint8_t { tls1_0, tls1_1, tls1_2, tls1_3 };

// TLS settings of a server context. The defaults are the "intermediate" configuration of
// Mozilla's Server Side TLS guidelines, the one Node and nginx default to: TLS 1.2 and 1.3,
// ECDHE key exchange (forward secrecy) with AES-GCM or ChaCha20-Poly1305 (AEAD), and the cipher
// preferred by the client (which knows whether it has AES in hardware). Older clients (TLS 1.0
// and 1.1: Android before 4.4.2, Java 7, IE 10 and older...) are refused.
struct tls_options {
    tls_version min_version = tls_version::tls1_2;
    tls_version max_version = tls_version::tls1_3;
    // Ciphers of TLS 1.2 and older, in OpenSSL's format; empty: left as they are (OpenSSL's
    // defaults on a new context)
    std::string ciphers =
        "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305";
    // Cipher suites of TLS 1.3, in OpenSSL's format; empty: left as they are
    std::string ciphersuites = "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256";
    // Choose the cipher in the order of the server (`ciphers`) instead of the client's
    bool prefer_server_ciphers = false;
    // Key exchange groups (curves), e.g. "X25519:prime256v1:secp384r1"; empty: OpenSSL's
    // defaults, which OpenSSL keeps up to date (from 3.5 they include the post-quantum
    // X25519MLKEM768)
    std::string groups;
    // OpenSSL security level (0 to 5); none: OpenSSL's default (as configured on the system)
    std::optional<int> security_level;
};

// Apply `options` to a context: false (logged), and the context left as it was, if OpenSSL
// refuses one of them (e.g. a cipher list without any cipher it knows)
bool apply_tls_options(boost::asio::ssl::context& context, const tls_options& options);

// Why a certificate and its key cannot be used (see use_certificate())
enum class certificate_error : uint8_t {
    none,
    invalid_certificate,    // the certificate, or a certificate of its chain, cannot be read
    invalid_private_key,    // the key cannot be read (an encrypted key included)
    key_mismatch,           // the key is not the one of the certificate
};

// Use a certificate on `context`: its chain (PEM: the certificate first, then the intermediate
// certificates sent along with it) and its private key (PEM, not encrypted: the context is
// given a password callback that answers none, so an encrypted key fails instead of asking
// for its password on the terminal). Checks that the key is the certificate's. Why not, if
// they cannot be used, and logged (never the key itself); the context may be half set then
certificate_error use_certificate(boost::asio::ssl::context& context, std::string_view certificate_chain,
                                  std::string_view private_key);

class certificate_manager;

// What the handshake of a server connection chose by the host name its client sent (SNI), kept
// by the connection once the handshake completes (see ssl_socket), to check the host names of
// its requests against it
struct tls_handshake_record {
    // The host name of the ClientHello, lowercase, without a trailing dot (not that of a
    // session resumed); empty without SNI
    std::string name;
    bool has_name = false;
    // The client certificate mode the handshake applied, and the one of its listener
    client_verify mode = client_verify::none;
    client_verify listener_mode = client_verify::none;
    // The manager that chose; none if no manager did (gone, or not of this context)
    std::weak_ptr<const certificate_manager> manager;

    // Whether a request for `host` (its Host, port removed) is misdirected (RFC 9110 15.5.20)
    // on this connection: `host` is not the handshake's name, and the manager would give it
    // another client certificate mode than the one applied or, with SNI, another certificate
    // entry (by its registered name: replacing a certificate changes none) than to that name.
    // Without SNI only the mode is compared: a client that named no host negotiated the
    // default certificate for any. The same name, or no manager, is never misdirected; another
    // name is, once the manager that chose is gone (fails closed)
    bool misdirected(std::string_view host) const;

    // Internal (ssl_socket): the record of the server handshake just completed on `ssl`, moved
    // out of it; empty (no manager) if no manager chose for it
    static tls_handshake_record take(SSL* ssl);
};

// Certificates of the TLS servers: a default one, and one per host name selected by SNI (exact
// or wildcard). The contexts it creates (from PEM, or the self-signed default) have its TLS
// options (see set_tls_options()), the certificate of a host included, so SNI cannot lower them.
// The versions, ciphers, options and security level of a handshake are those of the context
// the server listens with (the default one when it started listening): the context SNI switches
// to gives its certificate only (the SNI callback keeps the cipher list and security level of
// the listener, which OpenSSL would otherwise take from the context switched to), so neither
// can a context given ready-made lower them. Those keep their own settings otherwise
// (apply_tls_options() sets these options on one).
//
// A host name may also ask for client certificates in a mode of its own (see
// set_client_verify()), e.g. the host of the devices in a listener that asks for none. Every
// context it creates or is given has the same session id context, so that sessions with client
// certificates can be resumed, and the session cache of the listeners that use it (see
// listener_options::session_cache), on which they must agree.
//
// An application creates as many managers as it needs (one per listener, or one shared), each
// with its own certificates, options and modes. A manager is always held by a shared_ptr (built
// with std::make_shared): the SNI callback of its contexts reaches it through a weak_ptr kept
// with each context (and freed with it), never through a raw pointer, so a context may outlive
// its manager. The handshakes of a context whose manager is gone stay on that context (their
// listener's certificate and client certificate mode). One kept elsewhere than in a shared_ptr
// leaves its contexts without SNI the same way. A context belongs to one manager: given ready
// made to two, the SNI of its handshakes goes to the last one it was given to (not supported).
//
// The setters return an error_code: empty on success, `invalid_argument` (the reason logged)
// when what they are given cannot be used, and then nothing changes.
class certificate_manager : public std::enable_shared_from_this<certificate_manager> {
private:
    // Install the server name callback (see sni_callback()) with this manager as its argument,
    // the client hello callback (which keeps the host name asked for) and the session id
    // context on a context, before any handshake uses it: listeners do not write them into
    // contexts shared with running servers
    void prepare_context(boost::asio::ssl::context& context);

    // Create base SSL context with common settings, and the TLS options
    std::shared_ptr<boost::asio::ssl::context> create_base_ssl_context(const tls_options& options);
    
    // Create SSL context from certificate and key strings
    std::shared_ptr<boost::asio::ssl::context> create_ssl_context(
        const std::string& cert_chain,
        const std::string& private_key);
    
    // Certificates by host name: exact names and wildcards (*.domain), lowercase
    std::unique_ptr<detail::host_table<std::shared_ptr<boost::asio::ssl::context>>> ssl_contexts_;

    // Client certificate modes by host name, matched as the certificates
    std::unique_ptr<detail::host_table<client_verify>> client_verify_;
    
    // Default certificate context
    std::shared_ptr<boost::asio::ssl::context> default_context_;
    
    // Default hostname
    std::string default_host_;
    
    // TLS options of the contexts created
    tls_options options_;

    // What enable_legacy_protocols(true) changed, to undo it (the ciphers only while it decides
    // them), and whether set_server_ciphers() chose the ciphers
    struct legacy_change {
        tls_version min_version;
        std::optional<int> security_level;
        std::optional<std::string> ciphers;
    };
    std::optional<legacy_change> legacy_;
    bool ciphers_chosen_ = false;

    // Whether a self-signed default is generated when there is none (until
    // remove_default_certificate())
    bool self_signed_default_ = true;

    // Validate and set the options (with the lock held)
    bool update_tls_options(const tls_options& options);
    
    // Guards all the above: certificates are set and removed while handshakes look them up
    mutable std::mutex mutex_;
    
    // Generate self-signed certificate for development (with the lock held)
    void generate_self_signed_certificate();

    // The session cache claimed by the listeners of this manager (see claim_session_cache()):
    // none until the first claim, kept (with no holder) when the last one is released
    struct session_cache_claim {
        session_cache_options options;
        std::size_t holders = 0;
    };
    std::optional<session_cache_claim> session_cache_;

    // Write the claimed session cache, if any, on a context the manager holds from now on
    // (with the lock held)
    void apply_session_cache(boost::asio::ssl::context& context) const;

    // A listener opening on this manager claims its session cache, which applies it to every
    // context of the manager (those it holds and those it gets afterwards).
    // `invalid_argument` (logged), and nothing changes, if it is enabled with a size of 0, or
    // differs from the one held by other listeners. A setting with no holder left may be
    // replaced. The listener releases it once, when it stops; the contexts keep it
    friend class listener;
    boost::system::error_code claim_session_cache(const session_cache_options& options);
    void release_session_cache();

    // The context of a handshake: the certificate of the host name (SNI), or the default one
    // (none if there is no default yet)
    std::shared_ptr<boost::asio::ssl::context> select_context(const char* hostname) const;

    // Ask the client of a handshake for its certificate in the mode of its host name, if it
    // has one, or in the mode of the listener, with the CAs and limits of the listener (which
    // switching the context dropped). Nothing if neither asks: the SSL keeps its settings.
    // Recorded in the handshake of the SSL (see tls_handshake_record)
    void apply_client_verify(SSL* ssl, const char* hostname) const;

    // See tls_handshake_record::misdirected(), once the names differ
    friend struct tls_handshake_record;
    bool misdirected(const tls_handshake_record& handshake, std::string_view host) const;
    
public:
    // Always kept in a shared_ptr (std::make_shared<certificate_manager>()): see above
    certificate_manager();
    ~certificate_manager();

    certificate_manager(const certificate_manager&) = delete;
    certificate_manager& operator=(const certificate_manager&) = delete;
    certificate_manager(certificate_manager&&) = delete;
    certificate_manager& operator=(certificate_manager&&) = delete;

    // Host names are matched exactly, or by a wildcard "*.domain" for one more label (not
    // empty) in front of domain, an exact name first, case-insensitive (a trailing dot of the
    // name looked up ignored): a lookup probes one exact name and one wildcard, however many
    // there are. A name is a host name (labels of 1 to 63 letters, digits, hyphens or
    // underscores, 253 characters at most) or "*." and one: the setters refuse any other
    // (`invalid_argument`), patterns included.
    //
    // The certificate of a host name, from PEM (as use_certificate()) or as a context made
    // elsewhere. `invalid_argument` if the name is not one or the certificate cannot be used (the
    // host keeps the one it had). Replacing the one of the default host (see set_default_host())
    // replaces the default certificate too
    boost::system::error_code set_certificate(const std::string& hostname,
                                              const std::string& certificate,
                                              const std::string& private_key);

    boost::system::error_code set_certificate(const std::string& hostname,
                                              std::shared_ptr<boost::asio::ssl::context> ssl_context);

    // The certificate of the handshakes with the host name `hostname` (wildcards resolved): a
    // wildcard is also found by its own name ("*.domain")
    std::shared_ptr<boost::asio::ssl::context> get_certificate(const std::string& hostname) const;
    
    bool has_certificate(const std::string& hostname) const;
    
    // Remove the certificate registered with this name (a wildcard by its "*.domain"): false,
    // and nothing changes, if there is none
    bool remove_certificate(const std::string& hostname);
    
    // Default certificate management: the certificate of the handshakes without a host name
    // (SNI) or with one without a certificate. Replacing it applies to the next handshakes of
    // the servers already listening, as Node's setSecureContext() does; connections keep theirs.
    // Both versions return `invalid_argument` (logged) and keep the current one, and the
    // self-signed setting, when given nothing usable: PEM that is not valid, or a null context
    // (null does not drop the default: remove_default_certificate() does). Without one,
    // get_default_certificate() generates a self-signed certificate (for development) on its
    // first call, unless remove_default_certificate() was called since the last one set
    boost::system::error_code set_default_certificate(std::shared_ptr<boost::asio::ssl::context> ssl_context);
    boost::system::error_code set_default_certificate(const std::string& certificate, const std::string& private_key);
    std::shared_ptr<boost::asio::ssl::context> get_default_certificate() const;
    // The way to drop the default certificate: get_default_certificate() returns none (no
    // self-signed one is generated) until one is set again. Servers already listening keep the context they
    // listen with; their handshakes switch to no other default
    void remove_default_certificate();
    void set_default_host(const std::string& host);
    std::string get_default_host() const;
    
    // The names of the certificates, lowercase, as registered ("*.domain" for a wildcard)
    std::set<std::string> get_registered_hosts() const;

    // Client certificates of the handshakes whose host name (SNI) is `hostname`: exact or a
    // wildcard ("*.domain"), matched as the certificates. Other hosts keep the mode of their
    // listener (see listener_options::client_verify), whose trusted CAs and limits apply
    // (deferred validation on a listener that asks for none). Applies to the next handshakes of
    // the servers already listening. `invalid_argument` if `hostname` is not a host name. A
    // session is never resumed without a certificate on a host that requires one
    boost::system::error_code set_client_verify(const std::string& hostname, client_verify mode);
    // Back to the mode of the listener: false if `hostname` had no mode of its own
    bool remove_client_verify(const std::string& hostname);
    // The mode of the handshakes with the host name `hostname` (wildcards resolved), if it has
    // one of its own
    std::optional<client_verify> get_client_verify(const std::string& hostname) const;
    
    // TLS options of the contexts created from now on (those created before keep theirs): set
    // them before the certificates, and before listening. `invalid_argument` if OpenSSL refuses
    // them, then the current ones are kept. The defaults are those of tls_options. They replace
    // everything the two setters below changed
    boost::system::error_code set_tls_options(const tls_options& options);
    tls_options get_tls_options() const;

    // Deprecated, use set_tls_options(); they change the options as set_tls_options() would,
    // and are validated as well (in any order, they give the same options).
    //
    // The TLS 1.2 ciphers (empty: OpenSSL's defaults) and whether the server's order is
    // preferred: tls_options::ciphers and tls_options::prefer_server_ciphers.
    // `invalid_argument` if OpenSSL refuses the ciphers (the current ones are kept)
    boost::system::error_code set_server_ciphers(const std::string& ciphers, bool prefer_server_ciphers);
    // Allow TLS 1.0 and 1.1 (true): the minimum version is TLS 1.0, the OpenSSL security level
    // 0 (OpenSSL 3 refuses them above), and the ciphers OpenSSL's defaults (TLS 1.0 and 1.1 have
    // no AEAD cipher) unless set_server_ciphers() chose them. False undoes only what true did,
    // and does nothing if they were not allowed
    void enable_legacy_protocols(bool enable);
    
    // Server name callback of the handshakes, installed on every context the manager creates
    // or is given: switches to the certificate of the host name, or to the default certificate
    // when it is not the one the listener was created with, and asks for the client
    // certificate in the mode of the host name (see set_client_verify()). OpenSSL calls it with
    // or without SNI, from the context of the listener, with the weak_ptr to the manager kept
    // with that context as `arg`: if the manager is gone (or `arg` is null) the handshake stays
    // on the context it started with
    static int sni_callback(SSL* ssl, int* al, void* arg);
};

} // namespace thinger::asio

#endif // THINGER_ASIO_SSL_CERTIFICATE_MANAGER_HPP
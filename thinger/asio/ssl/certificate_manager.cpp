#include "certificate_manager.hpp"
#include "../../util/logger.hpp"
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <string_view>

namespace thinger::asio {

namespace {

    // Whether a context has some TLS 1.3 cipher suite (OpenSSL 3 drops the names it does not
    // know without failing)
    bool has_tls13_suites(SSL_CTX* handle) {
        auto* ciphers = SSL_CTX_get_ciphers(handle);
        for (int i = 0; i < sk_SSL_CIPHER_num(ciphers); ++i) {
            if (std::string_view(SSL_CIPHER_get_version(sk_SSL_CIPHER_value(ciphers, i))) == "TLSv1.3") return true;
        }
        return false;
    }

    int protocol_version(tls_version version) {
        switch (version) {
            case tls_version::tls1_0: return TLS1_VERSION;
            case tls_version::tls1_1: return TLS1_1_VERSION;
            case tls_version::tls1_2: return TLS1_2_VERSION;
            case tls_version::tls1_3: return TLS1_3_VERSION;
        }
        return 0;
    }

}

namespace {

    // Set the options on a context, the security level first (it decides which ciphers and
    // certificates are allowed): false (logged) at the first one OpenSSL refuses
    bool set_tls_options(SSL_CTX* handle, const tls_options& options) {
        if (options.security_level) {
            if (*options.security_level < 0 || *options.security_level > 5) {
                LOG_ERROR("invalid OpenSSL security level: {}", *options.security_level);
                return false;
            }
            SSL_CTX_set_security_level(handle, *options.security_level);
        }
        if (SSL_CTX_set_min_proto_version(handle, protocol_version(options.min_version)) != 1 ||
            SSL_CTX_set_max_proto_version(handle, protocol_version(options.max_version)) != 1) {
            LOG_ERROR("invalid TLS protocol versions");
            return false;
        }
        if (!options.ciphers.empty() && SSL_CTX_set_cipher_list(handle, options.ciphers.c_str()) != 1) {
            LOG_ERROR("invalid TLS cipher list: {}", options.ciphers);
            return false;
        }
        if (!options.ciphersuites.empty() &&
            (SSL_CTX_set_ciphersuites(handle, options.ciphersuites.c_str()) != 1 || !has_tls13_suites(handle))) {
            LOG_ERROR("invalid TLS 1.3 cipher suites: {}", options.ciphersuites);
            return false;
        }
        if (!options.groups.empty() && SSL_CTX_set1_groups_list(handle, options.groups.c_str()) != 1) {
            LOG_ERROR("invalid TLS groups: {}", options.groups);
            return false;
        }
        if (options.prefer_server_ciphers) {
            SSL_CTX_set_options(handle, SSL_OP_CIPHER_SERVER_PREFERENCE);
        } else {
            SSL_CTX_clear_options(handle, SSL_OP_CIPHER_SERVER_PREFERENCE);
        }
        return true;
    }

    // The TLS 1.2 (and older) ciphers of a handshake, in OpenSSL's format (empty if none)
    std::string tls12_cipher_list(const SSL* ssl) {
        std::string list;
        auto* ciphers = SSL_get_ciphers(ssl);
        for (int i = 0; i < sk_SSL_CIPHER_num(ciphers); ++i) {
            const SSL_CIPHER* cipher = sk_SSL_CIPHER_value(ciphers, i);
            if (std::string_view(SSL_CIPHER_get_version(cipher)) == "TLSv1.3") continue;
            if (!list.empty()) list += ':';
            list += SSL_CIPHER_get_name(cipher);
        }
        return list;
    }

    // Whether OpenSSL takes the options, tried on a context of its own
    bool valid_tls_options(const tls_options& options) {
        boost::asio::ssl::context test(boost::asio::ssl::context::tls_server);
        return set_tls_options(test.native_handle(), options);
    }

}

bool apply_tls_options(boost::asio::ssl::context& context, const tls_options& options) {
    // Tried first, so a context is never left half set
    return valid_tls_options(options) && set_tls_options(context.native_handle(), options);
}

certificate_manager::certificate_manager() 
    : ssl_contexts_(std::make_unique<regex_map<std::shared_ptr<boost::asio::ssl::context>>>()) {
}

certificate_manager& certificate_manager::instance() {
    static certificate_manager instance;
    return instance;
}

void certificate_manager::install_sni_callback(boost::asio::ssl::context& context) {
    SSL_CTX_set_tlsext_servername_callback(context.native_handle(), sni_callback);
}

std::shared_ptr<boost::asio::ssl::context> certificate_manager::create_base_ssl_context(const tls_options& options) {
    auto context = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
    context->set_options(boost::asio::ssl::context::single_dh_use);
    install_sni_callback(*context);
    // Validated when set (see set_tls_options()), so this fails only if OpenSSL changed its mind
    if (!apply_tls_options(*context, options)) {
        LOG_ERROR("cannot set the TLS options of a new context");
        return nullptr;
    }
    return context;
}

std::shared_ptr<boost::asio::ssl::context> certificate_manager::create_ssl_context(
    const std::string& cert_chain,
    const std::string& private_key) {
    
    tls_options options;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        options = options_;
    }
    auto context = create_base_ssl_context(options);
    if (!context) return nullptr;

    boost::system::error_code ec;
    context->use_certificate_chain(boost::asio::buffer(cert_chain.data(), cert_chain.size()), ec);
    if (ec) {
        LOG_ERROR("Cannot set certificate chain: {}", ec.message());
        return nullptr;
    }

    context->use_private_key(boost::asio::buffer(private_key.data(), private_key.size()),
                           boost::asio::ssl::context::pem, ec);
    if (ec) {
        LOG_ERROR("Cannot set private key: {}", ec.message());
        return nullptr;
    }

    return context;
}

bool certificate_manager::set_certificate(const std::string& hostname, 
                                         const std::string& certificate, 
                                         const std::string& private_key) {
    LOG_INFO("Setting SSL certificate for domain: {}", hostname);
    return set_certificate(hostname, create_ssl_context(certificate, private_key));
}

bool certificate_manager::set_certificate(const std::string& hostname,
                                         std::shared_ptr<boost::asio::ssl::context> ssl_context) {
    LOG_INFO("Setting SSL certificate for domain: {}", hostname);
    
    if (hostname.empty() || !ssl_context) {
        return false;
    }
    // It may become the default one (default host)
    install_sni_callback(*ssl_context);
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string computed_hostname = hostname;
    
    // Check if the hostname is a wildcard (e.g., *.example.com)
    static std::regex wildcard_regex(R"(^\*\.(.*)$)");
    std::smatch match;
    if (std::regex_search(hostname, match, wildcard_regex)) {
        // Get domain (e.g., example.com)
        std::string wildcard_domain = match[1].str();
        // Replace domain dots with "\." for the regex
        wildcard_domain = std::regex_replace(wildcard_domain, std::regex("\\."), "\\.");
        // Compute wildcard regex hostname (std::regex doesn't support named groups)
        // This will match any subdomain followed by the domain
        computed_hostname = "^[^.]+\\." + wildcard_domain + "$";
        LOG_DEBUG("Computed wildcard certificate regex: {}", computed_hostname);
    }
    
    ssl_contexts_->set(computed_hostname, ssl_context, hostname);
    
    if (default_host_ == hostname) {
        LOG_INFO("Overriding default SSL certificate for domain: {}", computed_hostname);
        default_context_ = ssl_context;
    }
    
    return true;
}

std::shared_ptr<boost::asio::ssl::context> certificate_manager::get_certificate(const std::string& hostname) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto cert = ssl_contexts_->get(hostname);
    if (cert.has_value()) {
        return cert.value();
    }
    return nullptr;
}

bool certificate_manager::has_certificate(const std::string& hostname) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ssl_contexts_->has(hostname);
}

bool certificate_manager::remove_certificate(const std::string& hostname) {
    LOG_INFO("Removing SSL certificate: {}", hostname);
    std::lock_guard<std::mutex> lock(mutex_);
    ssl_contexts_->erase(hostname);
    return true;
}

void certificate_manager::set_default_certificate(std::shared_ptr<boost::asio::ssl::context> ssl_context) {
    LOG_INFO("Setting default SSL certificate");
    if (ssl_context) install_sni_callback(*ssl_context);
    std::lock_guard<std::mutex> lock(mutex_);
    default_context_ = ssl_context;
}

bool certificate_manager::set_default_certificate(const std::string& certificate, const std::string& private_key) {
    auto context = create_ssl_context(certificate, private_key);
    if (!context) return false;
    set_default_certificate(std::move(context));
    return true;
}

std::shared_ptr<boost::asio::ssl::context> certificate_manager::get_default_certificate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // If no default certificate is set, generate a self-signed one
    if (!default_context_) {
        LOG_WARNING("No default SSL certificate configured, generating self-signed certificate for development use");
        const_cast<certificate_manager*>(this)->generate_self_signed_certificate();
    }
    
    return default_context_;
}

void certificate_manager::set_default_host(const std::string& host) {
    std::lock_guard<std::mutex> lock(mutex_);
    default_host_ = host;
}

std::string certificate_manager::get_default_host() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return default_host_;
}

std::set<std::string> certificate_manager::get_registered_hosts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ssl_contexts_->keys();
}

bool certificate_manager::update_tls_options(const tls_options& options) {
    // Tried on a context first, so the contexts created afterwards cannot fail
    if (!valid_tls_options(options)) return false;
    options_ = options;
    return true;
}

bool certificate_manager::set_tls_options(const tls_options& options) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!update_tls_options(options)) return false;
    // They replace what the setters below changed
    legacy_.reset();
    ciphers_chosen_ = false;
    return true;
}

tls_options certificate_manager::get_tls_options() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return options_;
}

bool certificate_manager::set_server_ciphers(const std::string& ciphers, bool prefer_server_ciphers) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto options = options_;
    options.ciphers = ciphers;
    options.prefer_server_ciphers = prefer_server_ciphers;
    if (!update_tls_options(options)) return false;
    ciphers_chosen_ = true;
    // chosen here: the legacy protocols no longer decide them
    if (legacy_) legacy_->ciphers.reset();
    return true;
}

void certificate_manager::enable_legacy_protocols(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enable == legacy_.has_value()) return;
    auto options = options_;
    if (enable) {
        legacy_change change{options.min_version, options.security_level, std::nullopt};
        options.min_version = tls_version::tls1_0;
        options.security_level = 0;
        // TLS 1.0 and 1.1 have no AEAD cipher: OpenSSL's defaults, unless they were chosen
        if (!ciphers_chosen_) {
            change.ciphers = options.ciphers;
            options.ciphers.clear();
        }
        if (update_tls_options(options)) legacy_ = std::move(change);
    } else {
        // What enabling them changed, and only that
        options.min_version = legacy_->min_version;
        options.security_level = legacy_->security_level;
        if (legacy_->ciphers) options.ciphers = *legacy_->ciphers;
        if (update_tls_options(options)) legacy_.reset();
    }
}

void certificate_manager::generate_self_signed_certificate() {
    // Generate RSA key
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY* pkey = nullptr;
    
    if (ctx) {
        if (EVP_PKEY_keygen_init(ctx) > 0) {
            if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) > 0) {
                EVP_PKEY_keygen(ctx, &pkey);
            }
        }
        EVP_PKEY_CTX_free(ctx);
    }
    
    if (!pkey) {
        LOG_ERROR("Failed to generate RSA key for self-signed certificate");
        return;
    }
    
    // Generate certificate
    X509* x509 = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 31536000L); // 1 year
    
    X509_set_pubkey(x509, pkey);
    
    // Subject, and issuer as it is self-signed: a name of its own, as recent OpenSSL versions
    // give the name of a certificate as const
    X509_NAME* name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, 
                              reinterpret_cast<const unsigned char*>("US"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, 
                              reinterpret_cast<const unsigned char*>("Thinger Development"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, 
                              reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    
    X509_set_subject_name(x509, name);
    X509_set_issuer_name(x509, name);
    X509_NAME_free(name);
    X509_sign(x509, pkey, EVP_sha256());
    
    // Create SSL context
    auto context = create_base_ssl_context(options_);
    if (!context) {
        X509_free(x509);
        EVP_PKEY_free(pkey);
        return;
    }
    
    // Set certificate and key
    if (SSL_CTX_use_certificate(context->native_handle(), x509) != 1) {
        LOG_ERROR("Failed to use certificate for self-signed cert");
    } else if (SSL_CTX_use_PrivateKey(context->native_handle(), pkey) != 1) {
        LOG_ERROR("Failed to use private key for self-signed cert");
    } else {
        default_context_ = context;
        LOG_INFO("Generated self-signed certificate for development use (CN=localhost)");
    }
    
    // Cleanup
    X509_free(x509);
    EVP_PKEY_free(pkey);
}

std::shared_ptr<boost::asio::ssl::context> certificate_manager::select_context(const char* hostname) const {
    if (hostname != nullptr) {
        LOG_DEBUG("SNI connection for: {}", hostname);
        if (auto certificate = get_certificate(hostname)) return certificate;
        // At debug level: any client (or scanner) can send names without a certificate
        LOG_DEBUG("Using default server certificate for hostname: {}", hostname);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return default_context_;
}

// SNI callback implementation
int certificate_manager::sni_callback(SSL* ssl, int* al, void* arg) {
    if (!ssl) return SSL_TLSEXT_ERR_OK;

    // Get SNI server name (none if the client sent none)
    const char* hostname = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);

    // Called from OpenSSL: nothing may be thrown through it
    try {
        // The default certificate may have been replaced since the listener took it. The
        // connection holds a reference to the context it switches to, which outlives a later
        // replacement
        auto context = certificate_manager::instance().select_context(hostname);
        if (context && context->native_handle() != SSL_get_SSL_CTX(ssl)) {
            // OpenSSL takes the certificate of the context switched to, but also its cipher
            // list and security level: the handshake keeps those of the listener (as its
            // versions and options), so a context given ready-made cannot lower them
            auto ciphers = tls12_cipher_list(ssl);
            int level = SSL_get_security_level(ssl);
            if (SSL_set_SSL_CTX(ssl, context->native_handle()) == nullptr ||
                (!ciphers.empty() && SSL_set_cipher_list(ssl, ciphers.c_str()) != 1)) {
                LOG_ERROR("cannot set the certificate for hostname {}", hostname ? hostname : "");
                if (al) *al = SSL_AD_INTERNAL_ERROR;
                return SSL_TLSEXT_ERR_ALERT_FATAL;
            }
            SSL_set_security_level(ssl, level);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("cannot select the certificate for hostname {}: {}", hostname ? hostname : "", e.what());
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    
    return SSL_TLSEXT_ERR_OK;
}

} // namespace thinger::asio
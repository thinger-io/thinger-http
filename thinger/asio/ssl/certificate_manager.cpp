#include "certificate_manager.hpp"
#include "../../util/logger.hpp"
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace thinger::asio {

certificate_manager::certificate_manager() 
    : ssl_contexts_(std::make_unique<regex_map<std::shared_ptr<boost::asio::ssl::context>>>()) {
}

certificate_manager& certificate_manager::instance() {
    static certificate_manager instance;
    return instance;
}

std::shared_ptr<boost::asio::ssl::context> certificate_manager::create_base_ssl_context(const tls_settings& settings) {
    auto context = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
    context->set_options(boost::asio::ssl::context::single_dh_use);
    
    // Enable older TLS versions if configured
    if (settings.legacy_protocols) {
        SSL_CTX_set_security_level(context->native_handle(), 0);
    }
    
    // Set cipher list if configured
    if (!settings.ciphers.empty()) {
        SSL_CTX_set_cipher_list(context->native_handle(), settings.ciphers.c_str());
        if (settings.prefer_server_ciphers) {
            SSL_CTX_set_options(context->native_handle(), SSL_OP_CIPHER_SERVER_PREFERENCE);
        }
    }
    
    return context;
}

std::shared_ptr<boost::asio::ssl::context> certificate_manager::create_ssl_context(
    const std::string& cert_chain,
    const std::string& private_key) {
    
    tls_settings settings;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings = settings_;
    }
    auto context = create_base_ssl_context(settings);

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

void certificate_manager::set_server_ciphers(const std::string& ciphers, bool prefer_server_ciphers) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_.ciphers = ciphers;
    settings_.prefer_server_ciphers = prefer_server_ciphers;
}

void certificate_manager::enable_legacy_protocols(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_.legacy_protocols = enable;
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
    
    // Set subject
    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, 
                              reinterpret_cast<const unsigned char*>("US"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, 
                              reinterpret_cast<const unsigned char*>("Thinger Development"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, 
                              reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    
    X509_set_issuer_name(x509, name);
    X509_sign(x509, pkey, EVP_sha256());
    
    // Create SSL context
    auto context = create_base_ssl_context(settings_);
    
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
        LOG_WARNING("Using default server certificate for hostname: {}", hostname);
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
        if (context && context->native_handle() != SSL_get_SSL_CTX(ssl) &&
            SSL_set_SSL_CTX(ssl, context->native_handle()) == nullptr) {
            LOG_ERROR("cannot set the certificate for hostname {}", hostname ? hostname : "");
            if (al) *al = SSL_AD_INTERNAL_ERROR;
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("cannot select the certificate for hostname {}: {}", hostname ? hostname : "", e.what());
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    
    return SSL_TLSEXT_ERR_OK;
}

} // namespace thinger::asio
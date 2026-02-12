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

std::shared_ptr<boost::asio::ssl::context> certificate_manager::create_base_ssl_context() {
    auto context = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
    context->set_options(boost::asio::ssl::context::single_dh_use);
    
    // Enable older TLS versions if configured
    if (enable_legacy_protocols_) {
        SSL_CTX_set_security_level(context->native_handle(), 0);
    }
    
    // Set cipher list if configured
    if (!server_ciphers_.empty()) {
        SSL_CTX_set_cipher_list(context->native_handle(), server_ciphers_.c_str());
        if (prefer_server_ciphers_) {
            SSL_CTX_set_options(context->native_handle(), SSL_OP_CIPHER_SERVER_PREFERENCE);
        }
    }
    
    return context;
}

std::shared_ptr<boost::asio::ssl::context> certificate_manager::create_ssl_context(
    const std::string& cert_chain,
    const std::string& private_key) {
    
    auto context = create_base_ssl_context();
    
    try {
        context->use_certificate_chain(boost::asio::buffer(cert_chain.data(), cert_chain.size()));
        context->use_private_key(boost::asio::buffer(private_key.data(), private_key.size()), 
                               boost::asio::ssl::context::pem);
        return context;
    } catch (const std::exception& e) {
        LOG_ERROR("Cannot set certificates: {}", e.what());
    } catch (...) {
        LOG_ERROR("Cannot set certificates: unknown error");
    }
    
    return nullptr;
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

void certificate_manager::set_default_certificate(const std::string& certificate, const std::string& private_key) {
    set_default_certificate(create_ssl_context(certificate, private_key));
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

const std::string& certificate_manager::get_default_host() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return default_host_;
}

std::set<std::string> certificate_manager::get_registered_hosts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ssl_contexts_->keys();
}

void certificate_manager::set_server_ciphers(const std::string& ciphers, bool prefer_server_ciphers) {
    std::lock_guard<std::mutex> lock(mutex_);
    server_ciphers_ = ciphers;
    prefer_server_ciphers_ = prefer_server_ciphers;
}

void certificate_manager::enable_legacy_protocols(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    enable_legacy_protocols_ = enable;
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
    auto context = create_base_ssl_context();
    
    try {
        // Set certificate and key
        if (SSL_CTX_use_certificate(context->native_handle(), x509) != 1) {
            throw std::runtime_error("Failed to use certificate");
        }
        
        if (SSL_CTX_use_PrivateKey(context->native_handle(), pkey) != 1) {
            throw std::runtime_error("Failed to use private key");
        }
        
        default_context_ = context;
        LOG_INFO("Generated self-signed certificate for development use (CN=localhost)");
        
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to set self-signed certificate: {}", e.what());
    }
    
    // Cleanup
    X509_free(x509);
    EVP_PKEY_free(pkey);
}

// SNI callback implementation
int certificate_manager::sni_callback(SSL* ssl, int* al, void* arg) {
    // Get SNI server name
    const char* hostname = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    
    if (hostname != nullptr) {
        LOG_DEBUG("SNI connection for: {}", hostname);
        
        auto& mgr = certificate_manager::instance();
        auto certificate = mgr.get_certificate(hostname);
        
        if (certificate) {
            auto ssl_ctx = certificate->native_handle();
            SSL_set_SSL_CTX(ssl, ssl_ctx);
        } else {
            LOG_WARNING("Using default server certificate for hostname: {}", hostname);
        }
    }
    
    return SSL_TLSEXT_ERR_OK;
}

} // namespace thinger::asio
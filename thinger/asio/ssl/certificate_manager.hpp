#ifndef THINGER_ASIO_SSL_CERTIFICATE_MANAGER_HPP
#define THINGER_ASIO_SSL_CERTIFICATE_MANAGER_HPP

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <string>
#include <set>
#include <regex>
#include <mutex>
#include <optional>
#include <boost/asio/ssl/context.hpp>

namespace thinger::asio {

// Forward declaration
template<typename T>
class regex_map;

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

// Certificates of the TLS servers: a default one, and one per host name selected by SNI (exact
// or wildcard). The contexts it creates (from PEM, or the self-signed default) have its TLS
// options (see set_tls_options()), the certificate of a host included, so SNI cannot lower them.
// The versions, ciphers, options and security level of a handshake are those of the context
// the server listens with (the default one when it started listening): the context SNI switches
// to gives its certificate only (the SNI callback keeps the cipher list and security level of
// the listener, which OpenSSL would otherwise take from the context switched to), so neither
// can a context given ready-made lower them. Those keep their own settings otherwise
// (apply_tls_options() sets these options on one).
class certificate_manager {
private:
    // Private constructor for singleton
    certificate_manager();
    
    // Delete copy and move operations
    certificate_manager(const certificate_manager&) = delete;
    certificate_manager& operator=(const certificate_manager&) = delete;
    certificate_manager(certificate_manager&&) = delete;
    certificate_manager& operator=(certificate_manager&&) = delete;
    
    // Install the server name callback (see sni_callback()) on a context, before any
    // handshake uses it: listeners do not write it into contexts shared with running servers
    static void install_sni_callback(boost::asio::ssl::context& context);

    // Create base SSL context with common settings, and the TLS options
    static std::shared_ptr<boost::asio::ssl::context> create_base_ssl_context(const tls_options& options);
    
    // Create SSL context from certificate and key strings
    std::shared_ptr<boost::asio::ssl::context> create_ssl_context(
        const std::string& cert_chain,
        const std::string& private_key);
    
    // Certificate storage with regex support for wildcards
    std::unique_ptr<regex_map<std::shared_ptr<boost::asio::ssl::context>>> ssl_contexts_;
    
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

    // Validate and set the options (with the lock held)
    bool update_tls_options(const tls_options& options);
    
    // Guards all the above: certificates are set and removed while handshakes look them up
    mutable std::mutex mutex_;
    
    // Generate self-signed certificate for development (with the lock held)
    void generate_self_signed_certificate();

    // The context of a handshake: the certificate of the host name (SNI), or the default one
    // (none if there is no default yet)
    std::shared_ptr<boost::asio::ssl::context> select_context(const char* hostname) const;
    
public:
    // Singleton instance
    static certificate_manager& instance();
    
    // Certificate management
    bool set_certificate(const std::string& hostname, 
                        const std::string& certificate, 
                        const std::string& private_key);
    
    bool set_certificate(const std::string& hostname,
                        std::shared_ptr<boost::asio::ssl::context> ssl_context);
    
    std::shared_ptr<boost::asio::ssl::context> get_certificate(const std::string& hostname) const;
    
    bool has_certificate(const std::string& hostname) const;
    
    bool remove_certificate(const std::string& hostname);
    
    // Default certificate management: the certificate of the handshakes without a host name
    // (SNI) or with one without a certificate. Replacing it applies to the next handshakes of
    // the servers already listening, as Node's setSecureContext() does; connections keep theirs.
    // The PEM version keeps the current one and returns false if they are not valid
    void set_default_certificate(std::shared_ptr<boost::asio::ssl::context> ssl_context);
    bool set_default_certificate(const std::string& certificate, const std::string& private_key);
    std::shared_ptr<boost::asio::ssl::context> get_default_certificate() const;
    void set_default_host(const std::string& host);
    std::string get_default_host() const;
    
    // Get all registered hostnames
    std::set<std::string> get_registered_hosts() const;
    
    // TLS options of the contexts created from now on (those created before keep theirs): set
    // them before the certificates, and before listening. False if OpenSSL refuses them, then
    // the current ones are kept. The defaults are those of tls_options. They replace everything
    // the two setters below changed
    bool set_tls_options(const tls_options& options);
    tls_options get_tls_options() const;

    // Deprecated, use set_tls_options(); they change the options as set_tls_options() would,
    // and are validated as well (in any order, they give the same options).
    //
    // The TLS 1.2 ciphers (empty: OpenSSL's defaults) and whether the server's order is
    // preferred: tls_options::ciphers and tls_options::prefer_server_ciphers. False if OpenSSL
    // refuses the ciphers (the current ones are kept)
    bool set_server_ciphers(const std::string& ciphers, bool prefer_server_ciphers);
    // Allow TLS 1.0 and 1.1 (true): the minimum version is TLS 1.0, the OpenSSL security level
    // 0 (OpenSSL 3 refuses them above), and the ciphers OpenSSL's defaults (TLS 1.0 and 1.1 have
    // no AEAD cipher) unless set_server_ciphers() chose them. False undoes only what true did,
    // and does nothing if they were not allowed
    void enable_legacy_protocols(bool enable);
    
    // Server name callback of the handshakes, installed on every context the manager creates
    // or is given: switches to the certificate of the host name, or to the default certificate
    // when it is not the one the listener was created with. OpenSSL calls it with or without
    // SNI, from the context of the listener
    static int sni_callback(SSL* ssl, int* al, void* arg);
};

// Simple regex map implementation for certificate hostname matching
template<typename T>
class regex_map {
private:
    struct regex_entry {
        std::string key;
        T value;
        std::regex pattern;
        
        regex_entry(const std::string& k, const T& v, const std::regex& p) 
            : key(k), value(v), pattern(p) {}
    };
    
    std::vector<regex_entry> regex_items_;
    std::unordered_map<std::string, T> non_regex_items_;
    mutable std::mutex mutex_;
    
public:
    void set(const std::string& key, const T& value, const std::string& original = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Store the original key
        std::string store_key = original.empty() ? key : original;
        
        // Check if key contains regex patterns
        bool has_regex_chars = key.find_first_of("^$.*+?{}[]|()\\") != std::string::npos;
        
        if (!has_regex_chars) {
            // Store as non-regex item
            non_regex_items_[store_key] = value;
        } else {
            // Remove any existing regex entry with the same original key
            regex_items_.erase(
                std::remove_if(regex_items_.begin(), regex_items_.end(),
                    [&store_key](const regex_entry& e) { return e.key == store_key; }),
                regex_items_.end()
            );
            
            // Treat as regex pattern
            try {
                regex_items_.emplace_back(store_key, value, std::regex(key));
            } catch (const std::regex_error& e) {
                // If regex compilation fails, skip this entry
                return;
            }
        }
    }
    
    std::optional<T> get(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // First try exact match in non-regex items
        auto it = non_regex_items_.find(key);
        if (it != non_regex_items_.end()) {
            return it->second;
        }
        
        // Check if the key itself is a stored regex pattern (like *.example.com)
        for (const auto& item : regex_items_) {
            if (item.key == key) {
                return item.value;
            }
        }
        
        // Then try regex matches
        for (const auto& item : regex_items_) {
            if (std::regex_match(key, item.pattern)) {
                return item.value;
            }
        }
        
        return std::nullopt;
    }
    
    bool has(const std::string& key) const {
        return get(key).has_value();
    }
    
    void erase(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Try to erase from non-regex items first
        auto it = non_regex_items_.find(key);
        if (it != non_regex_items_.end()) {
            non_regex_items_.erase(it);
            return;
        }
        
        // Then try regex items
        regex_items_.erase(
            std::remove_if(regex_items_.begin(), regex_items_.end(),
                [&key](const regex_entry& e) { return e.key == key; }),
            regex_items_.end()
        );
    }
    
    std::set<std::string> keys() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::set<std::string> result;
        
        // Add non-regex keys
        for (const auto& pair : non_regex_items_) {
            result.insert(pair.first);
        }
        
        // Add regex keys
        for (const auto& entry : regex_items_) {
            result.insert(entry.key);
        }
        
        return result;
    }
};

} // namespace thinger::asio

#endif // THINGER_ASIO_SSL_CERTIFICATE_MANAGER_HPP
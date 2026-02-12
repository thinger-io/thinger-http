#ifndef THINGER_ASIO_SSL_CERTIFICATE_MANAGER_HPP
#define THINGER_ASIO_SSL_CERTIFICATE_MANAGER_HPP

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

class certificate_manager {
private:
    // Private constructor for singleton
    certificate_manager();
    
    // Delete copy and move operations
    certificate_manager(const certificate_manager&) = delete;
    certificate_manager& operator=(const certificate_manager&) = delete;
    certificate_manager(certificate_manager&&) = delete;
    certificate_manager& operator=(certificate_manager&&) = delete;
    
    // Create base SSL context with common settings
    std::shared_ptr<boost::asio::ssl::context> create_base_ssl_context();
    
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
    
    // SSL/TLS configuration
    std::string server_ciphers_;
    bool prefer_server_ciphers_ = false;
    bool enable_legacy_protocols_ = false;
    
    // Thread safety
    mutable std::mutex mutex_;
    
    // Generate self-signed certificate for development
    void generate_self_signed_certificate();
    
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
    
    // Default certificate management
    void set_default_certificate(std::shared_ptr<boost::asio::ssl::context> ssl_context);
    void set_default_certificate(const std::string& certificate, const std::string& private_key);
    std::shared_ptr<boost::asio::ssl::context> get_default_certificate() const;
    void set_default_host(const std::string& host);
    const std::string& get_default_host() const;
    
    // Get all registered hostnames
    std::set<std::string> get_registered_hosts() const;
    
    // SSL/TLS configuration
    void set_server_ciphers(const std::string& ciphers, bool prefer_server_ciphers);
    void enable_legacy_protocols(bool enable);
    
    // SNI callback for SSL handshake
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
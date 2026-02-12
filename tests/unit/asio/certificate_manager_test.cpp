#include <catch2/catch_test_macros.hpp>
#include <thinger/asio/ssl/certificate_manager.hpp>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <thread>
#include <vector>

using namespace thinger::asio;

// Helper function to generate a self-signed certificate for testing
std::pair<std::string, std::string> generate_test_certificate(const std::string& common_name) {
    // Generate RSA key using OpenSSL 3 API
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
    
    // Generate certificate
    X509* x509 = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 31536000L); // 1 year
    
    X509_set_pubkey(x509, pkey);
    
    // Set subject
    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, (unsigned char*)"US", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (unsigned char*)common_name.c_str(), -1, -1, 0);
    
    X509_set_issuer_name(x509, name);
    X509_sign(x509, pkey, EVP_sha256());
    
    // Convert to PEM
    BIO* cert_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(cert_bio, x509);
    
    BIO* key_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(key_bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    
    // Get strings
    char* cert_data;
    long cert_len = BIO_get_mem_data(cert_bio, &cert_data);
    std::string cert_pem(cert_data, cert_len);
    
    char* key_data;
    long key_len = BIO_get_mem_data(key_bio, &key_data);
    std::string key_pem(key_data, key_len);
    
    // Cleanup
    BIO_free_all(cert_bio);
    BIO_free_all(key_bio);
    X509_free(x509);
    EVP_PKEY_free(pkey);
    
    return {cert_pem, key_pem};
}

// Helper to clear all certificates
void clear_all_certificates() {
    auto& mgr = certificate_manager::instance();
    auto hosts = mgr.get_registered_hosts();
    for (const auto& host : hosts) {
        mgr.remove_certificate(host);
    }
    // Also clear default certificate
    mgr.set_default_certificate(nullptr);
}

// Helper to get the Common Name from an SSL context
std::string get_certificate_cn(const std::shared_ptr<boost::asio::ssl::context>& ctx) {
    if (!ctx) return "";
    
    SSL_CTX* ssl_ctx = ctx->native_handle();
    X509* cert = SSL_CTX_get0_certificate(ssl_ctx);
    if (!cert) return "";
    
    X509_NAME* subject = X509_get_subject_name(cert);
    char cn[256];
    if (X509_NAME_get_text_by_NID(subject, NID_commonName, cn, sizeof(cn)) > 0) {
        return {cn};
    }
    return "";
}

TEST_CASE("Certificate Manager Tests", "[certificate_manager]") {
    // Clear certificates before each test
    clear_all_certificates();
    
    SECTION("Singleton Instance") {
        auto& mgr1 = certificate_manager::instance();
        auto& mgr2 = certificate_manager::instance();
        REQUIRE(&mgr1 == &mgr2);
    }
    
    SECTION("Set and Get Certificate") {
        auto& mgr = certificate_manager::instance();
        auto [cert, key] = generate_test_certificate("example.com");
        
        REQUIRE(mgr.set_certificate("example.com", cert, key));
        
        auto ctx = mgr.get_certificate("example.com");
        REQUIRE(ctx != nullptr);
        REQUIRE(mgr.has_certificate("example.com"));
        
        // Verify it's the correct certificate
        REQUIRE(get_certificate_cn(ctx) == "example.com");
    }
    
    SECTION("Certificate Not Found") {
        auto& mgr = certificate_manager::instance();
        
        auto ctx = mgr.get_certificate("nonexistent.com");
        REQUIRE(ctx == nullptr);
        REQUIRE_FALSE(mgr.has_certificate("nonexistent.com"));
    }
    
    SECTION("Wildcard Certificate") {
        auto& mgr = certificate_manager::instance();
        auto [cert, key] = generate_test_certificate("*.example.com");
        
        REQUIRE(mgr.set_certificate("*.example.com", cert, key));
        
        // Test various subdomains
        REQUIRE(mgr.get_certificate("www.example.com") != nullptr);
        REQUIRE(mgr.get_certificate("api.example.com") != nullptr);
        REQUIRE(mgr.get_certificate("test.example.com") != nullptr);
        
        // Root domain should not match wildcard
        REQUIRE(mgr.get_certificate("example.com") == nullptr);
        
        // Different domain should not match
        REQUIRE(mgr.get_certificate("www.different.com") == nullptr);
    }
    
    SECTION("Exact Match Priority") {
        auto& mgr = certificate_manager::instance();
        auto [wildcard_cert, wildcard_key] = generate_test_certificate("*.example.com");
        auto [exact_cert, exact_key] = generate_test_certificate("www.example.com");
        
        // Set both wildcard and exact match
        REQUIRE(mgr.set_certificate("*.example.com", wildcard_cert, wildcard_key));
        REQUIRE(mgr.set_certificate("www.example.com", exact_cert, exact_key));
        
        // Check registered hosts
        auto hosts = mgr.get_registered_hosts();
        REQUIRE(hosts.find("*.example.com") != hosts.end());
        REQUIRE(hosts.find("www.example.com") != hosts.end());
        
        // Exact match should take priority
        auto ctx_www = mgr.get_certificate("www.example.com");
        REQUIRE(ctx_www != nullptr);
        
        // Debug: Check if both certificates are different
        auto ctx_wildcard = mgr.get_certificate("*.example.com");
        REQUIRE(ctx_wildcard != nullptr);
        INFO("www.example.com cert CN: " << get_certificate_cn(ctx_www));
        INFO("*.example.com cert CN: " << get_certificate_cn(ctx_wildcard));
        INFO("Are they the same cert? " << (ctx_www == ctx_wildcard ? "YES" : "NO"));
        
        REQUIRE(get_certificate_cn(ctx_www) == "www.example.com");
        
        // Other subdomains should use wildcard
        auto ctx_api = mgr.get_certificate("api.example.com");
        REQUIRE(ctx_api != nullptr);
        REQUIRE(get_certificate_cn(ctx_api) == "*.example.com");
    }
    
    SECTION("Default Certificate") {
        auto& mgr = certificate_manager::instance();
        auto [cert, key] = generate_test_certificate("default");
        
        mgr.set_default_certificate(cert, key);
        
        auto ctx = mgr.get_default_certificate();
        REQUIRE(ctx != nullptr);
        REQUIRE(get_certificate_cn(ctx) == "default");
    }
    
    SECTION("Auto-generated Default Certificate") {
        auto& mgr = certificate_manager::instance();
        
        // Clear any existing default certificate
        mgr.set_default_certificate(nullptr);
        
        // Get default should generate a self-signed certificate
        auto ctx = mgr.get_default_certificate();
        REQUIRE(ctx != nullptr);
        
        // Verify it's a localhost certificate
        REQUIRE(get_certificate_cn(ctx) == "localhost");
        
        // Getting it again should return the same certificate
        auto ctx2 = mgr.get_default_certificate();
        REQUIRE(ctx2 != nullptr);
        REQUIRE(ctx == ctx2);
    }
    
    SECTION("Default Host") {
        auto& mgr = certificate_manager::instance();
        
        mgr.set_default_host("example.com");
        REQUIRE(mgr.get_default_host() == "example.com");
    }
    
    SECTION("Remove Certificate") {
        auto& mgr = certificate_manager::instance();
        auto [cert, key] = generate_test_certificate("example.com");
        
        REQUIRE(mgr.set_certificate("example.com", cert, key));
        REQUIRE(mgr.has_certificate("example.com"));
        
        REQUIRE(mgr.remove_certificate("example.com"));
        REQUIRE_FALSE(mgr.has_certificate("example.com"));
        REQUIRE(mgr.get_certificate("example.com") == nullptr);
    }
    
    SECTION("Registered Hosts") {
        auto& mgr = certificate_manager::instance();
        auto [cert1, key1] = generate_test_certificate("example.com");
        auto [cert2, key2] = generate_test_certificate("test.com");
        auto [cert3, key3] = generate_test_certificate("*.wildcard.com");
        
        mgr.set_certificate("example.com", cert1, key1);
        mgr.set_certificate("test.com", cert2, key2);
        mgr.set_certificate("*.wildcard.com", cert3, key3);
        
        auto hosts = mgr.get_registered_hosts();
        REQUIRE(hosts.size() == 3);
        REQUIRE(hosts.find("example.com") != hosts.end());
        REQUIRE(hosts.find("test.com") != hosts.end());
        REQUIRE(hosts.find("*.wildcard.com") != hosts.end());
    }
    
    SECTION("SSL Configuration") {
        auto& mgr = certificate_manager::instance();
        
        mgr.set_server_ciphers("ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384", true);
        mgr.enable_legacy_protocols(true);
        
        auto [cert, key] = generate_test_certificate("example.com");
        REQUIRE(mgr.set_certificate("example.com", cert, key));
        
        auto ctx = mgr.get_certificate("example.com");
        REQUIRE(ctx != nullptr);
    }
    
    SECTION("Invalid Certificate") {
        auto& mgr = certificate_manager::instance();
        
        REQUIRE_FALSE(mgr.set_certificate("example.com", "invalid cert", "invalid key"));
        REQUIRE_FALSE(mgr.has_certificate("example.com"));
    }
    
    SECTION("Empty Hostname") {
        auto& mgr = certificate_manager::instance();
        auto [cert, key] = generate_test_certificate("example.com");
        
        REQUIRE_FALSE(mgr.set_certificate("", cert, key));
    }
    
    SECTION("Replace Existing Certificate") {
        auto& mgr = certificate_manager::instance();
        auto [cert1, key1] = generate_test_certificate("example1.com");
        auto [cert2, key2] = generate_test_certificate("example2.com");
        
        // Set first certificate
        REQUIRE(mgr.set_certificate("example.com", cert1, key1));
        auto ctx1 = mgr.get_certificate("example.com");
        REQUIRE(get_certificate_cn(ctx1) == "example1.com");
        
        // Replace with second certificate
        REQUIRE(mgr.set_certificate("example.com", cert2, key2));
        auto ctx2 = mgr.get_certificate("example.com");
        REQUIRE(get_certificate_cn(ctx2) == "example2.com");
        
        REQUIRE(mgr.has_certificate("example.com"));
    }
    
    SECTION("Complex Wildcard Patterns") {
        auto& mgr = certificate_manager::instance();
        auto [cert, key] = generate_test_certificate("*.sub.example.com");
        
        REQUIRE(mgr.set_certificate("*.sub.example.com", cert, key));
        
        // Should match
        REQUIRE(mgr.get_certificate("www.sub.example.com") != nullptr);
        REQUIRE(mgr.get_certificate("api.sub.example.com") != nullptr);
        
        // Should not match
        REQUIRE(mgr.get_certificate("sub.example.com") == nullptr);
        REQUIRE(mgr.get_certificate("www.example.com") == nullptr);
        REQUIRE(mgr.get_certificate("deep.www.sub.example.com") == nullptr);
    }
    
    SECTION("SNI Callback") {
        auto& mgr = certificate_manager::instance();
        auto [cert1, key1] = generate_test_certificate("example.com");
        auto [cert2, key2] = generate_test_certificate("test.com");
        
        mgr.set_certificate("example.com", cert1, key1);
        mgr.set_certificate("test.com", cert2, key2);
        mgr.set_default_certificate(cert1, key1);
        
        // The SNI callback is tested indirectly through socket_server integration
        // Here we just verify it doesn't crash
        REQUIRE_NOTHROW(certificate_manager::sni_callback(nullptr, nullptr, nullptr));
    }
    
    SECTION("Thread Safety") {
        auto& mgr = certificate_manager::instance();
        constexpr int num_threads = 10;
        constexpr int certs_per_thread = 10;
        
        std::vector<std::thread> threads;
        
        // Launch multiple threads setting certificates
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&mgr, t, certs_per_thread]() {
                for (int i = 0; i < certs_per_thread; ++i) {
                    auto hostname = "thread" + std::to_string(t) + "-cert" + std::to_string(i) + ".com";
                    auto [cert, key] = generate_test_certificate(hostname);
                    mgr.set_certificate(hostname, cert, key);
                }
            });
        }
        
        // Wait for all threads
        for (auto& t : threads) {
            t.join();
        }
        
        // Verify all certificates were set
        auto hosts = mgr.get_registered_hosts();
        REQUIRE(hosts.size() == num_threads * certs_per_thread);
    }
    
    SECTION("Remove Wildcard Certificate") {
        auto& mgr = certificate_manager::instance();
        auto [cert, key] = generate_test_certificate("*.example.com");
        
        // Set wildcard certificate
        REQUIRE(mgr.set_certificate("*.example.com", cert, key));
        REQUIRE(mgr.has_certificate("*.example.com"));
        
        // Verify it matches subdomains
        REQUIRE(mgr.get_certificate("www.example.com") != nullptr);
        REQUIRE(mgr.get_certificate("api.example.com") != nullptr);
        
        // Remove wildcard certificate
        REQUIRE(mgr.remove_certificate("*.example.com"));
        REQUIRE_FALSE(mgr.has_certificate("*.example.com"));
        
        // Verify subdomains no longer match
        REQUIRE(mgr.get_certificate("www.example.com") == nullptr);
        REQUIRE(mgr.get_certificate("api.example.com") == nullptr);
    }
    
    SECTION("Update Wildcard Certificate") {
        auto& mgr = certificate_manager::instance();
        auto [cert1, key1] = generate_test_certificate("*.old.com");
        auto [cert2, key2] = generate_test_certificate("*.new.com");
        
        // Set first wildcard
        REQUIRE(mgr.set_certificate("*.example.com", cert1, key1));
        auto ctx1 = mgr.get_certificate("www.example.com");
        REQUIRE(ctx1 != nullptr);
        REQUIRE(get_certificate_cn(ctx1) == "*.old.com");
        
        // Update with new certificate
        REQUIRE(mgr.set_certificate("*.example.com", cert2, key2));
        auto ctx2 = mgr.get_certificate("www.example.com");
        REQUIRE(ctx2 != nullptr);
        REQUIRE(get_certificate_cn(ctx2) == "*.new.com");
        
        // Verify it's truly updated (different context)
        REQUIRE(ctx1 != ctx2);
    }
    
    SECTION("Remove Certificate with Active Default") {
        auto& mgr = certificate_manager::instance();
        auto [cert, key] = generate_test_certificate("example.com");
        
        // Set certificate and make it default
        REQUIRE(mgr.set_certificate("example.com", cert, key));
        mgr.set_default_host("example.com");
        
        // Remove the certificate
        REQUIRE(mgr.remove_certificate("example.com"));
        REQUIRE_FALSE(mgr.has_certificate("example.com"));
        
        // Default host should remain but certificate lookup fails
        REQUIRE(mgr.get_default_host() == "example.com");
        REQUIRE(mgr.get_certificate("example.com") == nullptr);
    }
    
    SECTION("Remove Exact Certificate Keep Wildcard") {
        auto& mgr = certificate_manager::instance();
        auto [wildcard_cert, wildcard_key] = generate_test_certificate("*.example.com");
        auto [exact_cert, exact_key] = generate_test_certificate("www.example.com");
        
        // Set both certificates (wildcard and exact match)
        REQUIRE(mgr.set_certificate("*.example.com", wildcard_cert, wildcard_key));
        REQUIRE(mgr.set_certificate("www.example.com", exact_cert, exact_key));
        
        // Verify exact match takes priority
        auto ctx_before = mgr.get_certificate("www.example.com");
        REQUIRE(get_certificate_cn(ctx_before) == "www.example.com");
        
        // Remove only the exact certificate
        REQUIRE(mgr.remove_certificate("www.example.com"));
        // Note: has_certificate still returns true because wildcard matches
        REQUIRE(mgr.has_certificate("www.example.com")); // wildcard still matches
        REQUIRE(mgr.has_certificate("*.example.com"));
        
        // Now www.example.com should fall back to wildcard
        auto ctx_after = mgr.get_certificate("www.example.com");
        REQUIRE(ctx_after != nullptr);
        REQUIRE(get_certificate_cn(ctx_after) == "*.example.com");
    }
    
    SECTION("Regex Map Patterns") {
        auto& mgr = certificate_manager::instance();
        
        auto [cert1, key1] = generate_test_certificate("regex1");
        auto [cert2, key2] = generate_test_certificate("regex2");
        auto [cert3, key3] = generate_test_certificate("regex3");
        
        // Set certificates with different patterns
        mgr.set_certificate("^api\\..*\\.com$", cert1, key1);  // Matches api.*.com
        mgr.set_certificate(".*\\.internal$", cert2, key2);     // Matches *.internal
        mgr.set_certificate("^(www|api)\\.example\\.com$", cert3, key3); // Matches www or api.example.com
        
        // Test matches
        REQUIRE(mgr.get_certificate("api.test.com") != nullptr);
        REQUIRE(mgr.get_certificate("api.production.com") != nullptr);
        REQUIRE(mgr.get_certificate("server.internal") != nullptr);
        REQUIRE(mgr.get_certificate("api.internal") != nullptr);
        REQUIRE(mgr.get_certificate("www.example.com") != nullptr);
        REQUIRE(mgr.get_certificate("api.example.com") != nullptr);
        
        // Test non-matches
        REQUIRE(mgr.get_certificate("test.com") == nullptr);
        REQUIRE(mgr.get_certificate("internal.com") == nullptr);
        REQUIRE(mgr.get_certificate("test.example.com") == nullptr);
    }
    
    // Clean up after all tests
    clear_all_certificates();
}
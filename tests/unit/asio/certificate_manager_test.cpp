#include <catch2/catch_test_macros.hpp>
#include <thinger/asio/ssl/certificate_manager.hpp>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <set>
#include <thread>
#include <string>
#include <vector>
#include "../../fixtures/test_ca.hpp"

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
    // and the TLS options
    REQUIRE(mgr.set_tls_options({}));
}

// Names of the ciphers of a context: those of TLS 1.2 and older, or those of TLS 1.3
std::set<std::string> cipher_names(const std::shared_ptr<boost::asio::ssl::context>& ctx, bool tls13) {
    std::set<std::string> names;
    auto* ciphers = SSL_CTX_get_ciphers(ctx->native_handle());
    for (int i = 0; i < sk_SSL_CIPHER_num(ciphers); ++i) {
        const SSL_CIPHER* cipher = sk_SSL_CIPHER_value(ciphers, i);
        if ((std::string(SSL_CIPHER_get_version(cipher)) == "TLSv1.3") == tls13) names.insert(SSL_CIPHER_standard_name(cipher));
    }
    return names;
}

bool prefers_server_ciphers(const std::shared_ptr<boost::asio::ssl::context>& ctx) {
    return SSL_CTX_get_options(ctx->native_handle()) & SSL_OP_CIPHER_SERVER_PREFERENCE;
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
        // the setters of earlier versions set the TLS options
        REQUIRE(cipher_names(ctx, false) == std::set<std::string>{"TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256",
                                                                  "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384"});
        REQUIRE(prefers_server_ciphers(ctx));
        REQUIRE(SSL_CTX_get_min_proto_version(ctx->native_handle()) == TLS1_VERSION);
        REQUIRE(SSL_CTX_get_security_level(ctx->native_handle()) == 0);

        // back to the defaults, keeping the ciphers set
        mgr.enable_legacy_protocols(false);
        REQUIRE(mgr.get_tls_options().min_version == tls_version::tls1_2);
        REQUIRE_FALSE(mgr.get_tls_options().security_level);
        REQUIRE(mgr.get_tls_options().ciphers == "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384");
    }

    SECTION("Legacy protocols with the default ciphers") {
        auto& mgr = certificate_manager::instance();
        mgr.enable_legacy_protocols(true);
        // OpenSSL's defaults, as TLS 1.0 and 1.1 have no AEAD cipher
        REQUIRE(mgr.get_tls_options().ciphers.empty());
        mgr.enable_legacy_protocols(false);
        REQUIRE(mgr.get_tls_options().ciphers == tls_options{}.ciphers);
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
    
    SECTION("SNI Callback switches to the default certificate replaced") {
        auto& mgr = certificate_manager::instance();
        auto [cert1, key1] = generate_test_certificate("listener");
        auto [cert2, key2] = generate_test_certificate("replaced");
        REQUIRE(mgr.set_default_certificate(cert1, key1));
        auto listener = mgr.get_default_certificate();

        // without SNI, and while the default is the one of the listener, it is kept
        SSL* ssl = SSL_new(listener->native_handle());
        int alert = 0;
        REQUIRE(certificate_manager::sni_callback(ssl, &alert, nullptr) == SSL_TLSEXT_ERR_OK);
        REQUIRE(SSL_get_SSL_CTX(ssl) == listener->native_handle());

        REQUIRE(mgr.set_default_certificate(cert2, key2));
        REQUIRE(certificate_manager::sni_callback(ssl, &alert, nullptr) == SSL_TLSEXT_ERR_OK);
        REQUIRE(SSL_get_SSL_CTX(ssl) == mgr.get_default_certificate()->native_handle());

        // the connection keeps its context alive once replaced again
        SSL_CTX* taken = SSL_get_SSL_CTX(ssl);
        REQUIRE(mgr.set_default_certificate(cert1, key1));
        char cn[256] = {};
        X509_NAME_get_text_by_NID(X509_get_subject_name(SSL_CTX_get0_certificate(taken)), NID_commonName, cn, sizeof(cn));
        REQUIRE(std::string(cn) == "replaced");
        SSL_free(ssl);
    }

    SECTION("Invalid Default Certificate keeps the current one") {
        auto& mgr = certificate_manager::instance();
        auto [cert, key] = generate_test_certificate("current");
        REQUIRE(mgr.set_default_certificate(cert, key));

        REQUIRE_FALSE(mgr.set_default_certificate("invalid cert", "invalid key"));
        REQUIRE(get_certificate_cn(mgr.get_default_certificate()) == "current");
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
TEST_CASE("Certificate manager TLS options", "[certificate_manager][tls]") {
    clear_all_certificates();
    auto& mgr = certificate_manager::instance();
    auto [cert, key] = generate_test_certificate("tls.example.com");

    SECTION("Mozilla intermediate by default, on every context created") {
        auto options = mgr.get_tls_options();
        REQUIRE(options.min_version == tls_version::tls1_2);
        REQUIRE(options.max_version == tls_version::tls1_3);
        REQUIRE_FALSE(options.prefer_server_ciphers);

        REQUIRE(mgr.set_certificate("tls.example.com", cert, key));
        REQUIRE(mgr.set_default_certificate(cert, key));
        auto host = mgr.get_certificate("tls.example.com");
        auto fallback = mgr.get_default_certificate();
        mgr.set_default_certificate(nullptr);
        auto self_signed = mgr.get_default_certificate();

        const std::set<std::string> tls12{
            "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256", "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256",
            "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384", "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384",
            "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256", "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256"};
        const std::set<std::string> tls13{"TLS_AES_128_GCM_SHA256", "TLS_AES_256_GCM_SHA384",
                                          "TLS_CHACHA20_POLY1305_SHA256"};
        for (const auto& ctx : {host, fallback, self_signed}) {
            REQUIRE(ctx);
            REQUIRE(SSL_CTX_get_min_proto_version(ctx->native_handle()) == TLS1_2_VERSION);
            REQUIRE(SSL_CTX_get_max_proto_version(ctx->native_handle()) == TLS1_3_VERSION);
            REQUIRE(cipher_names(ctx, false) == tls12);
            REQUIRE(cipher_names(ctx, true) == tls13);
            REQUIRE_FALSE(prefers_server_ciphers(ctx));
        }
    }

    SECTION("Options set apply to the contexts created afterwards") {
        REQUIRE(mgr.set_certificate("before.example.com", cert, key));
        tls_options options;
        options.min_version = tls_version::tls1_3;
        options.ciphersuites = "TLS_AES_256_GCM_SHA384";
        options.prefer_server_ciphers = true;
        options.groups = "X25519:prime256v1";
        options.security_level = 1;
        REQUIRE(mgr.set_tls_options(options));
        REQUIRE(mgr.get_tls_options().security_level == 1);
        REQUIRE(mgr.set_certificate("tls.example.com", cert, key));

        auto ctx = mgr.get_certificate("tls.example.com");
        REQUIRE(SSL_CTX_get_min_proto_version(ctx->native_handle()) == TLS1_3_VERSION);
        REQUIRE(cipher_names(ctx, true) == std::set<std::string>{"TLS_AES_256_GCM_SHA384"});
        REQUIRE(prefers_server_ciphers(ctx));
        REQUIRE(SSL_CTX_get_security_level(ctx->native_handle()) == 1);
        // the ones created before keep theirs
        auto before = mgr.get_certificate("before.example.com");
        REQUIRE(SSL_CTX_get_min_proto_version(before->native_handle()) == TLS1_2_VERSION);
    }

    SECTION("Options OpenSSL refuses are not set") {
        auto refused = [&](auto change) {
            tls_options options;
            change(options);
            return !mgr.set_tls_options(options);
        };
        REQUIRE(refused([](tls_options& options) { options.ciphers = "NOT-A-CIPHER"; }));
        REQUIRE(refused([](tls_options& options) { options.ciphersuites = "TLS_NOT_A_SUITE"; }));
        REQUIRE(refused([](tls_options& options) { options.groups = "not-a-curve"; }));
        REQUIRE(refused([](tls_options& options) { options.security_level = 6; }));
        REQUIRE(mgr.get_tls_options().ciphers == tls_options{}.ciphers);
        REQUIRE(mgr.get_tls_options().ciphersuites == tls_options{}.ciphersuites);
    }

    SECTION("The deprecated setters change the options, in any order") {
        auto legacy_first = [&] {
            REQUIRE(mgr.set_tls_options({}));
            mgr.enable_legacy_protocols(true);
            REQUIRE(mgr.set_server_ciphers("ECDHE-RSA-AES128-GCM-SHA256", true));
            return mgr.get_tls_options();
        }();
        auto ciphers_first = [&] {
            REQUIRE(mgr.set_tls_options({}));
            REQUIRE(mgr.set_server_ciphers("ECDHE-RSA-AES128-GCM-SHA256", true));
            mgr.enable_legacy_protocols(true);
            return mgr.get_tls_options();
        }();
        for (const auto& options : {legacy_first, ciphers_first}) {
            REQUIRE(options.min_version == tls_version::tls1_0);
            REQUIRE(options.security_level == 0);
            REQUIRE(options.ciphers == "ECDHE-RSA-AES128-GCM-SHA256");
            REQUIRE(options.prefer_server_ciphers);
        }
        // undone, keeping the ciphers chosen
        mgr.enable_legacy_protocols(false);
        REQUIRE(mgr.get_tls_options().min_version == tls_version::tls1_2);
        REQUIRE_FALSE(mgr.get_tls_options().security_level);
        REQUIRE(mgr.get_tls_options().ciphers == "ECDHE-RSA-AES128-GCM-SHA256");
    }

    SECTION("The deprecated setters are validated") {
        REQUIRE_FALSE(mgr.set_server_ciphers("NOT-A-CIPHER", true));
        REQUIRE(mgr.get_tls_options().ciphers == tls_options{}.ciphers);
        REQUIRE_FALSE(mgr.get_tls_options().prefer_server_ciphers);
    }

    SECTION("Disabling the legacy protocols undoes only what enabling them did") {
        tls_options options;
        options.min_version = tls_version::tls1_3;
        options.security_level = 3;
        REQUIRE(mgr.set_tls_options(options));
        // not enabled: nothing to undo
        mgr.enable_legacy_protocols(false);
        REQUIRE(mgr.get_tls_options().min_version == tls_version::tls1_3);
        REQUIRE(mgr.get_tls_options().security_level == 3);
        // enabled and disabled: back to what was set
        mgr.enable_legacy_protocols(true);
        REQUIRE(mgr.get_tls_options().min_version == tls_version::tls1_0);
        mgr.enable_legacy_protocols(false);
        REQUIRE(mgr.get_tls_options().min_version == tls_version::tls1_3);
        REQUIRE(mgr.get_tls_options().security_level == 3);
        REQUIRE(mgr.get_tls_options().ciphers == tls_options{}.ciphers);
        // ciphers chosen while enabled stay, also OpenSSL's defaults chosen explicitly
        mgr.enable_legacy_protocols(true);
        REQUIRE(mgr.set_server_ciphers("", false));
        mgr.enable_legacy_protocols(false);
        REQUIRE(mgr.get_tls_options().ciphers.empty());
    }

    SECTION("A context is left as it was when OpenSSL refuses the options") {
        auto ctx = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
        REQUIRE(apply_tls_options(*ctx, {}));
        tls_options refused;
        refused.min_version = tls_version::tls1_3;
        refused.ciphers = "NOT-A-CIPHER";
        REQUIRE_FALSE(apply_tls_options(*ctx, refused));
        REQUIRE(SSL_CTX_get_min_proto_version(ctx->native_handle()) == TLS1_2_VERSION);
        REQUIRE(cipher_names(ctx, false).size() == 6);
    }

    SECTION("Applied to a context given") {
        auto ctx = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
        REQUIRE(apply_tls_options(*ctx, mgr.get_tls_options()));
        REQUIRE(SSL_CTX_get_min_proto_version(ctx->native_handle()) == TLS1_2_VERSION);
        REQUIRE(cipher_names(ctx, false).size() == 6);
    }

    REQUIRE(mgr.set_tls_options({}));
}

TEST_CASE("Certificate manager client certificate modes per host", "[certificate_manager][mtls]") {
    auto& manager = certificate_manager::instance();

    SECTION("Exact names and wildcards, case-insensitive") {
        REQUIRE(manager.set_client_verify("Devices.Example.com", client_verify::required));
        REQUIRE(manager.get_client_verify("devices.example.com") == client_verify::required);
        REQUIRE(manager.get_client_verify("DEVICES.EXAMPLE.COM") == client_verify::required);
        REQUIRE(manager.set_client_verify("*.wild.example.com", client_verify::optional));
        REQUIRE(manager.get_client_verify("a.wild.example.com") == client_verify::optional);
        REQUIRE_FALSE(manager.get_client_verify("a.b.wild.example.com"));
        REQUIRE_FALSE(manager.get_client_verify("wild.example.com"));
        REQUIRE_FALSE(manager.get_client_verify("other.example.com"));
        // replaced
        REQUIRE(manager.set_client_verify("devices.example.com", client_verify::optional));
        REQUIRE(manager.get_client_verify("devices.example.com") == client_verify::optional);

        REQUIRE(manager.remove_client_verify("devices.example.com"));
        REQUIRE(manager.remove_client_verify("*.wild.example.com"));
        REQUIRE_FALSE(manager.get_client_verify("devices.example.com"));
        REQUIRE_FALSE(manager.get_client_verify("a.wild.example.com"));
        REQUIRE_FALSE(manager.remove_client_verify("devices.example.com"));
    }

    SECTION("Names that are not host names are refused") {
        for (std::string name : {"", "*", "*.", "bad host", "a*.example.com", "a..example.com", ".example.com",
                                 "example.com.", "host/path", std::string(64, 'a') + ".com"}) {
            REQUIRE_FALSE(manager.set_client_verify(name, client_verify::required));
        }
        REQUIRE(manager.set_client_verify("under_score-1.example.com", client_verify::none));
        REQUIRE(manager.remove_client_verify("under_score-1.example.com"));
    }

    SECTION("Set and read from several threads") {
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&manager, t] {
                for (int i = 0; i < 200; ++i) {
                    auto host = "host" + std::to_string(t) + "-" + std::to_string(i % 10) + ".example.com";
                    manager.set_client_verify(host, client_verify::optional);
                    manager.get_client_verify(host);
                    manager.remove_client_verify(host);
                }
            });
        }
        for (auto& thread : threads) thread.join();
        REQUIRE_FALSE(manager.get_client_verify("host0-0.example.com"));
    }
}

TEST_CASE("use_certificate checks the certificate and its key", "[certificate_manager][mtls]") {
    thinger::http::test::test_ca ca;
    auto certificate = ca.issue("localhost");
    auto other = ca.issue("other");
    boost::asio::ssl::context context(boost::asio::ssl::context::tls_server);
    REQUIRE(use_certificate(context, certificate.first, certificate.second) == certificate_error::none);
    REQUIRE(use_certificate(context, "garbage", certificate.second) == certificate_error::invalid_certificate);
    boost::asio::ssl::context mismatched(boost::asio::ssl::context::tls_server);
    REQUIRE(use_certificate(mismatched, certificate.first, other.second) == certificate_error::key_mismatch);
    boost::asio::ssl::context unreadable(boost::asio::ssl::context::tls_server);
    REQUIRE(use_certificate(unreadable, certificate.first, "garbage") == certificate_error::invalid_private_key);
}

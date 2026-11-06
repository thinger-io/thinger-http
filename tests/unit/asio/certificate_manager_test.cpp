#include <catch2/catch_test_macros.hpp>
#include <thinger/asio/listener.hpp>
#include <thinger/asio/ssl/certificate_manager.hpp>
#include <boost/asio/io_context.hpp>
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
    X509_NAME* name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, (unsigned char*)"US", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (unsigned char*)common_name.c_str(), -1, -1, 0);

    X509_set_subject_name(x509, name);
    X509_set_issuer_name(x509, name);
    X509_NAME_free(name);
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

// The server side of a handshake of a server listening with `listener`, once it read the
// ClientHello of a client asking for `name` (none: no SNI), so after its SNI callback ran
SSL* server_after_hello(const std::shared_ptr<boost::asio::ssl::context>& listener, const char* name) {
    SSL_CTX* client_context = SSL_CTX_new(TLS_client_method());
    SSL* client = SSL_new(client_context);
    SSL* server = SSL_new(listener->native_handle());
    BIO* client_bio = nullptr;
    BIO* server_bio = nullptr;
    BIO_new_bio_pair(&client_bio, 0, &server_bio, 0);
    SSL_set_bio(client, client_bio, client_bio);
    SSL_set_bio(server, server_bio, server_bio);
    SSL_set_connect_state(client);
    SSL_set_accept_state(server);
    if (name) SSL_set_tlsext_host_name(client, name);
    SSL_do_handshake(client);   // writes the ClientHello
    SSL_do_handshake(server);   // reads it: the callbacks run
    SSL_free(client);
    SSL_CTX_free(client_context);
    return server;
}

namespace {

    // The common name of a certificate (empty if none)
    std::string common_name(X509* certificate) {
        if (!certificate) return "";
        char cn[256] = {};
        if (X509_NAME_get_text_by_NID(X509_get_subject_name(certificate), NID_commonName, cn, sizeof(cn)) <= 0) return "";
        return cn;
    }

    // A whole handshake, in memory, of a client asking for `name` (none: no SNI) with a server
    // listening with `listener`: the common name of the certificate the server presented, empty
    // if the handshake failed
    std::string presented_certificate(const std::shared_ptr<boost::asio::ssl::context>& listener, const char* name) {
        SSL_CTX* client_context = SSL_CTX_new(TLS_client_method());
        SSL* client = SSL_new(client_context);
        SSL* server = SSL_new(listener->native_handle());
        BIO* client_bio = nullptr;
        BIO* server_bio = nullptr;
        BIO_new_bio_pair(&client_bio, 0, &server_bio, 0);
        SSL_set_bio(client, client_bio, client_bio);
        SSL_set_bio(server, server_bio, server_bio);
        SSL_set_connect_state(client);
        SSL_set_accept_state(server);
        if (name) SSL_set_tlsext_host_name(client, name);
        auto pending = [](SSL* ssl, int result) {
            return result != 1 && SSL_get_error(ssl, result) == SSL_ERROR_WANT_READ;
        };
        int client_result = 0;
        int server_result = 0;
        bool failed = false;
        for (int i = 0; i < 10 && !failed && (client_result != 1 || server_result != 1); ++i) {
            if (client_result != 1) client_result = SSL_do_handshake(client);
            if (server_result != 1) server_result = SSL_do_handshake(server);
            failed = (client_result != 1 && !pending(client, client_result)) ||
                     (server_result != 1 && !pending(server, server_result));
        }
        std::string presented;
        if (client_result == 1 && server_result == 1) {
            X509* certificate = SSL_get1_peer_certificate(client);
            presented = common_name(certificate);
            X509_free(certificate);
        }
        SSL_free(server);
        SSL_free(client);
        SSL_CTX_free(client_context);
        return presented;
    }

    // A context made outside the managers, with the certificate of `certificate`
    std::shared_ptr<boost::asio::ssl::context> ready_made(const std::pair<std::string, std::string>& certificate) {
        auto context = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
        REQUIRE(use_certificate(*context, certificate.first, certificate.second) == certificate_error::none);
        return context;
    }

    // An allocator that counts the blocks it has allocated and not freed: given to
    // std::allocate_shared, `live` is 1 until the control block of the manager is freed, which
    // the weak_ptr kept with each context of the manager holds
    template<typename T>
    struct counting_allocator {
        using value_type = T;
        int* live;

        explicit counting_allocator(int* blocks) : live(blocks) {}
        template<typename U>
        counting_allocator(const counting_allocator<U>& other) : live(other.live) {}

        T* allocate(size_t n) {
            ++*live;
            return std::allocator<T>{}.allocate(n);
        }
        void deallocate(T* pointer, size_t n) {
            --*live;
            std::allocator<T>{}.deallocate(pointer, n);
        }
        template<typename U>
        bool operator==(const counting_allocator<U>& other) const { return live == other.live; }
    };

    std::shared_ptr<certificate_manager> counted_manager(int* live) {
        return std::allocate_shared<certificate_manager>(counting_allocator<certificate_manager>(live));
    }

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
    
    const X509_NAME* subject = X509_get_subject_name(cert);
    char cn[256];
    if (X509_NAME_get_text_by_NID(subject, NID_commonName, cn, sizeof(cn)) > 0) {
        return {cn};
    }
    return "";
}

TEST_CASE("Certificate Manager Tests", "[certificate_manager]") {
    // A manager of its own for each section
    auto mgr = std::make_shared<certificate_manager>();

    SECTION("Managers are independent") {
        auto other = std::make_shared<certificate_manager>();
        auto [cert, key] = generate_test_certificate("example.com");
        REQUIRE_FALSE(mgr->set_certificate("example.com", cert, key));
        REQUIRE(mgr->has_certificate("example.com"));
        REQUIRE_FALSE(other->has_certificate("example.com"));
        REQUIRE(mgr->get_default_certificate() != other->get_default_certificate());
    }
    
    SECTION("Set and Get Certificate") {
        auto [cert, key] = generate_test_certificate("example.com");
        
        REQUIRE_FALSE(mgr->set_certificate("example.com", cert, key));
        
        auto ctx = mgr->get_certificate("example.com");
        REQUIRE(ctx != nullptr);
        REQUIRE(mgr->has_certificate("example.com"));
        
        // Verify it's the correct certificate
        REQUIRE(get_certificate_cn(ctx) == "example.com");
    }
    
    SECTION("Certificate Not Found") {
        auto ctx = mgr->get_certificate("nonexistent.com");
        REQUIRE(ctx == nullptr);
        REQUIRE_FALSE(mgr->has_certificate("nonexistent.com"));
    }
    
    SECTION("Wildcard Certificate") {
        auto [cert, key] = generate_test_certificate("*.example.com");
        
        REQUIRE_FALSE(mgr->set_certificate("*.example.com", cert, key));
        
        // Test various subdomains
        REQUIRE(mgr->get_certificate("www.example.com") != nullptr);
        REQUIRE(mgr->get_certificate("api.example.com") != nullptr);
        REQUIRE(mgr->get_certificate("test.example.com") != nullptr);
        
        // Root domain should not match wildcard
        REQUIRE(mgr->get_certificate("example.com") == nullptr);
        
        // Different domain should not match
        REQUIRE(mgr->get_certificate("www.different.com") == nullptr);
    }
    
    SECTION("Exact Match Priority") {
        auto [wildcard_cert, wildcard_key] = generate_test_certificate("*.example.com");
        auto [exact_cert, exact_key] = generate_test_certificate("www.example.com");
        
        // Set both wildcard and exact match
        REQUIRE_FALSE(mgr->set_certificate("*.example.com", wildcard_cert, wildcard_key));
        REQUIRE_FALSE(mgr->set_certificate("www.example.com", exact_cert, exact_key));
        
        // Check registered hosts
        auto hosts = mgr->get_registered_hosts();
        REQUIRE(hosts.find("*.example.com") != hosts.end());
        REQUIRE(hosts.find("www.example.com") != hosts.end());
        
        // Exact match should take priority
        auto ctx_www = mgr->get_certificate("www.example.com");
        REQUIRE(ctx_www != nullptr);
        
        // Debug: Check if both certificates are different
        auto ctx_wildcard = mgr->get_certificate("*.example.com");
        REQUIRE(ctx_wildcard != nullptr);
        INFO("www.example.com cert CN: " << get_certificate_cn(ctx_www));
        INFO("*.example.com cert CN: " << get_certificate_cn(ctx_wildcard));
        INFO("Are they the same cert? " << (ctx_www == ctx_wildcard ? "YES" : "NO"));
        
        REQUIRE(get_certificate_cn(ctx_www) == "www.example.com");
        
        // Other subdomains should use wildcard
        auto ctx_api = mgr->get_certificate("api.example.com");
        REQUIRE(ctx_api != nullptr);
        REQUIRE(get_certificate_cn(ctx_api) == "*.example.com");
    }
    
    SECTION("Default Certificate") {
        auto [cert, key] = generate_test_certificate("default");
        
        mgr->set_default_certificate(cert, key);
        
        auto ctx = mgr->get_default_certificate();
        REQUIRE(ctx != nullptr);
        REQUIRE(get_certificate_cn(ctx) == "default");
    }
    
    SECTION("Auto-generated Default Certificate") {
        // A manager that never had a default certificate generates a self-signed one
        auto fresh = std::make_shared<certificate_manager>();
        auto ctx = fresh->get_default_certificate();
        REQUIRE(ctx != nullptr);
        
        // Verify it's a localhost certificate
        REQUIRE(get_certificate_cn(ctx) == "localhost");
        
        // Getting it again should return the same certificate
        auto ctx2 = fresh->get_default_certificate();
        REQUIRE(ctx2 != nullptr);
        REQUIRE(ctx == ctx2);
    }
    
    SECTION("Default Host") {
        mgr->set_default_host("example.com");
        REQUIRE(mgr->get_default_host() == "example.com");
    }
    
    SECTION("Remove Certificate") {
        auto [cert, key] = generate_test_certificate("example.com");
        
        REQUIRE_FALSE(mgr->set_certificate("example.com", cert, key));
        REQUIRE(mgr->has_certificate("example.com"));
        
        REQUIRE(mgr->remove_certificate("example.com"));
        REQUIRE_FALSE(mgr->has_certificate("example.com"));
        REQUIRE(mgr->get_certificate("example.com") == nullptr);
    }
    
    SECTION("Registered Hosts") {
        auto [cert1, key1] = generate_test_certificate("example.com");
        auto [cert2, key2] = generate_test_certificate("test.com");
        auto [cert3, key3] = generate_test_certificate("*.wildcard.com");
        
        mgr->set_certificate("example.com", cert1, key1);
        mgr->set_certificate("test.com", cert2, key2);
        mgr->set_certificate("*.wildcard.com", cert3, key3);
        
        auto hosts = mgr->get_registered_hosts();
        REQUIRE(hosts.size() == 3);
        REQUIRE(hosts.find("example.com") != hosts.end());
        REQUIRE(hosts.find("test.com") != hosts.end());
        REQUIRE(hosts.find("*.wildcard.com") != hosts.end());
    }
    
    SECTION("SSL Configuration") {
        mgr->set_server_ciphers("ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384", true);
        mgr->enable_legacy_protocols(true);
        
        auto [cert, key] = generate_test_certificate("example.com");
        REQUIRE_FALSE(mgr->set_certificate("example.com", cert, key));
        
        auto ctx = mgr->get_certificate("example.com");
        REQUIRE(ctx != nullptr);
        // the setters of earlier versions set the TLS options
        REQUIRE(cipher_names(ctx, false) == std::set<std::string>{"TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256",
                                                                  "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384"});
        REQUIRE(prefers_server_ciphers(ctx));
        REQUIRE(SSL_CTX_get_min_proto_version(ctx->native_handle()) == TLS1_VERSION);
        REQUIRE(SSL_CTX_get_security_level(ctx->native_handle()) == 0);

        // back to the defaults, keeping the ciphers set
        mgr->enable_legacy_protocols(false);
        REQUIRE(mgr->get_tls_options().min_version == tls_version::tls1_2);
        REQUIRE_FALSE(mgr->get_tls_options().security_level);
        REQUIRE(mgr->get_tls_options().ciphers == "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384");
    }

    SECTION("Legacy protocols with the default ciphers") {
        mgr->enable_legacy_protocols(true);
        // OpenSSL's defaults, as TLS 1.0 and 1.1 have no AEAD cipher
        REQUIRE(mgr->get_tls_options().ciphers.empty());
        mgr->enable_legacy_protocols(false);
        REQUIRE(mgr->get_tls_options().ciphers == tls_options{}.ciphers);
    }
    
    SECTION("Invalid Certificate") {
        REQUIRE(mgr->set_certificate("example.com", "invalid cert", "invalid key") == boost::system::errc::invalid_argument);
        REQUIRE_FALSE(mgr->has_certificate("example.com"));
    }
    
    SECTION("Empty Hostname") {
        auto [cert, key] = generate_test_certificate("example.com");
        
        REQUIRE(mgr->set_certificate("", cert, key) == boost::system::errc::invalid_argument);
    }
    
    SECTION("Replace Existing Certificate") {
        auto [cert1, key1] = generate_test_certificate("example1.com");
        auto [cert2, key2] = generate_test_certificate("example2.com");
        
        // Set first certificate
        REQUIRE_FALSE(mgr->set_certificate("example.com", cert1, key1));
        auto ctx1 = mgr->get_certificate("example.com");
        REQUIRE(get_certificate_cn(ctx1) == "example1.com");
        
        // Replace with second certificate
        REQUIRE_FALSE(mgr->set_certificate("example.com", cert2, key2));
        auto ctx2 = mgr->get_certificate("example.com");
        REQUIRE(get_certificate_cn(ctx2) == "example2.com");
        
        REQUIRE(mgr->has_certificate("example.com"));
    }
    
    SECTION("Complex Wildcard Patterns") {
        auto [cert, key] = generate_test_certificate("*.sub.example.com");
        
        REQUIRE_FALSE(mgr->set_certificate("*.sub.example.com", cert, key));
        
        // Should match
        REQUIRE(mgr->get_certificate("www.sub.example.com") != nullptr);
        REQUIRE(mgr->get_certificate("api.sub.example.com") != nullptr);
        
        // Should not match
        REQUIRE(mgr->get_certificate("sub.example.com") == nullptr);
        REQUIRE(mgr->get_certificate("www.example.com") == nullptr);
        REQUIRE(mgr->get_certificate("deep.www.sub.example.com") == nullptr);
    }
    
    SECTION("SNI Callback") {
        auto [cert1, key1] = generate_test_certificate("example.com");
        auto [cert2, key2] = generate_test_certificate("test.com");
        
        mgr->set_certificate("example.com", cert1, key1);
        mgr->set_certificate("test.com", cert2, key2);
        mgr->set_default_certificate(cert1, key1);
        
        // The SNI callback is tested indirectly through socket_server integration
        // Here we just verify it doesn't crash
        REQUIRE_NOTHROW(certificate_manager::sni_callback(nullptr, nullptr, nullptr));
        // nor does it switch without the manager of a context (arg)
        auto listener = mgr->get_default_certificate();
        SSL* ssl = SSL_new(listener->native_handle());
        int alert = 0;
        REQUIRE(certificate_manager::sni_callback(ssl, &alert, nullptr) == SSL_TLSEXT_ERR_OK);
        REQUIRE(SSL_get_SSL_CTX(ssl) == listener->native_handle());
        SSL_free(ssl);
    }
    
    SECTION("SNI Callback switches to the default certificate replaced") {
        auto [cert1, key1] = generate_test_certificate("listener");
        auto [cert2, key2] = generate_test_certificate("replaced");
        REQUIRE_FALSE(mgr->set_default_certificate(cert1, key1));
        auto listener = mgr->get_default_certificate();

        // without SNI, and while the default is the one of the listener, it is kept
        SSL* ssl = server_after_hello(listener, nullptr);
        REQUIRE(SSL_get_SSL_CTX(ssl) == listener->native_handle());
        SSL_free(ssl);

        REQUIRE_FALSE(mgr->set_default_certificate(cert2, key2));
        ssl = server_after_hello(listener, nullptr);
        REQUIRE(SSL_get_SSL_CTX(ssl) == mgr->get_default_certificate()->native_handle());

        // the connection keeps its context alive once replaced again
        SSL_CTX* taken = SSL_get_SSL_CTX(ssl);
        REQUIRE_FALSE(mgr->set_default_certificate(cert1, key1));
        char cn[256] = {};
        X509_NAME_get_text_by_NID(X509_get_subject_name(SSL_CTX_get0_certificate(taken)), NID_commonName, cn, sizeof(cn));
        REQUIRE(std::string(cn) == "replaced");
        SSL_free(ssl);
    }

    SECTION("Invalid Default Certificate keeps the current one") {
        auto [cert, key] = generate_test_certificate("current");
        REQUIRE_FALSE(mgr->set_default_certificate(cert, key));

        REQUIRE(mgr->set_default_certificate("invalid cert", "invalid key") == boost::system::errc::invalid_argument);
        REQUIRE(get_certificate_cn(mgr->get_default_certificate()) == "current");
    }

    SECTION("Thread Safety") {
        constexpr int num_threads = 10;
        constexpr int certs_per_thread = 10;
        
        std::vector<std::thread> threads;
        
        // Launch multiple threads setting certificates
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&mgr, t, certs_per_thread]() {
                for (int i = 0; i < certs_per_thread; ++i) {
                    auto hostname = "thread" + std::to_string(t) + "-cert" + std::to_string(i) + ".com";
                    auto [cert, key] = generate_test_certificate(hostname);
                    mgr->set_certificate(hostname, cert, key);
                }
            });
        }
        
        // Wait for all threads
        for (auto& t : threads) {
            t.join();
        }
        
        // Verify all certificates were set
        auto hosts = mgr->get_registered_hosts();
        REQUIRE(hosts.size() == num_threads * certs_per_thread);
    }
    
    SECTION("Remove Wildcard Certificate") {
        auto [cert, key] = generate_test_certificate("*.example.com");
        
        // Set wildcard certificate
        REQUIRE_FALSE(mgr->set_certificate("*.example.com", cert, key));
        REQUIRE(mgr->has_certificate("*.example.com"));
        
        // Verify it matches subdomains
        REQUIRE(mgr->get_certificate("www.example.com") != nullptr);
        REQUIRE(mgr->get_certificate("api.example.com") != nullptr);
        
        // Remove wildcard certificate
        REQUIRE(mgr->remove_certificate("*.example.com"));
        REQUIRE_FALSE(mgr->has_certificate("*.example.com"));
        
        // Verify subdomains no longer match
        REQUIRE(mgr->get_certificate("www.example.com") == nullptr);
        REQUIRE(mgr->get_certificate("api.example.com") == nullptr);
    }
    
    SECTION("Update Wildcard Certificate") {
        auto [cert1, key1] = generate_test_certificate("*.old.com");
        auto [cert2, key2] = generate_test_certificate("*.new.com");
        
        // Set first wildcard
        REQUIRE_FALSE(mgr->set_certificate("*.example.com", cert1, key1));
        auto ctx1 = mgr->get_certificate("www.example.com");
        REQUIRE(ctx1 != nullptr);
        REQUIRE(get_certificate_cn(ctx1) == "*.old.com");
        
        // Update with new certificate
        REQUIRE_FALSE(mgr->set_certificate("*.example.com", cert2, key2));
        auto ctx2 = mgr->get_certificate("www.example.com");
        REQUIRE(ctx2 != nullptr);
        REQUIRE(get_certificate_cn(ctx2) == "*.new.com");
        
        // Verify it's truly updated (different context)
        REQUIRE(ctx1 != ctx2);
    }
    
    SECTION("Remove Certificate with Active Default") {
        auto [cert, key] = generate_test_certificate("example.com");
        
        // Set certificate and make it default
        REQUIRE_FALSE(mgr->set_certificate("example.com", cert, key));
        mgr->set_default_host("example.com");
        
        // Remove the certificate
        REQUIRE(mgr->remove_certificate("example.com"));
        REQUIRE_FALSE(mgr->has_certificate("example.com"));
        
        // Default host should remain but certificate lookup fails
        REQUIRE(mgr->get_default_host() == "example.com");
        REQUIRE(mgr->get_certificate("example.com") == nullptr);
    }
    
    SECTION("Remove Exact Certificate Keep Wildcard") {
        auto [wildcard_cert, wildcard_key] = generate_test_certificate("*.example.com");
        auto [exact_cert, exact_key] = generate_test_certificate("www.example.com");
        
        // Set both certificates (wildcard and exact match)
        REQUIRE_FALSE(mgr->set_certificate("*.example.com", wildcard_cert, wildcard_key));
        REQUIRE_FALSE(mgr->set_certificate("www.example.com", exact_cert, exact_key));
        
        // Verify exact match takes priority
        auto ctx_before = mgr->get_certificate("www.example.com");
        REQUIRE(get_certificate_cn(ctx_before) == "www.example.com");
        
        // Remove only the exact certificate
        REQUIRE(mgr->remove_certificate("www.example.com"));
        // Note: has_certificate still returns true because wildcard matches
        REQUIRE(mgr->has_certificate("www.example.com")); // wildcard still matches
        REQUIRE(mgr->has_certificate("*.example.com"));
        
        // Now www.example.com should fall back to wildcard
        auto ctx_after = mgr->get_certificate("www.example.com");
        REQUIRE(ctx_after != nullptr);
        REQUIRE(get_certificate_cn(ctx_after) == "*.example.com");
    }
    
    SECTION("Patterns are refused, not stored as regexes") {
        auto [cert, key] = generate_test_certificate("regex");
        const auto invalid = boost::system::errc::invalid_argument;
        for (const auto& pattern : {"^api\\..*\\.com$", ".*\\.internal$", "^(www|api)\\.example\\.com$"}) {
            REQUIRE(mgr->set_certificate(pattern, cert, key) == invalid);
            REQUIRE(mgr->set_certificate(pattern, ready_made({cert, key})) == invalid);
        }
        REQUIRE(mgr->get_registered_hosts().empty());
        REQUIRE(mgr->get_certificate("api.test.com") == nullptr);
        REQUIRE(mgr->get_certificate("server.internal") == nullptr);
        REQUIRE(mgr->get_certificate("www.example.com") == nullptr);
    }
}

TEST_CASE("Certificate manager host names: exact, one-label wildcards, case-insensitive", "[certificate_manager][sni]") {
    auto mgr = std::make_shared<certificate_manager>();
    auto exact = generate_test_certificate("api.example.com");
    auto wildcard = generate_test_certificate("*.example.com");

    SECTION("An exact name matches itself only, in any letter case") {
        REQUIRE_FALSE(mgr->set_certificate("API.Example.COM", exact.first, exact.second));
        REQUIRE(get_certificate_cn(mgr->get_certificate("api.example.com")) == "api.example.com");
        REQUIRE(mgr->has_certificate("Api.Example.Com"));
        // no character of the name is a pattern
        REQUIRE(mgr->get_certificate("apiXexample.com") == nullptr);
        REQUIRE(mgr->get_certificate("apiXexampleXcom") == nullptr);
        REQUIRE_FALSE(mgr->has_certificate("x.api.example.com"));
        REQUIRE_FALSE(mgr->has_certificate("api.example.co"));
    }

    SECTION("A wildcard covers one more label") {
        REQUIRE_FALSE(mgr->set_certificate("*.example.com", wildcard.first, wildcard.second));
        auto entry = mgr->get_certificate("*.example.com");
        REQUIRE(entry != nullptr);
        REQUIRE(mgr->get_certificate("a.example.com") == entry);
        REQUIRE(mgr->get_certificate("A.Example.Com") == entry);
        REQUIRE(mgr->get_certificate("example.com") == nullptr);
        REQUIRE(mgr->get_certificate("a.b.example.com") == nullptr);
        REQUIRE(mgr->get_certificate("aexample.com") == nullptr);
        REQUIRE(mgr->get_certificate(".example.com") == nullptr);
    }

    SECTION("An exact name wins over a wildcard, which applies again once it is removed") {
        REQUIRE_FALSE(mgr->set_certificate("*.example.com", wildcard.first, wildcard.second));
        auto www = generate_test_certificate("www.example.com");
        REQUIRE_FALSE(mgr->set_certificate("www.example.com", www.first, www.second));
        REQUIRE(get_certificate_cn(mgr->get_certificate("www.example.com")) == "www.example.com");
        REQUIRE(mgr->remove_certificate("www.example.com"));
        REQUIRE(get_certificate_cn(mgr->get_certificate("www.example.com")) == "*.example.com");
    }

    SECTION("Among wildcards, the one of the name less its first label") {
        REQUIRE_FALSE(mgr->set_certificate("*.example.com", wildcard.first, wildcard.second));
        REQUIRE_FALSE(mgr->set_certificate("*.b.example.com", ready_made(exact)));
        REQUIRE(get_certificate_cn(mgr->get_certificate("x.b.example.com")) == "api.example.com");
        REQUIRE(get_certificate_cn(mgr->get_certificate("b.example.com")) == "*.example.com");
        REQUIRE(mgr->get_certificate("y.x.b.example.com") == nullptr);
    }

    SECTION("Registered names, lowercase as registered") {
        REQUIRE_FALSE(mgr->set_certificate("API.example.com", exact.first, exact.second));
        REQUIRE_FALSE(mgr->set_certificate("*.Example.com", wildcard.first, wildcard.second));
        REQUIRE(mgr->get_registered_hosts() == std::set<std::string>{"api.example.com", "*.example.com"});
        // the same name in another case replaces it
        REQUIRE_FALSE(mgr->set_certificate("api.EXAMPLE.com", wildcard.first, wildcard.second));
        REQUIRE(mgr->get_registered_hosts().size() == 2);
        REQUIRE(get_certificate_cn(mgr->get_certificate("api.example.com")) == "*.example.com");
    }

    SECTION("The handshake serves the certificate of the name, in any letter case, and no other") {
        auto fallback = generate_test_certificate("default");
        REQUIRE_FALSE(mgr->set_default_certificate(fallback.first, fallback.second));
        REQUIRE_FALSE(mgr->set_certificate("api.example.com", exact.first, exact.second));
        REQUIRE_FALSE(mgr->set_certificate("*.wild.example.com", wildcard.first, wildcard.second));
        auto listener = mgr->get_default_certificate();
        REQUIRE(presented_certificate(listener, "Api.EXAMPLE.com") == "api.example.com");
        REQUIRE(presented_certificate(listener, "apiXexample.com") == "default");
        REQUIRE(presented_certificate(listener, "A.Wild.Example.COM") == "*.example.com");
        REQUIRE(presented_certificate(listener, "a.b.wild.example.com") == "default");
    }
}

TEST_CASE("Certificate manager certificates of names that are not host names", "[certificate_manager][sni]") {
    auto mgr = std::make_shared<certificate_manager>();
    auto [cert, key] = generate_test_certificate("api.example.com");
    const auto invalid = boost::system::errc::invalid_argument;

    SECTION("Refused by both setters, nothing stored") {
        std::string long_name;
        for (int i = 0; i < 4; ++i) long_name += std::string(62, 'a') + ".";
        REQUIRE(long_name.size() == 252);
        for (const auto& name : std::vector<std::string>{"", "^api\\..*\\.com$", ".*\\.internal$", "a..example.com",
                                                         "*.", "*", "a*.example.com", "*.*.example.com", "exa mple.com",
                                                         std::string(64, 'a') + ".com", long_name + "ab"}) {
            INFO("name: " << name);
            REQUIRE(mgr->set_certificate(name, cert, key) == invalid);
            REQUIRE(mgr->set_certificate(name, ready_made({cert, key})) == invalid);
            REQUIRE_FALSE(mgr->has_certificate(name));
            REQUIRE(mgr->get_registered_hosts().empty());
        }
        // the longest name and label allowed
        REQUIRE_FALSE(mgr->set_certificate(long_name + "a", cert, key));
        REQUIRE_FALSE(mgr->set_certificate(std::string(63, 'a') + ".com", cert, key));
        REQUIRE(mgr->get_registered_hosts().size() == 2);
    }

    SECTION("A name refused leaves the certificates in place") {
        REQUIRE_FALSE(mgr->set_certificate("api.example.com", cert, key));
        auto other = generate_test_certificate("other");
        REQUIRE(mgr->set_certificate("api..example.com", other.first, other.second) == invalid);
        REQUIRE(get_certificate_cn(mgr->get_certificate("api.example.com")) == "api.example.com");
        REQUIRE(mgr->get_registered_hosts() == std::set<std::string>{"api.example.com"});
    }

    SECTION("remove_certificate() says whether it removed one") {
        REQUIRE_FALSE(mgr->remove_certificate("nothing.example.com"));
        REQUIRE_FALSE(mgr->set_certificate("api.example.com", cert, key));
        REQUIRE(mgr->remove_certificate("API.example.com"));
        REQUIRE_FALSE(mgr->has_certificate("api.example.com"));
        REQUIRE_FALSE(mgr->remove_certificate("API.example.com"));

        // a wildcard by its own name only
        REQUIRE_FALSE(mgr->set_certificate("*.example.com", cert, key));
        REQUIRE_FALSE(mgr->remove_certificate("a.example.com"));
        REQUIRE(mgr->has_certificate("a.example.com"));
        REQUIRE(mgr->remove_certificate("*.Example.com"));
        REQUIRE(mgr->get_registered_hosts().empty());
    }

    SECTION("The default host is compared case-insensitively") {
        mgr->set_default_host("API.example.com");
        REQUIRE_FALSE(mgr->set_certificate("api.example.com", cert, key));
        REQUIRE(mgr->get_default_certificate() == mgr->get_certificate("api.example.com"));
        REQUIRE_FALSE(mgr->set_certificate("Api.Example.com", ready_made(generate_test_certificate("replaced"))));
        REQUIRE(get_certificate_cn(mgr->get_default_certificate()) == "replaced");
    }
}

TEST_CASE("Certificate manager TLS options", "[certificate_manager][tls]") {
    auto mgr = std::make_shared<certificate_manager>();
    auto [cert, key] = generate_test_certificate("tls.example.com");

    SECTION("Mozilla intermediate by default, on every context created") {
        auto options = mgr->get_tls_options();
        REQUIRE(options.min_version == tls_version::tls1_2);
        REQUIRE(options.max_version == tls_version::tls1_3);
        REQUIRE_FALSE(options.prefer_server_ciphers);

        REQUIRE_FALSE(mgr->set_certificate("tls.example.com", cert, key));
        REQUIRE_FALSE(mgr->set_default_certificate(cert, key));
        auto host = mgr->get_certificate("tls.example.com");
        auto fallback = mgr->get_default_certificate();
        // the self-signed one, of a manager that never had a default certificate
        auto self_signed = std::make_shared<certificate_manager>()->get_default_certificate();

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
        REQUIRE_FALSE(mgr->set_certificate("before.example.com", cert, key));
        tls_options options;
        options.min_version = tls_version::tls1_3;
        options.ciphersuites = "TLS_AES_256_GCM_SHA384";
        options.prefer_server_ciphers = true;
        options.groups = "X25519:prime256v1";
        options.security_level = 1;
        REQUIRE_FALSE(mgr->set_tls_options(options));
        REQUIRE(mgr->get_tls_options().security_level == 1);
        REQUIRE_FALSE(mgr->set_certificate("tls.example.com", cert, key));

        auto ctx = mgr->get_certificate("tls.example.com");
        REQUIRE(SSL_CTX_get_min_proto_version(ctx->native_handle()) == TLS1_3_VERSION);
        REQUIRE(cipher_names(ctx, true) == std::set<std::string>{"TLS_AES_256_GCM_SHA384"});
        REQUIRE(prefers_server_ciphers(ctx));
        REQUIRE(SSL_CTX_get_security_level(ctx->native_handle()) == 1);
        // the ones created before keep theirs
        auto before = mgr->get_certificate("before.example.com");
        REQUIRE(SSL_CTX_get_min_proto_version(before->native_handle()) == TLS1_2_VERSION);
    }

    SECTION("Options OpenSSL refuses are not set") {
        auto refused = [&](auto change) {
            tls_options options;
            change(options);
            return mgr->set_tls_options(options) == boost::system::errc::invalid_argument;
        };
        REQUIRE(refused([](tls_options& options) { options.ciphers = "NOT-A-CIPHER"; }));
        REQUIRE(refused([](tls_options& options) { options.ciphersuites = "TLS_NOT_A_SUITE"; }));
        REQUIRE(refused([](tls_options& options) { options.groups = "not-a-curve"; }));
        REQUIRE(refused([](tls_options& options) { options.security_level = 6; }));
        REQUIRE(mgr->get_tls_options().ciphers == tls_options{}.ciphers);
        REQUIRE(mgr->get_tls_options().ciphersuites == tls_options{}.ciphersuites);
    }

    SECTION("The deprecated setters change the options, in any order") {
        auto legacy_first = [&] {
            REQUIRE_FALSE(mgr->set_tls_options({}));
            mgr->enable_legacy_protocols(true);
            REQUIRE_FALSE(mgr->set_server_ciphers("ECDHE-RSA-AES128-GCM-SHA256", true));
            return mgr->get_tls_options();
        }();
        auto ciphers_first = [&] {
            REQUIRE_FALSE(mgr->set_tls_options({}));
            REQUIRE_FALSE(mgr->set_server_ciphers("ECDHE-RSA-AES128-GCM-SHA256", true));
            mgr->enable_legacy_protocols(true);
            return mgr->get_tls_options();
        }();
        for (const auto& options : {legacy_first, ciphers_first}) {
            REQUIRE(options.min_version == tls_version::tls1_0);
            REQUIRE(options.security_level == 0);
            REQUIRE(options.ciphers == "ECDHE-RSA-AES128-GCM-SHA256");
            REQUIRE(options.prefer_server_ciphers);
        }
        // undone, keeping the ciphers chosen
        mgr->enable_legacy_protocols(false);
        REQUIRE(mgr->get_tls_options().min_version == tls_version::tls1_2);
        REQUIRE_FALSE(mgr->get_tls_options().security_level);
        REQUIRE(mgr->get_tls_options().ciphers == "ECDHE-RSA-AES128-GCM-SHA256");
    }

    SECTION("The deprecated setters are validated") {
        REQUIRE(mgr->set_server_ciphers("NOT-A-CIPHER", true) == boost::system::errc::invalid_argument);
        REQUIRE(mgr->get_tls_options().ciphers == tls_options{}.ciphers);
        REQUIRE_FALSE(mgr->get_tls_options().prefer_server_ciphers);
    }

    SECTION("Disabling the legacy protocols undoes only what enabling them did") {
        tls_options options;
        options.min_version = tls_version::tls1_3;
        options.security_level = 3;
        REQUIRE_FALSE(mgr->set_tls_options(options));
        // not enabled: nothing to undo
        mgr->enable_legacy_protocols(false);
        REQUIRE(mgr->get_tls_options().min_version == tls_version::tls1_3);
        REQUIRE(mgr->get_tls_options().security_level == 3);
        // enabled and disabled: back to what was set
        mgr->enable_legacy_protocols(true);
        REQUIRE(mgr->get_tls_options().min_version == tls_version::tls1_0);
        mgr->enable_legacy_protocols(false);
        REQUIRE(mgr->get_tls_options().min_version == tls_version::tls1_3);
        REQUIRE(mgr->get_tls_options().security_level == 3);
        REQUIRE(mgr->get_tls_options().ciphers == tls_options{}.ciphers);
        // ciphers chosen while enabled stay, also OpenSSL's defaults chosen explicitly
        mgr->enable_legacy_protocols(true);
        REQUIRE_FALSE(mgr->set_server_ciphers("", false));
        mgr->enable_legacy_protocols(false);
        REQUIRE(mgr->get_tls_options().ciphers.empty());
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
        REQUIRE(apply_tls_options(*ctx, mgr->get_tls_options()));
        REQUIRE(SSL_CTX_get_min_proto_version(ctx->native_handle()) == TLS1_2_VERSION);
        REQUIRE(cipher_names(ctx, false).size() == 6);
    }

}

TEST_CASE("Certificate manager contexts refuse renegotiation", "[certificate_manager][tls]") {
#ifdef SSL_OP_NO_RENEGOTIATION
    auto refuses = [](const std::shared_ptr<boost::asio::ssl::context>& ctx) {
        return (SSL_CTX_get_options(ctx->native_handle()) & SSL_OP_NO_RENEGOTIATION) != 0;
    };
    auto mgr = std::make_shared<certificate_manager>();
    auto [cert, key] = generate_test_certificate("tls.example.com");

    SECTION("Those it creates: from PEM and its self-signed default") {
        REQUIRE_FALSE(mgr->set_certificate("tls.example.com", cert, key));
        auto host = mgr->get_certificate("tls.example.com");
        REQUIRE(host);
        REQUIRE(refuses(host));
        auto self_signed = mgr->get_default_certificate();
        REQUIRE(self_signed);
        REQUIRE(refuses(self_signed));
    }

    SECTION("Not one passed in ready-made: it keeps its own options") {
        auto ready = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
        ready->use_certificate_chain(boost::asio::buffer(cert));
        ready->use_private_key(boost::asio::buffer(key), boost::asio::ssl::context::pem);
        const auto options = SSL_CTX_get_options(ready->native_handle());
        REQUIRE_FALSE(options & SSL_OP_NO_RENEGOTIATION);

        REQUIRE_FALSE(mgr->set_certificate("tls.example.com", ready));
        REQUIRE(mgr->get_certificate("tls.example.com") == ready);
        REQUIRE_FALSE(mgr->set_default_certificate(ready));
        REQUIRE(SSL_CTX_get_options(ready->native_handle()) == options);
    }
#else
    SKIP("this OpenSSL has no SSL_OP_NO_RENEGOTIATION");
#endif
}

TEST_CASE("Certificate manager client certificate modes per host", "[certificate_manager][mtls]") {
    auto manager = std::make_shared<certificate_manager>();

    SECTION("Exact names and wildcards, case-insensitive") {
        REQUIRE_FALSE(manager->set_client_verify("Devices.Example.com", client_verify::required));
        REQUIRE(manager->get_client_verify("devices.example.com") == client_verify::required);
        REQUIRE(manager->get_client_verify("DEVICES.EXAMPLE.COM") == client_verify::required);
        REQUIRE_FALSE(manager->set_client_verify("*.wild.example.com", client_verify::optional));
        REQUIRE(manager->get_client_verify("a.wild.example.com") == client_verify::optional);
        REQUIRE_FALSE(manager->get_client_verify("a.b.wild.example.com"));
        REQUIRE_FALSE(manager->get_client_verify("wild.example.com"));
        REQUIRE_FALSE(manager->get_client_verify("other.example.com"));
        // no character of a name is a pattern
        REQUIRE_FALSE(manager->get_client_verify("devicesXexample.com"));
        REQUIRE_FALSE(manager->get_client_verify("aXwild.example.com"));
        REQUIRE(manager->get_client_verify("A.WILD.example.com") == client_verify::optional);
        // replaced
        REQUIRE_FALSE(manager->set_client_verify("devices.example.com", client_verify::optional));
        REQUIRE(manager->get_client_verify("devices.example.com") == client_verify::optional);

        REQUIRE(manager->remove_client_verify("devices.example.com"));
        REQUIRE(manager->remove_client_verify("*.wild.example.com"));
        REQUIRE_FALSE(manager->get_client_verify("devices.example.com"));
        REQUIRE_FALSE(manager->get_client_verify("a.wild.example.com"));
        REQUIRE_FALSE(manager->remove_client_verify("devices.example.com"));
    }

    SECTION("Names that are not host names are refused") {
        for (const auto& name : std::vector<std::string>{"", "*", "*.", "bad host", "a*.example.com", "a..example.com",
                                                         ".example.com", "example.com.", "host/path",
                                                         std::string(64, 'a') + ".com"}) {
            REQUIRE(manager->set_client_verify(name, client_verify::required) == boost::system::errc::invalid_argument);
        }
        REQUIRE_FALSE(manager->set_client_verify("under_score-1.example.com", client_verify::none));
        REQUIRE(manager->remove_client_verify("under_score-1.example.com"));
    }

    SECTION("Set and read from several threads") {
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([manager, t] {
                for (int i = 0; i < 200; ++i) {
                    auto host = "host" + std::to_string(t) + "-" + std::to_string(i % 10) + ".example.com";
                    manager->set_client_verify(host, client_verify::optional);
                    manager->get_client_verify(host);
                    manager->remove_client_verify(host);
                }
            });
        }
        for (auto& thread : threads) thread.join();
        REQUIRE_FALSE(manager->get_client_verify("host0-0.example.com"));
    }
}

TEST_CASE("Handshake record of a manager gone", "[certificate_manager][mtls][sni]") {
    // A connection whose handshake applied `required` for its SNI name, and a Host that
    // would get `none`
    auto manager = std::make_shared<certificate_manager>();
    REQUIRE_FALSE(manager->set_client_verify("devices.example.com", client_verify::required));
    tls_handshake_record record;
    record.name = "devices.example.com";
    record.has_name = true;
    record.mode = client_verify::required;
    record.manager = manager;
    REQUIRE(record.misdirected("www.example.com"));
    REQUIRE_FALSE(record.misdirected("Devices.Example.com."));

    // No per-host table left to compare: another name fails closed, the same one is served,
    // and a record no manager wrote serves any
    manager.reset();
    REQUIRE(record.misdirected("www.example.com"));
    REQUIRE_FALSE(record.misdirected("Devices.Example.com."));
    REQUIRE_FALSE(tls_handshake_record{}.misdirected("www.example.com"));
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

TEST_CASE("Certificate manager destroyed while a context of its own is in use", "[certificate_manager][tls]") {
    auto [listener_cert, listener_key] = generate_test_certificate("listener");
    auto [other_cert, other_key] = generate_test_certificate("other");
    auto [replaced_cert, replaced_key] = generate_test_certificate("replaced");

    SECTION("Contexts it created") {
        auto mgr = std::make_shared<certificate_manager>();
        REQUIRE_FALSE(mgr->set_default_certificate(listener_cert, listener_key));
        REQUIRE_FALSE(mgr->set_certificate("other.test", other_cert, other_key));
        auto listener = mgr->get_default_certificate();
        auto host = mgr->get_certificate("other.test");
        // alive, SNI switches
        REQUIRE(presented_certificate(listener, "other.test") == "other");

        // a default replaced the handshakes of the listener would switch to, were it alive
        REQUIRE_FALSE(mgr->set_default_certificate(replaced_cert, replaced_key));
        std::weak_ptr<certificate_manager> watch = mgr;
        mgr.reset();
        REQUIRE(watch.expired());

        // gone: the handshakes stay on the context they started with, SNI or not
        REQUIRE(presented_certificate(listener, "other.test") == "listener");
        REQUIRE(presented_certificate(listener, nullptr) == "listener");
        REQUIRE(presented_certificate(listener, "unknown.test") == "listener");
        // a listener on the certificate of a host as well
        REQUIRE(presented_certificate(host, nullptr) == "other");
        SSL* ssl = server_after_hello(listener, "other.test");
        REQUIRE(SSL_get_SSL_CTX(ssl) == listener->native_handle());
        SSL_free(ssl);
    }

    SECTION("Contexts given ready-made") {
        auto mgr = std::make_shared<certificate_manager>();
        auto listener = ready_made({listener_cert, listener_key});
        auto host = ready_made({other_cert, other_key});
        mgr->set_default_certificate(listener);
        REQUIRE_FALSE(mgr->set_certificate("other.test", host));
        REQUIRE(presented_certificate(listener, "other.test") == "other");
        REQUIRE(presented_certificate(host, nullptr) == "listener");

        mgr.reset();
        REQUIRE(presented_certificate(listener, "other.test") == "listener");
        REQUIRE(presented_certificate(listener, nullptr) == "listener");
        REQUIRE(presented_certificate(host, nullptr) == "other");
        REQUIRE(presented_certificate(host, "other.test") == "other");
    }
}

TEST_CASE("Certificate manager reference of a context is freed with the context", "[certificate_manager][tls]") {
    auto [cert, key] = generate_test_certificate("freed");
    int live = 0;

    SECTION("A context it created, freed before the manager") {
        auto mgr = counted_manager(&live);
        REQUIRE(live == 1);
        REQUIRE_FALSE(mgr->set_certificate("freed.test", cert, key));
        REQUIRE(mgr->remove_certificate("freed.test"));
        mgr.reset();
        // nothing else held the manager's block
        REQUIRE(live == 0);
    }

    SECTION("A context it created, freed after the manager") {
        auto mgr = counted_manager(&live);
        REQUIRE_FALSE(mgr->set_default_certificate(cert, key));
        auto context = mgr->get_default_certificate();
        mgr.reset();
        // the reference kept with the context outlives the manager, and goes with the context
        REQUIRE(live == 1);
        context.reset();
        REQUIRE(live == 0);
    }

    SECTION("A context given ready-made, more than once") {
        auto mgr = counted_manager(&live);
        auto given = ready_made({cert, key});
        REQUIRE_FALSE(mgr->set_certificate("a.test", given));
        REQUIRE_FALSE(mgr->set_certificate("b.test", given));
        mgr->set_default_certificate(given);
        mgr.reset();
        REQUIRE(live == 1);
        given.reset();
        REQUIRE(live == 0);
    }

    SECTION("A context given to another manager afterwards") {
        int other_live = 0;
        auto first = counted_manager(&live);
        auto second = counted_manager(&other_live);
        auto given = ready_made({cert, key});
        REQUIRE_FALSE(first->set_certificate("given.test", given));
        REQUIRE_FALSE(second->set_certificate("given.test", given));
        // the reference to the first was replaced (and freed) by the one to the second
        first.reset();
        REQUIRE(live == 0);
        second.reset();
        REQUIRE(other_live == 1);
        given.reset();
        REQUIRE(other_live == 0);
    }
}

TEST_CASE("Certificate manager setters return an error_code", "[certificate_manager][tls]") {
    auto mgr = std::make_shared<certificate_manager>();
    auto [current_cert, current_key] = generate_test_certificate("current");
    auto [other_cert, other_key] = generate_test_certificate("other");

    SECTION("Empty on success, and the certificate in use") {
        boost::system::error_code ec = mgr->set_certificate("host.test", current_cert, current_key);
        REQUIRE_FALSE(ec);
        REQUIRE(ec.value() == 0);
        REQUIRE(get_certificate_cn(mgr->get_certificate("host.test")) == "current");
        ec = mgr->set_default_certificate(current_cert, current_key);
        REQUIRE_FALSE(ec);
        REQUIRE(get_certificate_cn(mgr->get_default_certificate()) == "current");
        REQUIRE_FALSE(mgr->set_certificate("given.test", ready_made({other_cert, other_key})));
        REQUIRE_FALSE(mgr->set_tls_options({}));
        REQUIRE_FALSE(mgr->set_client_verify("devices.test", client_verify::required));
        REQUIRE_FALSE(mgr->set_server_ciphers("", false));
    }

    SECTION("invalid_argument for certificates that cannot be used, keeping the ones in use") {
        REQUIRE_FALSE(mgr->set_certificate("host.test", current_cert, current_key));
        REQUIRE_FALSE(mgr->set_default_certificate(current_cert, current_key));
        const auto invalid = boost::system::errc::invalid_argument;

        // a PEM that is not one
        REQUIRE(mgr->set_certificate("host.test", "not a certificate", other_key) == invalid);
        REQUIRE(mgr->set_default_certificate(other_cert, "not a key") == invalid);
        // the key of another certificate
        REQUIRE(mgr->set_certificate("host.test", other_cert, current_key) == invalid);
        REQUIRE(mgr->set_default_certificate(other_cert, current_key) == invalid);
        // no host name, or no context
        REQUIRE(mgr->set_certificate("", other_cert, other_key) == invalid);
        REQUIRE(mgr->set_certificate("", ready_made({other_cert, other_key})) == invalid);
        REQUIRE(mgr->set_certificate("given.test", nullptr) == invalid);

        REQUIRE(get_certificate_cn(mgr->get_certificate("host.test")) == "current");
        REQUIRE(get_certificate_cn(mgr->get_default_certificate()) == "current");
        REQUIRE(mgr->get_registered_hosts() == std::set<std::string>{"host.test"});
    }

    SECTION("invalid_argument for TLS options OpenSSL refuses, keeping the ones set") {
        tls_options options;
        options.min_version = tls_version::tls1_3;
        options.groups = "X25519";
        REQUIRE_FALSE(mgr->set_tls_options(options));
        auto refused = options;
        refused.ciphers = "NOT-A-CIPHER";
        REQUIRE(mgr->set_tls_options(refused) == boost::system::errc::invalid_argument);
        REQUIRE(mgr->set_server_ciphers("NOT-A-CIPHER", true) == boost::system::errc::invalid_argument);
        auto kept = mgr->get_tls_options();
        REQUIRE(kept.min_version == tls_version::tls1_3);
        REQUIRE(kept.groups == "X25519");
        REQUIRE(kept.ciphers == tls_options{}.ciphers);
        REQUIRE_FALSE(kept.prefer_server_ciphers);
    }

    SECTION("invalid_argument for a name that is not a host name, keeping the modes set") {
        REQUIRE_FALSE(mgr->set_client_verify("devices.test", client_verify::required));
        REQUIRE(mgr->set_client_verify("devices test", client_verify::none) == boost::system::errc::invalid_argument);
        REQUIRE(mgr->set_client_verify("", client_verify::none) == boost::system::errc::invalid_argument);
        REQUIRE(mgr->get_client_verify("devices.test") == client_verify::required);
    }

    SECTION("remove_default_certificate(): none, and listeners keep theirs") {
        REQUIRE_FALSE(mgr->set_default_certificate(current_cert, current_key));
        auto listener = mgr->get_default_certificate();
        mgr->remove_default_certificate();
        // no self-signed one either
        REQUIRE(mgr->get_default_certificate() == nullptr);
        REQUIRE(mgr->get_default_certificate() == nullptr);
        // the handshakes of a listener started with it stay on it
        REQUIRE(presented_certificate(listener, nullptr) == "current");
        REQUIRE(presented_certificate(listener, "unknown.test") == "current");

        // a default set again is the default
        REQUIRE_FALSE(mgr->set_default_certificate(other_cert, other_key));
        REQUIRE(get_certificate_cn(mgr->get_default_certificate()) == "other");
        REQUIRE(presented_certificate(listener, nullptr) == "other");
    }

    SECTION("A null default context after remove_default_certificate(): refused, no self-signed one") {
        mgr->remove_default_certificate();
        REQUIRE(mgr->set_default_certificate(nullptr) == boost::system::errc::invalid_argument);
        REQUIRE(mgr->get_default_certificate() == nullptr);
        REQUIRE(mgr->get_default_certificate() == nullptr);
    }

    SECTION("A null default context: refused, the default and the handshakes keep the current one") {
        REQUIRE_FALSE(mgr->set_default_certificate(current_cert, current_key));
        auto listener = mgr->get_default_certificate();
        REQUIRE(mgr->set_default_certificate(nullptr) == boost::system::errc::invalid_argument);
        REQUIRE(mgr->get_default_certificate() == listener);
        REQUIRE(get_certificate_cn(mgr->get_default_certificate()) == "current");
        REQUIRE(presented_certificate(listener, nullptr) == "current");
    }

    SECTION("A default context given ready made: empty, and it is the default") {
        auto given = ready_made({other_cert, other_key});
        boost::system::error_code ec = mgr->set_default_certificate(given);
        REQUIRE_FALSE(ec);
        REQUIRE(mgr->get_default_certificate() == given);
        REQUIRE(presented_certificate(given, nullptr) == "other");
    }
}

namespace {

    // A TLS listener on the loopback, on a port chosen by the system, with `manager` and
    // `cache`: opened only, nothing is accepted (the io_context never runs)
    struct cache_listener {
        boost::asio::io_context io_context;
        listener server;

        cache_listener(std::shared_ptr<certificate_manager> manager, session_cache_options cache, uint16_t port = 0)
            : server(options(std::move(manager), cache, port), provider(), provider()) {}

        executor_provider provider() {
            return [this] { return boost::asio::any_io_executor(io_context.get_executor()); };
        }

        static listener_options options(std::shared_ptr<certificate_manager> manager, session_cache_options cache,
                                        uint16_t port) {
            listener_options result;
            result.endpoint = tcp_endpoint{"127.0.0.1", port};
            result.tls = std::move(manager);
            result.session_cache = cache;
            result.max_listening_attempts = 1;
            return result;
        }
    };

    // The session cache settings of a context, as OpenSSL reports them
    struct cache_settings {
        long mode;
        long size;
        bool no_ticket;
        size_t tickets;

        bool operator==(const cache_settings&) const = default;
    };

    cache_settings settings_of(const std::shared_ptr<boost::asio::ssl::context>& context) {
        SSL_CTX* handle = context->native_handle();
        return {SSL_CTX_get_session_cache_mode(handle), SSL_CTX_sess_get_cache_size(handle),
                (SSL_CTX_get_options(handle) & SSL_OP_NO_TICKET) != 0, SSL_CTX_get_num_tickets(handle)};
    }

    // Those of a context as OpenSSL creates it, and of one with no resumption at all
    const cache_settings enabled_settings{SSL_SESS_CACHE_SERVER, 20480, false, 2};
    const cache_settings disabled_settings{SSL_SESS_CACHE_OFF, 20480, true, 0};

    const session_cache_options disabled_cache{false, 20480};

}

TEST_CASE("Certificate manager session cache claimed by its listeners", "[certificate_manager][session_cache]") {
    const auto invalid = boost::system::errc::invalid_argument;
    auto mgr = std::make_shared<certificate_manager>();
    auto [cert, key] = generate_test_certificate("cache.test");
    REQUIRE_FALSE(mgr->set_default_certificate(cert, key));
    REQUIRE_FALSE(mgr->set_certificate("before.test", cert, key));
    auto given = ready_made({cert, key});
    REQUIRE_FALSE(mgr->set_certificate("given.test", given));

    SECTION("A claim applies its size to every context, held before and got after") {
        cache_listener first(mgr, {true, 1000});
        REQUIRE_FALSE(first.server.open());
        auto expected = cache_settings{SSL_SESS_CACHE_SERVER, 1000, false, 2};
        REQUIRE(settings_of(mgr->get_default_certificate()) == expected);
        REQUIRE(settings_of(mgr->get_certificate("before.test")) == expected);
        REQUIRE(settings_of(given) == expected);
        REQUIRE_FALSE(mgr->set_certificate("after.test", cert, key));
        REQUIRE(settings_of(mgr->get_certificate("after.test")) == expected);
        REQUIRE_FALSE(mgr->set_certificate("*.wildcard.test", ready_made({cert, key})));
        REQUIRE(settings_of(mgr->get_certificate("*.wildcard.test")) == expected);
        REQUIRE_FALSE(mgr->set_default_certificate(cert, key));
        REQUIRE(settings_of(mgr->get_default_certificate()) == expected);
    }

    SECTION("Disabled: no server cache, no tickets, on every context") {
        cache_listener first(mgr, disabled_cache);
        REQUIRE_FALSE(first.server.open());
        REQUIRE(settings_of(mgr->get_default_certificate()) == disabled_settings);
        REQUIRE(settings_of(mgr->get_certificate("before.test")) == disabled_settings);
        REQUIRE(settings_of(given) == disabled_settings);
        REQUIRE_FALSE(mgr->set_certificate("after.test", cert, key));
        REQUIRE(settings_of(mgr->get_certificate("after.test")) == disabled_settings);
    }

    SECTION("The default one leaves the contexts as OpenSSL creates them") {
        cache_listener first(mgr, {});
        REQUIRE_FALSE(first.server.open());
        REQUIRE(settings_of(mgr->get_default_certificate()) == enabled_settings);
        REQUIRE(settings_of(given) == enabled_settings);
    }

    SECTION("Enabled with a size of 0: refused, nothing bound") {
        cache_listener first(mgr, {true, 0});
        REQUIRE(first.server.open() == invalid);
        REQUIRE(first.server.local_port() == 0);
        REQUIRE(first.server.url().empty());
        REQUIRE(settings_of(mgr->get_default_certificate()) == enabled_settings);
        // disabled, its size does not matter
        cache_listener second(mgr, {false, 0});
        REQUIRE_FALSE(second.server.open());
        REQUIRE(settings_of(mgr->get_default_certificate()).mode == SSL_SESS_CACHE_OFF);
    }

    SECTION("Equal claims of two listeners sharing the manager") {
        cache_listener first(mgr, {true, 500});
        cache_listener second(mgr, {true, 500});
        REQUIRE_FALSE(first.server.open());
        REQUIRE_FALSE(second.server.open());
        REQUIRE(second.server.local_port() != 0);
        REQUIRE(SSL_CTX_sess_get_cache_size(mgr->get_default_certificate()->native_handle()) == 500);
    }

    SECTION("A conflict on enabled or on size: refused, nothing bound, the setting kept") {
        cache_listener first(mgr, {true, 500});
        REQUIRE_FALSE(first.server.open());
        for (auto other : {disabled_cache, session_cache_options{true, 501}}) {
            cache_listener second(mgr, other);
            REQUIRE(second.server.open() == invalid);
            REQUIRE(second.server.local_port() == 0);
            REQUIRE(second.server.url().empty());
        }
        REQUIRE(settings_of(mgr->get_default_certificate()) == cache_settings{SSL_SESS_CACHE_SERVER, 500, false, 2});
        // the refused listeners released nothing: the first one still holds it
        cache_listener third(mgr, disabled_cache);
        REQUIRE(third.server.open() == invalid);
    }

    SECTION("Different managers: each with its own setting") {
        auto other = std::make_shared<certificate_manager>();
        REQUIRE_FALSE(other->set_default_certificate(cert, key));
        cache_listener first(mgr, {true, 500});
        cache_listener second(other, disabled_cache);
        REQUIRE_FALSE(first.server.open());
        REQUIRE_FALSE(second.server.open());
        REQUIRE(SSL_CTX_sess_get_cache_size(mgr->get_default_certificate()->native_handle()) == 500);
        REQUIRE(settings_of(other->get_default_certificate()) == disabled_settings);
    }

    SECTION("Released by every holder: another setting may be claimed") {
        cache_listener first(mgr, disabled_cache);
        cache_listener second(mgr, disabled_cache);
        REQUIRE_FALSE(first.server.open());
        REQUIRE_FALSE(second.server.open());
        // released once, however many times it stops
        first.server.stop();
        first.server.stop();
        cache_listener refused(mgr, {});
        REQUIRE(refused.server.open() == invalid);
        // kept by the contexts while nobody claims another one
        second.server.stop();
        REQUIRE(settings_of(mgr->get_default_certificate()) == disabled_settings);
        REQUIRE(settings_of(given) == disabled_settings);
        // re-enabled: the contexts are as OpenSSL creates them again
        cache_listener third(mgr, {});
        REQUIRE_FALSE(third.server.open());
        REQUIRE(settings_of(mgr->get_default_certificate()) == enabled_settings);
        REQUIRE(settings_of(given) == enabled_settings);
    }

    SECTION("Released by a listener destroyed, and by an open() that fails to bind") {
        {
            cache_listener first(mgr, disabled_cache);
            REQUIRE_FALSE(first.server.open());
        }
        cache_listener busy(mgr, {});
        REQUIRE_FALSE(busy.server.open());
        busy.server.stop();
        // the port of a listener still open: binding fails after its one attempt
        cache_listener holder(std::make_shared<certificate_manager>(), {});
        REQUIRE_FALSE(holder.server.open());
        cache_listener failed(mgr, disabled_cache, holder.server.local_port());
        REQUIRE(failed.server.open());
        cache_listener last(mgr, {true, 100});
        REQUIRE_FALSE(last.server.open());
        REQUIRE(SSL_CTX_sess_get_cache_size(mgr->get_default_certificate()->native_handle()) == 100);
    }
}

TEST_CASE("Certificate manager session cache on its self-signed default", "[certificate_manager][session_cache]") {
    auto mgr = std::make_shared<certificate_manager>();
    cache_listener first(mgr, disabled_cache);
    // the self-signed default, generated by open(), has it too
    REQUIRE_FALSE(first.server.open());
    REQUIRE(settings_of(mgr->get_default_certificate()) == disabled_settings);
}

TEST_CASE("Certificate manager: requests misdirected on a handshake", "[certificate_manager][tls][sni]") {
    thinger::http::test::test_ca ca;
    auto mgr = std::make_shared<certificate_manager>();
    for (std::string name : {"devices.example.com", "*.example.com", "api.example.com"}) {
        auto certificate = ca.issue(name);
        REQUIRE_FALSE(mgr->set_certificate(name, certificate.first, certificate.second));
    }
    REQUIRE_FALSE(mgr->set_client_verify("devices.example.com", client_verify::required));

    // What a handshake with SNI `name` (none if empty) applied on a listener asking for none
    auto handshake = [&](const std::string& name) {
        tls_handshake_record record;
        record.has_name = !name.empty();
        record.name = name;
        record.mode = mgr->get_client_verify(name).value_or(client_verify::none);
        record.manager = mgr;
        return record;
    };

    SECTION("The same name is never misdirected, whatever changed") {
        auto devices = handshake("devices.example.com");
        REQUIRE_FALSE(devices.misdirected("devices.example.com"));
        REQUIRE_FALSE(devices.misdirected("DEVICES.example.COM."));
        REQUIRE(mgr->remove_client_verify("devices.example.com"));
        REQUIRE_FALSE(devices.misdirected("devices.example.com"));
    }

    SECTION("Another mode, either way") {
        REQUIRE(handshake("www.example.com").misdirected("devices.example.com"));
        REQUIRE(handshake("").misdirected("devices.example.com"));
        REQUIRE(handshake("devices.example.com").misdirected("www.example.com"));
        REQUIRE(handshake("devices.example.com").misdirected(""));
    }

    SECTION("Another certificate entry, with SNI only") {
        REQUIRE(handshake("www.other.org").misdirected("api.example.com"));
        REQUIRE(handshake("api.example.com").misdirected("www.example.com"));
        REQUIRE_FALSE(handshake("").misdirected("api.example.com"));
        REQUIRE_FALSE(handshake("").misdirected(""));
    }

    SECTION("The same configuration under another name") {
        REQUIRE_FALSE(handshake("a.example.com").misdirected("b.example.com"));
        REQUIRE_FALSE(handshake("one.example.org").misdirected("two.example.org"));
        REQUIRE_FALSE(handshake("one.example.org").misdirected(""));
        REQUIRE_FALSE(handshake("one.example.org").misdirected("10.0.0.1"));
    }

    SECTION("A certificate replaced keeps its entry") {
        auto wildcard = handshake("a.example.com");
        auto certificate = ca.issue("*.example.com");
        REQUIRE_FALSE(mgr->set_certificate("*.example.com", certificate.first, certificate.second));
        REQUIRE_FALSE(wildcard.misdirected("b.example.com"));
    }

    SECTION("A mode raised after the handshake") {
        auto www = handshake("www.example.com");
        REQUIRE_FALSE(www.misdirected("b.example.com"));
        REQUIRE_FALSE(mgr->set_client_verify("b.example.com", client_verify::optional));
        REQUIRE(www.misdirected("b.example.com"));
    }

    SECTION("The listener's mode for the hosts without one") {
        auto record = handshake("public.example.com");
        record.listener_mode = client_verify::required;
        REQUIRE(record.misdirected("api.example.com"));
        record.mode = client_verify::required;
        REQUIRE_FALSE(record.misdirected("b.example.com"));
    }

    SECTION("No manager: nothing is misdirected; a manager gone fails closed") {
        auto record = handshake("www.example.com");
        REQUIRE(record.misdirected("devices.example.com"));
        record.manager.reset();
        REQUIRE_FALSE(record.misdirected("devices.example.com"));
        auto gone = handshake("www.example.com");
        mgr.reset();
        REQUIRE(gone.misdirected("devices.example.com"));
        REQUIRE_FALSE(gone.misdirected("www.example.com"));
    }
}

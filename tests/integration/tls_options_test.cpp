#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/asio/ssl/certificate_manager.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include "../fixtures/test_ca.hpp"

using namespace thinger;

// TLS options of the server contexts (asio::tls_options): the protocol versions and ciphers a
// handshake accepts, by default and once set, with and without SNI

namespace {

    // What a client offers
    struct client_offer {
        int min_version = TLS1_VERSION;
        int max_version = TLS1_3_VERSION;
        // TLS 1.2 and older ciphers (OpenSSL's defaults if empty)
        std::string ciphers;
        std::string host;   // SNI, none if empty
    };

    // The result of a handshake with the server on localhost
    struct handshake_result {
        bool connected = false;
        std::string version;
        std::string cipher;     // standard name
        std::string subject;    // of the server certificate
    };

    handshake_result handshake(uint16_t port, const client_offer& offer) {
        boost::asio::io_context io_context;
        boost::asio::ssl::context context(boost::asio::ssl::context::tls_client);
        // A client able to speak the old versions, to see the server refuse them
        SSL_CTX_set_security_level(context.native_handle(), 0);
        SSL_CTX_set_min_proto_version(context.native_handle(), offer.min_version);
        SSL_CTX_set_max_proto_version(context.native_handle(), offer.max_version);
        SSL_CTX_set_cipher_list(context.native_handle(), offer.ciphers.empty() ? "DEFAULT@SECLEVEL=0" : offer.ciphers.c_str());
        boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream(io_context, context);

        handshake_result result;
        boost::system::error_code error;
        stream.next_layer().connect({boost::asio::ip::make_address("127.0.0.1"), port}, error);
        REQUIRE_FALSE(error);
        timeval timeout{5, 0};
        ::setsockopt(stream.next_layer().native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        if (!offer.host.empty()) SSL_set_tlsext_host_name(stream.native_handle(), offer.host.c_str());
        stream.handshake(boost::asio::ssl::stream_base::client, error);
        if (error) return result;

        result.connected = true;
        result.version = SSL_get_version(stream.native_handle());
        result.cipher = SSL_CIPHER_standard_name(SSL_get_current_cipher(stream.native_handle()));
        if (X509* certificate = SSL_get1_peer_certificate(stream.native_handle())) {
            char name[256] = {};
            X509_NAME_get_text_by_NID(X509_get_subject_name(certificate), NID_commonName, name, sizeof(name));
            result.subject = name;
            X509_free(certificate);
        }
        stream.shutdown(error);
        return result;
    }

    // The certificates and TLS options of the manager, restored at the end of the test
    struct saved_manager {
        std::shared_ptr<boost::asio::ssl::context> default_certificate = asio::certificate_manager::instance().get_default_certificate();
        asio::tls_options options = asio::certificate_manager::instance().get_tls_options();
        ~saved_manager() {
            auto& manager = asio::certificate_manager::instance();
            manager.set_tls_options(options);
            manager.set_default_certificate(default_certificate);
        }
    };

    // An HTTPS server on localhost, with a default certificate created from `default_certificate`
    // with the current TLS options
    struct https_server {
        http::server server;
        uint16_t port = 0;
        std::thread thread;

        explicit https_server(const std::pair<std::string, std::string>& default_certificate) {
            REQUIRE(asio::certificate_manager::instance().set_default_certificate(default_certificate.first,
                                                                                  default_certificate.second));
            server.enable_ssl(true);
            server.get("/hello", [](http::response& res) { res.send("hello"); });
            REQUIRE(server.listen("127.0.0.1", 0));
            port = server.local_port();
            thread = std::thread([this] { server.wait(); });
        }

        ~https_server() {
            server.stop();
            thread.join();
        }
    };

}

TEST_CASE("HTTPS server accepts TLS 1.2 and 1.3 by default", "[ssl][tls][options][integration]") {
    http::test::test_ca ca;
    saved_manager saved;
    REQUIRE(asio::certificate_manager::instance().set_tls_options({}));
    https_server https(ca.issue("default.test"));

    SECTION("TLS 1.0 and 1.1 are refused") {
        REQUIRE_FALSE(handshake(https.port, {.min_version = TLS1_VERSION, .max_version = TLS1_VERSION}).connected);
        REQUIRE_FALSE(handshake(https.port, {.min_version = TLS1_1_VERSION, .max_version = TLS1_1_VERSION}).connected);
    }

    SECTION("TLS 1.2 and 1.3") {
        auto tls12 = handshake(https.port, {.min_version = TLS1_2_VERSION, .max_version = TLS1_2_VERSION});
        REQUIRE(tls12.connected);
        REQUIRE(tls12.version == "TLSv1.2");
        auto tls13 = handshake(https.port, {});
        REQUIRE(tls13.connected);
        REQUIRE(tls13.version == "TLSv1.3");
    }

    SECTION("TLS 1.2 ciphers: ECDHE with AES-GCM or ChaCha20-Poly1305") {
        auto tls12 = [&](const std::string& ciphers) {
            return handshake(https.port, {.min_version = TLS1_2_VERSION, .max_version = TLS1_2_VERSION, .ciphers = ciphers});
        };
        REQUIRE(tls12("ECDHE-ECDSA-AES128-GCM-SHA256").cipher == "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256");
        REQUIRE(tls12("ECDHE-ECDSA-AES256-GCM-SHA384").cipher == "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384");
        REQUIRE(tls12("ECDHE-ECDSA-CHACHA20-POLY1305").cipher == "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256");
        // CBC, or without forward secrecy
        REQUIRE_FALSE(tls12("ECDHE-ECDSA-AES128-SHA").connected);
        REQUIRE_FALSE(tls12("ECDHE-ECDSA-AES128-SHA256").connected);
        REQUIRE_FALSE(tls12("AES128-GCM-SHA256").connected);
        REQUIRE_FALSE(tls12("ECDHE-ECDSA-DES-CBC3-SHA").connected);
    }

    SECTION("The client's cipher preference") {
        auto result = handshake(https.port, {.min_version = TLS1_2_VERSION, .max_version = TLS1_2_VERSION,
                                             .ciphers = "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES128-GCM-SHA256"});
        REQUIRE(result.cipher == "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256");
    }
}

TEST_CASE("HTTPS server with TLS options set", "[ssl][tls][options][integration]") {
    http::test::test_ca ca;
    saved_manager saved;
    auto& manager = asio::certificate_manager::instance();

    SECTION("The server's cipher preference") {
        asio::tls_options options;
        options.prefer_server_ciphers = true;
        REQUIRE(manager.set_tls_options(options));
        https_server https(ca.issue("default.test"));
        auto result = handshake(https.port, {.min_version = TLS1_2_VERSION, .max_version = TLS1_2_VERSION,
                                             .ciphers = "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES128-GCM-SHA256"});
        REQUIRE(result.cipher == "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256");
    }

    SECTION("TLS 1.3 only") {
        asio::tls_options options;
        options.min_version = asio::tls_version::tls1_3;
        REQUIRE(manager.set_tls_options(options));
        https_server https(ca.issue("default.test"));
        REQUIRE_FALSE(handshake(https.port, {.min_version = TLS1_2_VERSION, .max_version = TLS1_2_VERSION}).connected);
        REQUIRE(handshake(https.port, {}).version == "TLSv1.3");
    }

    SECTION("Legacy protocols") {
        manager.enable_legacy_protocols(true);
        https_server https(ca.issue("default.test"));
        auto tls11 = handshake(https.port, {.min_version = TLS1_1_VERSION, .max_version = TLS1_1_VERSION});
        REQUIRE(tls11.connected);
        REQUIRE(tls11.version == "TLSv1.1");
    }
}

TEST_CASE("HTTPS server applies the TLS options to the certificates selected by SNI",
          "[ssl][tls][options][integration]") {
    http::test::test_ca ca;
    saved_manager saved;
    auto& manager = asio::certificate_manager::instance();
    REQUIRE(manager.set_tls_options({}));
    auto host = ca.issue("sni.test");
    REQUIRE(manager.set_certificate("sni.test", host.first, host.second));
    https_server https(ca.issue("default.test"));

    auto context = manager.get_certificate("sni.test");
    REQUIRE(SSL_CTX_get_min_proto_version(context->native_handle()) == TLS1_2_VERSION);

    REQUIRE_FALSE(handshake(https.port, {.max_version = TLS1_1_VERSION, .host = "sni.test"}).connected);
    REQUIRE_FALSE(handshake(https.port, {.min_version = TLS1_2_VERSION, .max_version = TLS1_2_VERSION,
                                         .ciphers = "ECDHE-ECDSA-AES128-SHA", .host = "sni.test"}).connected);
    auto result = handshake(https.port, {.min_version = TLS1_2_VERSION, .max_version = TLS1_2_VERSION, .host = "sni.test"});
    REQUIRE(result.connected);
    REQUIRE(result.subject == "sni.test");
    REQUIRE(manager.remove_certificate("sni.test"));
}

TEST_CASE("HTTPS server keeps its TLS options for a weaker certificate given ready-made",
          "[ssl][tls][options][integration]") {
    http::test::test_ca ca;
    saved_manager saved;
    auto& manager = asio::certificate_manager::instance();
    REQUIRE(manager.set_tls_options({}));

    // A context made elsewhere, allowing TLS 1.0 and every cipher: SNI switches to its
    // certificate, but the versions and ciphers stay those of the context of the listener
    auto weak_certificate = ca.issue("weak.test");
    auto weak = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
    weak->use_certificate_chain(boost::asio::buffer(weak_certificate.first));
    weak->use_private_key(boost::asio::buffer(weak_certificate.second), boost::asio::ssl::context::pem);
    SSL_CTX_set_min_proto_version(weak->native_handle(), TLS1_VERSION);
    SSL_CTX_set_security_level(weak->native_handle(), 0);
    SSL_CTX_set_cipher_list(weak->native_handle(), "ALL:@SECLEVEL=0");
    REQUIRE(manager.set_certificate("weak.test", weak));
    https_server https(ca.issue("default.test"));

    REQUIRE_FALSE(handshake(https.port, {.max_version = TLS1_1_VERSION, .host = "weak.test"}).connected);
    REQUIRE_FALSE(handshake(https.port, {.min_version = TLS1_2_VERSION, .max_version = TLS1_2_VERSION,
                                         .ciphers = "ECDHE-ECDSA-AES128-SHA", .host = "weak.test"}).connected);
    auto result = handshake(https.port, {.min_version = TLS1_2_VERSION, .max_version = TLS1_2_VERSION, .host = "weak.test"});
    REQUIRE(result.connected);
    REQUIRE(result.subject == "weak.test");
    REQUIRE((result.cipher.find("GCM") != std::string::npos || result.cipher.find("CHACHA20") != std::string::npos));
    REQUIRE(manager.remove_certificate("weak.test"));
}

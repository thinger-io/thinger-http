#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/pool_server.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/asio/ssl/certificate_manager.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <openssl/bn.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <sys/socket.h>
#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "../fixtures/test_ca.hpp"

using namespace thinger;

// Certificates replaced while a server listens apply to the next handshakes, with and without
// SNI, as Node's setSecureContext() and Go's GetCertificate do; connections keep theirs

namespace {

    // Subject and serial number of a certificate
    struct certificate_id {
        std::string subject;
        std::string serial;

        bool operator==(const certificate_id&) const = default;
        auto operator<=>(const certificate_id&) const = default;
    };

    certificate_id id_of(X509* certificate) {
        if (!certificate) return {};
        char* subject = X509_NAME_oneline(X509_get_subject_name(certificate), nullptr, 0);
        BIGNUM* serial = ASN1_INTEGER_to_BN(X509_get0_serialNumber(certificate), nullptr);
        char* serial_hex = BN_bn2hex(serial);
        certificate_id id{subject, serial_hex};
        OPENSSL_free(serial_hex);
        BN_free(serial);
        OPENSSL_free(subject);
        return id;
    }

    certificate_id id_of(const std::pair<std::string, std::string>& certificate) {
        BIO* bio = BIO_new_mem_buf(certificate.first.data(), static_cast<int>(certificate.first.size()));
        X509* x509 = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        auto id = id_of(x509);
        X509_free(x509);
        BIO_free(bio);
        return id;
    }

    // A TLS connection to the server on localhost, with the host name given (none: no SNI)
    struct tls_connection {
        boost::asio::io_context io_context;
        boost::asio::ssl::context context{boost::asio::ssl::context::tls_client};
        boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream{io_context, context};
        boost::system::error_code error;

        tls_connection(uint16_t port, const std::string& host) {
            stream.next_layer().connect({boost::asio::ip::make_address("127.0.0.1"), port}, error);
            if (error) return;
            timeval timeout{5, 0};
            ::setsockopt(stream.next_layer().native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            if (!host.empty()) SSL_set_tlsext_host_name(stream.native_handle(), host.c_str());
            stream.handshake(boost::asio::ssl::stream_base::client, error);
        }

        certificate_id peer() {
            X509* certificate = SSL_get1_peer_certificate(stream.native_handle());
            auto id = id_of(certificate);
            X509_free(certificate);
            return id;
        }

        // The body of GET /hello, empty on errors
        std::string hello() {
            static const std::string request = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
            boost::asio::write(stream, boost::asio::buffer(request), error);
            if (error) return {};
            boost::asio::streambuf response;
            boost::asio::read_until(stream, response, "hello", error);
            if (error) return {};
            std::string data(boost::asio::buffers_begin(response.data()), boost::asio::buffers_end(response.data()));
            return data.substr(data.find("\r\n\r\n") + 4);
        }
    };

    certificate_id handshake(uint16_t port, const std::string& host = "") {
        tls_connection connection(port, host);
        REQUIRE_FALSE(connection.error);
        return connection.peer();
    }

    // The default certificate of the manager, restored at the end of the test
    struct saved_default_certificate {
        std::shared_ptr<boost::asio::ssl::context> saved = asio::certificate_manager::instance().get_default_certificate();
        ~saved_default_certificate() { asio::certificate_manager::instance().set_default_certificate(saved); }
    };

    // An HTTPS pool server on localhost (several worker threads) answering GET /hello
    struct https_server {
        http::pool_server server;
        uint16_t port = 0;

        https_server() {
            server.enable_ssl(true);
            server.get("/hello", [](http::request&, http::response& res) { res.send("hello"); });
            REQUIRE(server.listen("127.0.0.1", 0));
            port = server.local_port();
        }
    };

    void set_default(const std::pair<std::string, std::string>& certificate) {
        REQUIRE(asio::certificate_manager::instance().set_default_certificate(certificate.first, certificate.second));
    }

}

TEST_CASE("HTTPS server serves the default certificate replaced while listening",
          "[ssl][tls][certificate][integration]") {
    http::test::test_ca ca;
    saved_default_certificate saved;
    auto first = ca.issue("first.default.test");
    auto second = ca.issue("second.default.test");
    auto third = ca.issue("third.default.test");
    set_default(first);
    https_server https;

    REQUIRE(handshake(https.port) == id_of(first));

    set_default(second);
    REQUIRE(handshake(https.port) == id_of(second));
    // a host name without a certificate of its own also gets the default one
    REQUIRE(handshake(https.port, "unknown.test") == id_of(second));

    SECTION("Connections keep the certificate they took") {
        tls_connection connection(https.port, "");
        REQUIRE_FALSE(connection.error);
        REQUIRE(connection.peer() == id_of(second));
        // the context of the connection is no longer referenced by the manager
        set_default(third);
        REQUIRE(connection.hello() == "hello");
        REQUIRE(connection.peer() == id_of(second));
        REQUIRE(handshake(https.port) == id_of(third));
    }

    SECTION("An invalid certificate keeps the current one") {
        REQUIRE_FALSE(asio::certificate_manager::instance().set_default_certificate("not a certificate", "not a key"));
        REQUIRE(handshake(https.port) == id_of(second));
    }
}

TEST_CASE("HTTPS server serves the certificate of a host replaced while listening",
          "[ssl][tls][certificate][integration]") {
    http::test::test_ca ca;
    saved_default_certificate saved;
    auto fallback = ca.issue("default.test");
    auto first = ca.issue("host.test");
    auto second = ca.issue("host.test");
    set_default(fallback);
    http::test::server_certificate host("host.test", first);
    https_server https;

    REQUIRE(handshake(https.port, "host.test") == id_of(first));

    REQUIRE(asio::certificate_manager::instance().set_certificate("host.test", second.first, second.second));
    REQUIRE(handshake(https.port, "host.test") == id_of(second));
    REQUIRE(handshake(https.port) == id_of(fallback));

    // without its certificate, the host gets the default one
    asio::certificate_manager::instance().remove_certificate("host.test");
    REQUIRE(handshake(https.port, "host.test") == id_of(fallback));
}

namespace {

    // Certificates set over and over on another thread, until destroyed
    struct certificate_rotation {
        std::vector<std::pair<std::string, std::string>> defaults;
        std::vector<std::pair<std::string, std::string>> hosts;
        std::atomic<bool> stopping{false};
        std::atomic<int> rotations{0};
        std::thread thread;

        certificate_rotation(const http::test::test_ca& ca, size_t count) {
            for (size_t i = 0; i < count; ++i) {
                defaults.push_back(ca.issue("default.test"));
                hosts.push_back(ca.issue("rotating.test"));
            }
        }

        void start() {
            thread = std::thread([this] {
                auto& manager = asio::certificate_manager::instance();
                for (size_t i = 0; !stopping; ++i) {
                    const auto& certificate = defaults[i % defaults.size()];
                    const auto& host = hosts[i % hosts.size()];
                    manager.set_default_certificate(certificate.first, certificate.second);
                    manager.set_certificate("rotating.test", host.first, host.second);
                    manager.set_certificate("*.removed.test", host.first, host.second);
                    manager.remove_certificate("*.removed.test");
                    manager.enable_legacy_protocols(false);
                    ++rotations;
                }
            });
        }

        ~certificate_rotation() {
            stopping = true;
            if (thread.joinable()) thread.join();
            asio::certificate_manager::instance().remove_certificate("rotating.test");
        }

        static std::set<certificate_id> ids(const std::vector<std::pair<std::string, std::string>>& certificates) {
            std::set<certificate_id> result;
            for (const auto& certificate : certificates) result.insert(id_of(certificate));
            return result;
        }
    };

}

TEST_CASE("HTTPS server handshakes while the certificates are replaced",
          "[ssl][tls][certificate][integration]") {
    http::test::test_ca ca;
    saved_default_certificate saved;
    certificate_rotation rotation(ca, 6);
    set_default(rotation.defaults[0]);
    REQUIRE(asio::certificate_manager::instance().set_certificate(
        "rotating.test", rotation.hosts[0].first, rotation.hosts[0].second));
    https_server https;
    const auto default_ids = certificate_rotation::ids(rotation.defaults);
    const auto host_ids = certificate_rotation::ids(rotation.hosts);

    constexpr int clients = 4;
    constexpr int handshakes = 30;
    const std::string names[] = {"", "rotating.test", "unknown.test", "x.removed.test"};
    // *.removed.test comes and goes: either certificate
    auto expected = [&](const std::string& name, const certificate_id& peer) {
        if (name == "rotating.test") return host_ids.contains(peer);
        if (name == "x.removed.test" && host_ids.contains(peer)) return true;
        return default_ids.contains(peer);
    };
    std::atomic<int> failed{0};
    std::atomic<int> wrong{0};
    std::atomic<int> served{0};

    rotation.start();
    std::vector<std::thread> threads;
    for (int client = 0; client < clients; ++client) {
        threads.emplace_back([&, client] {
            for (int i = 0; i < handshakes; ++i) {
                const auto& name = names[(client + i) % std::size(names)];
                tls_connection connection(https.port, name);
                if (connection.error) {
                    ++failed;
                    continue;
                }
                if (!expected(name, connection.peer())) ++wrong;
                if (i % 5 == 0 && connection.hello() == "hello") ++served;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    rotation.stopping = true;

    CHECK(failed == 0);
    CHECK(wrong == 0);
    CHECK(served == clients * ((handshakes + 4) / 5));
    CHECK(rotation.rotations > 0);
}

TEST_CASE("HTTPS server selects certificates by SNI with a default context given",
          "[ssl][tls][certificate][integration]") {
    http::test::test_ca ca;
    saved_default_certificate saved;
    auto fallback = ca.issue("given.test");
    auto host_certificate = ca.issue("host.test");
    // a context the manager did not create: it installs its SNI callback on it
    auto given = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
    given->use_certificate_chain(boost::asio::buffer(fallback.first));
    given->use_private_key(boost::asio::buffer(fallback.second), boost::asio::ssl::context::pem);
    asio::certificate_manager::instance().set_default_certificate(given);
    http::test::server_certificate host("host.test", host_certificate);
    https_server https;

    REQUIRE(handshake(https.port) == id_of(fallback));
    REQUIRE(handshake(https.port, "host.test") == id_of(host_certificate));
}

TEST_CASE("HTTPS servers start listening while others handshake",
          "[ssl][tls][certificate][integration]") {
    http::test::test_ca ca;
    saved_default_certificate saved;
    auto certificate = ca.issue("shared.test");
    set_default(certificate);
    https_server https;

    // servers on the same default context start listening while the first one handshakes
    std::atomic<bool> stopping{false};
    std::atomic<int> started{0};
    std::thread listeners([&] {
        while (!stopping) {
            http::server other;
            other.enable_ssl(true);
            if (other.listen("127.0.0.1", 0)) ++started;
        }
    });
    int failed = 0;
    for (int i = 0; i < 40; ++i) {
        tls_connection connection(https.port, i % 2 ? "shared.test" : "");
        if (connection.error || connection.peer() != id_of(certificate)) ++failed;
    }
    stopping = true;
    listeners.join();

    CHECK(failed == 0);
    CHECK(started > 0);
}

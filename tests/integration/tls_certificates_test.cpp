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
#include <type_traits>
#include <vector>
#include "../fixtures/test_ca.hpp"
#include "../fixtures/running_workers.hpp"
#include "../fixtures/server_listen.hpp"

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
        // The Host of its requests: the name of the handshake, as a client sends it (another
        // configuration would get a 421)
        std::string host;

        tls_connection(uint16_t port, const std::string& name) : host(name.empty() ? "localhost" : name) {
            stream.next_layer().connect({boost::asio::ip::make_address("127.0.0.1"), port}, error);
            if (error) return;
            timeval timeout{5, 0};
            ::setsockopt(stream.next_layer().native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            if (!name.empty()) SSL_set_tlsext_host_name(stream.native_handle(), name.c_str());
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
            const std::string request = "GET /hello HTTP/1.1\r\nHost: " + host + "\r\n\r\n";
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

    // An HTTPS server on localhost with the certificates of `manager`, answering GET /hello: a
    // pool_server (several worker threads) or a server (on a thread of its own)
    template<typename Server>
    struct basic_https_server {
        http::test::running_workers workers;  // a pool_server's
        Server server = make_server();
        uint16_t port = 0;
        std::thread thread;

        explicit basic_https_server(const std::shared_ptr<asio::certificate_manager>& manager) {
            server.get("/hello", [](http::request&, http::response& res) { res.send("hello"); });
            REQUIRE_FALSE(server.listen(http::test::listen_options("127.0.0.1", 0, manager)));
            port = http::test::listening_port(server);
            if constexpr (std::is_same_v<Server, http::server>) thread = std::thread([this] { server.wait(); });
        }

        Server make_server() {
            if constexpr (std::is_same_v<Server, http::pool_server>) {
                return Server(workers);
            } else {
                return Server();
            }
        }

        ~basic_https_server() {
            if (!thread.joinable()) return;
            server.stop();
            thread.join();
        }
    };

    using https_server = basic_https_server<http::pool_server>;

    void set_default(asio::certificate_manager& manager, const std::pair<std::string, std::string>& certificate) {
        REQUIRE_FALSE(manager.set_default_certificate(certificate.first, certificate.second));
    }

}

TEST_CASE("HTTPS server serves the default certificate replaced while listening",
          "[ssl][tls][certificate][integration]") {
    http::test::test_ca ca;
    auto manager = std::make_shared<asio::certificate_manager>();
    auto first = ca.issue("first.default.test");
    auto second = ca.issue("second.default.test");
    auto third = ca.issue("third.default.test");
    set_default(*manager, first);
    https_server https(manager);

    REQUIRE(handshake(https.port) == id_of(first));

    set_default(*manager, second);
    REQUIRE(handshake(https.port) == id_of(second));
    // a host name without a certificate of its own also gets the default one
    REQUIRE(handshake(https.port, "unknown.test") == id_of(second));

    SECTION("Connections keep the certificate they took") {
        tls_connection connection(https.port, "");
        REQUIRE_FALSE(connection.error);
        REQUIRE(connection.peer() == id_of(second));
        // the context of the connection is no longer referenced by the manager
        set_default(*manager, third);
        REQUIRE(connection.hello() == "hello");
        REQUIRE(connection.peer() == id_of(second));
        REQUIRE(handshake(https.port) == id_of(third));
    }

    SECTION("An invalid certificate keeps the current one") {
        REQUIRE(manager->set_default_certificate("not a certificate", "not a key") == boost::system::errc::invalid_argument);
        REQUIRE(handshake(https.port) == id_of(second));
    }
}

TEST_CASE("HTTPS server serves the certificate of a host replaced while listening",
          "[ssl][tls][certificate][integration]") {
    http::test::test_ca ca;
    auto manager = std::make_shared<asio::certificate_manager>();
    auto fallback = ca.issue("default.test");
    auto first = ca.issue("host.test");
    auto second = ca.issue("host.test");
    set_default(*manager, fallback);
    http::test::server_certificate host(manager, "host.test", first);
    https_server https(manager);

    REQUIRE(handshake(https.port, "host.test") == id_of(first));

    REQUIRE_FALSE(manager->set_certificate("host.test", second.first, second.second));
    REQUIRE(handshake(https.port, "host.test") == id_of(second));
    REQUIRE(handshake(https.port) == id_of(fallback));

    // without its certificate, the host gets the default one
    manager->remove_certificate("host.test");
    REQUIRE(handshake(https.port, "host.test") == id_of(fallback));
}

namespace {

    // Certificates set over and over on another thread, until destroyed
    struct certificate_rotation {
        std::shared_ptr<asio::certificate_manager> manager;
        std::vector<std::pair<std::string, std::string>> defaults;
        std::vector<std::pair<std::string, std::string>> hosts;
        std::atomic<bool> stopping{false};
        std::atomic<int> rotations{0};
        std::thread thread;

        certificate_rotation(std::shared_ptr<asio::certificate_manager> certificates, const http::test::test_ca& ca,
                             size_t count)
            : manager(std::move(certificates)) {
            for (size_t i = 0; i < count; ++i) {
                defaults.push_back(ca.issue("default.test"));
                hosts.push_back(ca.issue("rotating.test"));
            }
        }

        void start() {
            thread = std::thread([this] {
                for (size_t i = 0; !stopping; ++i) {
                    const auto& certificate = defaults[i % defaults.size()];
                    const auto& host = hosts[i % hosts.size()];
                    manager->set_default_certificate(certificate.first, certificate.second);
                    manager->set_certificate("rotating.test", host.first, host.second);
                    manager->set_certificate("*.removed.test", host.first, host.second);
                    manager->remove_certificate("*.removed.test");
                    manager->enable_legacy_protocols(false);
                    ++rotations;
                }
            });
        }

        ~certificate_rotation() {
            stopping = true;
            if (thread.joinable()) thread.join();
            manager->remove_certificate("rotating.test");
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
    auto manager = std::make_shared<asio::certificate_manager>();
    certificate_rotation rotation(manager, ca, 6);
    set_default(*manager, rotation.defaults[0]);
    REQUIRE_FALSE(manager->set_certificate("rotating.test", rotation.hosts[0].first, rotation.hosts[0].second));
    https_server https(manager);
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

    // OpenSSL fills caches of its own on the first handshakes, with lock-free (RCU) lookups
    // that TSan, without OpenSSL instrumented, takes for data races: fill them before
    for (const auto& name : names) {
        tls_connection connection(https.port, name);
        REQUIRE_FALSE(connection.error);
        REQUIRE(connection.hello() == "hello");
    }

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
    auto manager = std::make_shared<asio::certificate_manager>();
    auto fallback = ca.issue("given.test");
    auto host_certificate = ca.issue("host.test");
    // a context the manager did not create: it installs its SNI callback on it
    auto given = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
    given->use_certificate_chain(boost::asio::buffer(fallback.first));
    given->use_private_key(boost::asio::buffer(fallback.second), boost::asio::ssl::context::pem);
    manager->set_default_certificate(given);
    http::test::server_certificate host(manager, "host.test", host_certificate);
    https_server https(manager);

    REQUIRE(handshake(https.port) == id_of(fallback));
    REQUIRE(handshake(https.port, "host.test") == id_of(host_certificate));
}

TEST_CASE("HTTPS servers start listening while others handshake",
          "[ssl][tls][certificate][integration]") {
    http::test::test_ca ca;
    auto manager = std::make_shared<asio::certificate_manager>();
    auto certificate = ca.issue("shared.test");
    set_default(*manager, certificate);
    https_server https(manager);

    // servers on the same default context start listening while the first one handshakes
    std::atomic<bool> stopping{false};
    std::atomic<int> started{0};
    std::thread listeners([&] {
        while (!stopping) {
            http::server other;
            if (!other.listen(http::test::listen_options("127.0.0.1", 0, manager))) ++started;
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

TEST_CASE("HTTPS server keeps applying a certificate manager the application no longer holds",
          "[ssl][tls][certificate][integration]") {
    // Its listener holds the manager until it stops (a context outliving its manager is
    // covered by the certificate_manager unit tests)
    http::test::test_ca ca;
    auto manager = std::make_shared<asio::certificate_manager>();
    auto listener = ca.issue("listener.test");
    auto other = ca.issue("other.test");
    set_default(*manager, listener);
    REQUIRE_FALSE(manager->set_certificate("other.test", other.first, other.second));
    auto https = std::make_unique<https_server>(manager);
    REQUIRE(handshake(https->port, "other.test") == id_of(other));

    std::weak_ptr<asio::certificate_manager> watch = manager;
    manager.reset();
    REQUIRE_FALSE(watch.expired());

    // the host name keeps its certificate, the others get the default one
    for (const std::string name : {"other.test", "", "unknown.test"}) {
        tls_connection connection(https->port, name);
        REQUIRE_FALSE(connection.error);
        REQUIRE(connection.peer() == id_of(name == "other.test" ? other : listener));
        REQUIRE(connection.hello() == "hello");
    }

    https.reset();
    REQUIRE(watch.expired());
}

TEST_CASE("Two certificate managers serve their own certificates for the same host name",
          "[ssl][tls][certificate][integration]") {
    http::test::test_ca ca;
    auto fallback = ca.issue("default.test");
    auto first = ca.issue("host.test");
    auto second = ca.issue("host.test");
    REQUIRE(id_of(first) != id_of(second));
    auto first_manager = std::make_shared<asio::certificate_manager>();
    auto second_manager = std::make_shared<asio::certificate_manager>();
    set_default(*first_manager, fallback);
    set_default(*second_manager, fallback);
    http::test::server_certificate first_host(first_manager, "host.test", first);
    http::test::server_certificate second_host(second_manager, "host.test", second);
    // a pool_server and a server, each with the manager it was given
    https_server pool(first_manager);
    basic_https_server<http::server> single(second_manager);

    for (int i = 0; i < 2; ++i) {
        tls_connection to_pool(pool.port, "host.test");
        REQUIRE_FALSE(to_pool.error);
        REQUIRE(to_pool.peer() == id_of(first));
        REQUIRE(to_pool.hello() == "hello");
        tls_connection to_single(single.port, "host.test");
        REQUIRE_FALSE(to_single.error);
        REQUIRE(to_single.peer() == id_of(second));
        REQUIRE(to_single.hello() == "hello");
    }
    // a certificate replaced in one manager changes nothing in the other
    auto third = ca.issue("host.test");
    REQUIRE_FALSE(first_manager->set_certificate("host.test", third.first, third.second));
    REQUIRE(handshake(pool.port, "host.test") == id_of(third));
    REQUIRE(handshake(single.port, "host.test") == id_of(second));
}

TEST_CASE("HTTPS server keeps listening with the default certificate removed",
          "[ssl][tls][certificate][integration]") {
    thinger::http::test::running_workers workers;
    http::test::test_ca ca;
    auto manager = std::make_shared<asio::certificate_manager>();
    auto certificate = ca.issue("default.test");
    set_default(*manager, certificate);
    https_server https(manager);

    manager->remove_default_certificate();
    REQUIRE(manager->get_default_certificate() == nullptr);
    REQUIRE(handshake(https.port) == id_of(certificate));
    REQUIRE(handshake(https.port, "unknown.test") == id_of(certificate));

    // a server cannot listen with a manager without a default
    http::pool_server other(workers);
    auto options = http::test::listen_options("127.0.0.1", 0, manager);
    options.max_listening_attempts = 1;
    REQUIRE(other.listen(std::move(options)) == boost::asio::error::invalid_argument);
}

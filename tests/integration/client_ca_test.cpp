#include <catch2/catch_test_macros.hpp>
#include <thinger/http_client.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/asio/sockets/websocket.hpp>
#include <openssl/pem.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include "../fixtures/test_ca.hpp"

using namespace thinger;
using namespace std::chrono_literals;

// CAs trusted by the client besides the system ones: a server with a certificate of a private
// CA is only accepted once its CA is added (per client or per request, from a file or PEM text).
// And the certificate the client presents to the servers that ask for one

namespace {

    // A TLS server on localhost, with the routes `routes` adds
    struct tls_server {
        http::server server;
        uint16_t port = 0;
        std::thread thread;

        explicit tls_server(const std::function<void(http::server&)>& routes = {}) {
            server.enable_ssl(true);
            server.get("/hello", [](http::request&, http::response& res) { res.send("hello"); });
            if (routes) routes(server);
            REQUIRE(server.listen("127.0.0.1", 0));
            port = server.local_port();
            thread = std::thread([this] { server.wait(); });
        }

        ~tls_server() {
            server.stop();
            if (thread.joinable()) thread.join();
        }

        std::string url() const { return "https://localhost:" + std::to_string(port) + "/hello"; }
    };

    // An environment variable set while it exists
    struct environment_variable {
        std::string name;
        std::optional<std::string> saved;

        environment_variable(std::string variable, const std::string& value) : name(std::move(variable)) {
            if (const char* previous = std::getenv(name.c_str())) saved = previous;
            ::setenv(name.c_str(), value.c_str(), 1);
        }

        ~environment_variable() {
            if (saved) {
                ::setenv(name.c_str(), saved->c_str(), 1);
            } else {
                ::unsetenv(name.c_str());
            }
        }
    };

    // A file removed with the test
    struct temporary_file {
        std::filesystem::path path;

        temporary_file(const std::string& name, const std::string& content)
            : path(std::filesystem::temp_directory_path() / (name + "-" + std::to_string(::getpid()) + ".pem")) {
            std::ofstream(path) << content;
        }

        ~temporary_file() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    };

}

TEST_CASE("HTTP client trusts the CAs added", "[http][client][tls][ca][integration]") {
    http::test::test_ca ca;
    http::test::server_certificate certificate("localhost", ca.issue("localhost"));
    tls_server server;
    http::client client;
    client.timeout(5s);

    SECTION("A private CA is not trusted by default") {
        auto res = client.get(server.url());
        REQUIRE_FALSE(res);
        // The verification error itself (the handshake is not retried)
        REQUIRE(res.error_code().category() == boost::asio::error::get_ssl_category());
    }

    SECTION("PEM text, per client") {
        client.add_ca_pem(ca.pem());
        auto res = client.get(server.url());
        REQUIRE(res.ok());
        REQUIRE(res.body() == "hello");
    }

    SECTION("A file, per client") {
        temporary_file file("thinger-test-ca", ca.pem());
        client.add_ca_file(file.path.string());
        REQUIRE(client.get(server.url()).ok());
    }

    SECTION("Per request, from PEM text or a file") {
        temporary_file file("thinger-test-ca", ca.pem());
        REQUIRE(client.request(server.url()).ca_pem(ca.pem()).get().ok());
        REQUIRE(client.request(server.url()).ca_file(file.path.string()).get().ok());
        // Not for the requests without them
        REQUIRE_FALSE(client.get(server.url()));
    }

    SECTION("A bundle with several CAs") {
        http::test::test_ca other("Other Test CA");
        client.add_ca_pem(other.pem() + ca.pem());
        REQUIRE(client.get(server.url()).ok());
    }

    SECTION("The certificate is still verified for the host") {
        http::test::server_certificate other_name("localhost", ca.issue("other.example"));
        client.add_ca_pem(ca.pem());
        auto res = client.get(server.url());
        REQUIRE_FALSE(res);
        REQUIRE(res.has_network_error());
    }

    SECTION("A CA that cannot be loaded fails the request") {
        client.add_ca_file("/nonexistent/ca.pem");
        REQUIRE(client.get(server.url()).error_code() == http::client_error::invalid_ca_certificate);

        http::client other;
        other.add_ca_pem("not a certificate");
        REQUIRE(other.get(server.url()).error_code() == http::client_error::invalid_ca_certificate);

        // Plain requests do not need them
        REQUIRE(other.get("http://localhost:1/").error_code() != http::client_error::invalid_ca_certificate);
    }

    SECTION("A CA file changed in place is loaded again") {
        http::test::test_ca other("Other Test CA");
        temporary_file file("thinger-test-ca-changed", other.pem());
        client.add_ca_file(file.path.string());
        REQUIRE_FALSE(client.get(server.url()));

        // (a later modification time, whatever the clock resolution of the file system)
        std::ofstream(file.path) << ca.pem();
        std::filesystem::last_write_time(file.path, std::filesystem::last_write_time(file.path) + 2s);
        REQUIRE(client.get(server.url()).ok());
    }
}

TEST_CASE("HTTP client trusts the system CAs along with those added", "[http][client][tls][ca][integration]") {
    // The system CAs are those OpenSSL loads by default: SSL_CERT_FILE points them to a CA of
    // the test, so this runs offline. Set before any client exists (getenv and setenv are not
    // thread-safe together); each test case runs in a process of its own under ctest.
    http::test::test_ca system_ca("System Test CA");
    http::test::test_ca private_ca("Private Test CA");
    temporary_file system_file("thinger-test-system-ca", system_ca.pem());
    environment_variable cert_file("SSL_CERT_FILE", system_file.path.string());

    {
        tls_server server;
        http::client client;
        client.timeout(5s);

        // A server with a certificate of the "system" CA
        {
            http::test::server_certificate certificate("localhost", system_ca.issue("localhost"));
            REQUIRE(client.get(server.url()).ok());
            client.add_ca_pem(private_ca.pem());
            REQUIRE(client.get(server.url()).ok());
        }

        // And one of the private CA, added
        {
            http::test::server_certificate certificate("localhost", private_ca.issue("localhost"));
            REQUIRE(client.get(server.url()).ok());
        }

        // Without the system CAs, only the added ones
        client.use_system_ca(false);
        {
            http::test::server_certificate certificate("localhost", system_ca.issue("localhost"));
            REQUIRE_FALSE(client.get(server.url()));
        }
        {
            http::test::server_certificate certificate("localhost", private_ca.issue("localhost"));
            REQUIRE(client.get(server.url()).ok());
        }
    }
}

namespace {

    // The certificates the server got from its last client (tls_session::peer_chain_der)
    struct peer_record {
        std::mutex mutex;
        std::optional<std::vector<std::string>> chain;

        void set(const std::optional<asio::tls_session>& tls) {
            std::scoped_lock lock(mutex);
            chain = tls ? tls->peer_chain_der : std::vector<std::string>{};
        }

        std::optional<std::vector<std::string>> get() {
            std::scoped_lock lock(mutex);
            return chain;
        }
    };

    // A private key encrypted with a password (PEM)
    std::string encrypted_key() {
        EVP_PKEY* key = EVP_EC_gen("P-256");
        BIO* bio = BIO_new(BIO_s_mem());
        static const char password[] = "secret";
        PEM_write_bio_PrivateKey(bio, key, EVP_aes_256_cbc(), nullptr, 0, nullptr, const_cast<char*>(password));
        char* data = nullptr;
        long size = BIO_get_mem_data(bio, &data);
        std::string pem(data, static_cast<size_t>(size));
        BIO_free(bio);
        EVP_PKEY_free(key);
        return pem;
    }

}

TEST_CASE("HTTP and WebSocket clients present a client certificate", "[http][client][websocket][tls][mtls][integration]") {
    http::test::test_ca ca;
    auto intermediate = ca.issue_intermediate("Intermediate");
    auto certificate = intermediate->issue_client("client");
    auto leaf = http::test::test_ca::der_of(certificate.first);
    peer_record peer;
    tls_server server([&peer](http::server& routes) {
        routes.get("/peer", [&peer](http::request& req, http::response& res) {
            peer.set(req.get_socket()->tls());
            res.send("peer");
        });
        routes.get("/ws", [&peer](http::request&, http::response& res) {
            res.upgrade_websocket_stream([&peer](std::shared_ptr<asio::websocket> ws, std::string) {
                peer.set(ws->tls());
                co_spawn(ws->get_io_context(), [ws]() -> awaitable<void> {
                    uint8_t buffer[256];
                    for (;;) {
                        auto [ec, bytes] = co_await ws->read_some(buffer, sizeof(buffer));
                        if (ec) co_return;
                        co_await ws->write(buffer, bytes);
                    }
                }, detached);
            });
        });
    });
    // The host name the client sends (SNI) requires a certificate; others do not
    http::test::client_verify_host required("localhost", asio::client_verify::required);
    REQUIRE(required.set);
    auto url = [&server](const std::string& scheme, const std::string& path) {
        return scheme + "://localhost:" + std::to_string(server.port) + path;
    };
    http::client client;
    client.timeout(5s);
    client.verify_ssl(false);

    SECTION("HTTPS: the server gets the certificate configured") {
        REQUIRE_FALSE(client.client_certificate(certificate.first, certificate.second));
        REQUIRE(client.has_client_certificate());
        auto res = client.get(url("https", "/peer"));
        REQUIRE(res.ok());
        auto chain = peer.get();
        REQUIRE(chain);
        REQUIRE(chain->size() == 1);
        REQUIRE((*chain)[0] == leaf);
    }

    SECTION("WSS: the upgrade presents it, and messages are exchanged") {
        REQUIRE_FALSE(client.client_certificate(certificate.first, certificate.second));
        auto ws = client.websocket(url("wss", "/ws"));
        REQUIRE(ws);
        REQUIRE(ws->send_text("hello"));
        REQUIRE(ws->receive().first == "hello");
        auto chain = peer.get();
        REQUIRE(chain);
        REQUIRE((*chain)[0] == leaf);
        ws->close();
    }

    SECTION("Without a certificate the server refuses the client, and the caller gets the error") {
        auto res = client.get(url("https", "/peer"));
        REQUIRE_FALSE(res.ok());
        REQUIRE(res.has_error());
        REQUIRE_FALSE(client.websocket(url("wss", "/ws")));
        REQUIRE_FALSE(peer.get());
    }

    SECTION("The chain sent along, from memory and from files, gives the same") {
        std::vector<std::string> expected{leaf, http::test::test_ca::der_of(intermediate->pem())};
        REQUIRE_FALSE(client.client_certificate(certificate.first, certificate.second, intermediate->pem()));
        REQUIRE(client.get(url("https", "/peer")).ok());
        REQUIRE(peer.get() == expected);

        temporary_file certificate_file("thinger-test-client-certificate", certificate.first);
        temporary_file key_file("thinger-test-client-key", certificate.second);
        temporary_file chain_file("thinger-test-client-chain", intermediate->pem());
        http::client from_files;
        from_files.timeout(5s);
        from_files.verify_ssl(false);
        REQUIRE_FALSE(from_files.client_certificate_file(certificate_file.path.string(), key_file.path.string(),
                                                         chain_file.path.string()));
        REQUIRE(from_files.get(url("https", "/peer")).ok());
        REQUIRE(peer.get() == expected);
    }

    SECTION("A server that asks for no certificate gets none, and the request works") {
        REQUIRE_FALSE(client.client_certificate(certificate.first, certificate.second));
        // no host name (SNI) for an address: the listener asks for none
        auto res = client.get("https://127.0.0.1:" + std::to_string(server.port) + "/peer");
        REQUIRE(res.ok());
        REQUIRE(peer.get() == std::vector<std::string>{});
    }

    SECTION("Replaced, or cleared, for the next connections") {
        auto other = ca.issue_client("other client");
        REQUIRE_FALSE(client.client_certificate(certificate.first, certificate.second));
        REQUIRE(client.get(url("https", "/peer")).ok());
        REQUIRE_FALSE(client.client_certificate(other.first, other.second));
        REQUIRE(client.get(url("https", "/peer")).ok());
        REQUIRE((*peer.get())[0] == http::test::test_ca::der_of(other.first));
        client.clear_client_certificate();
        REQUIRE_FALSE(client.has_client_certificate());
        REQUIRE_FALSE(client.get(url("https", "/peer")).ok());
    }
}

TEST_CASE("Client certificates that cannot be used fail when set", "[http][client][tls][mtls]") {
    http::test::test_ca ca;
    auto certificate = ca.issue_client("client");
    auto other = ca.issue_client("other");
    auto rsa = ca.issue_client("rsa client", http::test::key_type::rsa2048);
    http::client client;

    SECTION("A key of another certificate") {
        REQUIRE(client.client_certificate(certificate.first, other.second) == http::client_error::client_key_mismatch);
        // of another type: an EC key for an RSA certificate
        REQUIRE(client.client_certificate(rsa.first, certificate.second) == http::client_error::client_key_mismatch);
        REQUIRE(client.client_certificate(certificate.first, rsa.second) == http::client_error::client_key_mismatch);
    }

    SECTION("PEM that cannot be read") {
        REQUIRE(client.client_certificate("not a certificate", certificate.second) ==
                http::client_error::invalid_client_certificate);
        REQUIRE(client.client_certificate(certificate.first, certificate.second, "-----BEGIN CERTIFICATE-----\nbroken\n") ==
                http::client_error::invalid_client_certificate);
        REQUIRE(client.client_certificate(certificate.first, "not a key") == http::client_error::invalid_client_key);
    }

    SECTION("An encrypted key fails, without asking for its password") {
        REQUIRE(client.client_certificate(certificate.first, encrypted_key()) == http::client_error::invalid_client_key);
    }

    SECTION("Files that do not exist") {
        temporary_file key_file("thinger-test-client-key", certificate.second);
        REQUIRE(client.client_certificate_file("/nonexistent/client.pem", key_file.path.string()) ==
                http::client_error::invalid_client_certificate);
        temporary_file certificate_file("thinger-test-client-certificate", certificate.first);
        REQUIRE(client.client_certificate_file(certificate_file.path.string(), "/nonexistent/key.pem") ==
                http::client_error::invalid_client_key);
    }

    SECTION("The current certificate is kept") {
        REQUIRE_FALSE(client.has_client_certificate());
        REQUIRE(client.client_certificate("not a certificate", certificate.second));
        REQUIRE_FALSE(client.has_client_certificate());
        REQUIRE_FALSE(client.client_certificate(certificate.first, certificate.second));
        REQUIRE(client.client_certificate(certificate.first, other.second));
        REQUIRE(client.has_client_certificate());
    }
}

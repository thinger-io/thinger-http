#include <catch2/catch_test_macros.hpp>
#include <thinger/asio/listener.hpp>
#include <thinger/asio/sockets/ssl_socket.hpp>
#include <thinger/asio/ssl/certificate_manager.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/write.hpp>
#include <openssl/x509.h>
#include <sys/resource.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "../fixtures/listener_helpers.hpp"
#include "../fixtures/test_ca.hpp"

using namespace thinger;
using namespace std::chrono_literals;
using http::test::client_verify_host;
using http::test::connect_tls;
using http::test::key_type;
using http::test::stalled_client;
using http::test::test_ca;
using http::test::tls_client;
using http::test::tls_client_options;

// Client certificates on TLS listeners (listener_options::client_verify):
// the three modes, deferred and trusted validation, limits, tls() and the accessors of ssl_socket,
// modes per host name (SNI), session resumption, and the deadline of the handshake

namespace {

    // A TLS server on localhost, on an io_context of its own, listening with the default
    // certificate of a certificate_manager (one of its own if none is given). It answers "ok" to
    // each connection once its handshake is done, then echoes what it receives, and keeps what
    // the connection told of TLS
    struct tls_server {
        std::shared_ptr<asio::certificate_manager> certificates;
        boost::asio::io_context io_context;
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{io_context.get_executor()};
        std::unique_ptr<asio::listener> server;
        std::thread thread;
        // What open() returned
        boost::system::error_code opened;

        std::mutex mutex;
        std::vector<std::optional<asio::tls_session>> sessions;
        // The DER of ssl_socket::peer_certificate() (empty if null), and the size of peer_chain()
        // (-1 if null)
        std::vector<std::string> peer_certificates;
        std::vector<int> peer_chain_sizes;
        // The session id context of each session
        std::vector<std::string> session_id_contexts;
        std::atomic<int> handled{0};

        explicit tls_server(const asio::client_verify_options& verify = {},
                            std::chrono::milliseconds handshake_timeout = 60s,
                            bool tls = true)
            : tls_server(std::make_shared<asio::certificate_manager>(), verify, handshake_timeout, tls) {}

        explicit tls_server(const std::shared_ptr<asio::certificate_manager>& manager,
                            const asio::client_verify_options& verify = {},
                            std::chrono::milliseconds handshake_timeout = 60s,
                            bool tls = true)
            : certificates(manager) {
            auto options = tls ? http::test::secure_options(handshake_timeout, manager) : http::test::tcp_options();
            options.client_verify = verify;
            server = std::make_unique<asio::listener>(std::move(options), provider(), provider());
            opened = server->open();
            if (!opened) server->start([this](std::shared_ptr<asio::socket> sock) { serve(std::move(sock)); });
            thread = std::thread([this] { io_context.run(); });
        }

        ~tls_server() {
            server->stop();
            work.reset();
            io_context.stop();
            thread.join();
        }

        tls_server(const tls_server&) = delete;
        tls_server& operator=(const tls_server&) = delete;

        uint16_t port() const { return server->local_port(); }

        std::optional<asio::tls_session> last_session() {
            std::scoped_lock lock(mutex);
            return sessions.empty() ? std::nullopt : sessions.back();
        }

    private:
        asio::executor_provider provider() {
            return [this] { return boost::asio::any_io_executor(io_context.get_executor()); };
        }

        void serve(std::shared_ptr<asio::socket> sock) {
            {
                std::scoped_lock lock(mutex);
                sessions.push_back(sock->tls());
                if (auto ssl = std::dynamic_pointer_cast<asio::ssl_socket>(sock)) {
                    X509* leaf = ssl->peer_certificate();
                    std::string der;
                    if (leaf) {
                        unsigned char* data = nullptr;
                        int size = i2d_X509(leaf, &data);
                        der.assign(reinterpret_cast<const char*>(data), static_cast<size_t>(size));
                        OPENSSL_free(data);
                    }
                    peer_certificates.push_back(der);
                    STACK_OF(X509)* chain = ssl->peer_chain();
                    peer_chain_sizes.push_back(chain ? sk_X509_num(chain) : -1);
                    unsigned int size = 0;
                    const unsigned char* context = SSL_SESSION_get0_id_context(SSL_get_session(ssl->native_handle()), &size);
                    session_id_contexts.emplace_back(reinterpret_cast<const char*>(context), size);
                }
            }
            ++handled;
            co_spawn(sock->get_io_context(), [sock]() -> awaitable<void> {
                auto [ec, bytes] = co_await sock->write(std::string_view("ok"));
                uint8_t buffer[256];
                while (!ec) {
                    std::tie(ec, bytes) = co_await sock->read_some(buffer, sizeof(buffer));
                    if (!ec) std::tie(ec, bytes) = co_await sock->write(buffer, bytes);
                }
                sock->close();
            }, detached);
        }
    };

    tls_client_options with_certificate(const std::pair<std::string, std::string>& certificate, int version = 0,
                                        const std::string& chain = "") {
        tls_client_options options;
        options.certificate = certificate.first + chain;
        options.key = certificate.second;
        options.version = version;
        return options;
    }

    asio::client_verify_options verify(asio::client_verify mode, std::string trusted_cas = "") {
        asio::client_verify_options options;
        options.mode = mode;
        options.trusted_cas = std::move(trusted_cas);
        return options;
    }

    // The start of a TLS ClientHello: a handshake record header and a few bytes of it
    const std::string partial_client_hello{"\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03", 11};

    // Bytes of the Certificate message of a client sending `chain` (DER certificates)
    size_t certificate_message_size(const std::vector<std::string>& chain, bool tls13) {
        // TLS 1.3 adds the request context (1 byte) and the extensions of each entry (2 bytes)
        size_t size = 3 + (tls13 ? 1 : 0);
        for (const auto& der : chain) size += 3 + der.size() + (tls13 ? 2 : 0);
        return size;
    }

}

TEST_CASE("Client certificates: modes of a listener", "[tls][mtls][integration]") {
    test_ca ca;
    auto certificate = ca.issue_client("device-1");
    auto leaf = test_ca::der_of(certificate.first);

    SECTION("none: no certificate is asked for") {
        tls_server server(verify(asio::client_verify::none));
        REQUIRE(connect_tls(server.port(), with_certificate(certificate)).accepted);
        auto session = server.last_session();
        REQUIRE(session);
        REQUIRE(session->peer_chain_der.empty());
    }

    SECTION("optional: with or without a certificate") {
        tls_server server(verify(asio::client_verify::optional));
        REQUIRE(connect_tls(server.port()).accepted);
        REQUIRE(server.last_session()->peer_chain_der.empty());
        REQUIRE(connect_tls(server.port(), with_certificate(certificate)).accepted);
        auto session = server.last_session();
        REQUIRE(session->peer_chain_der.size() == 1);
        REQUIRE(session->peer_chain_der[0] == leaf);
    }

    for (int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
        DYNAMIC_SECTION("required: refused without a certificate, version " << std::hex << version) {
            tls_server server(verify(asio::client_verify::required));
            tls_client_options without;
            without.version = version;
            REQUIRE_FALSE(connect_tls(server.port(), without).accepted);
            REQUIRE(connect_tls(server.port(), with_certificate(certificate, version)).accepted);
            REQUIRE(server.handled == 1);
            REQUIRE(server.last_session()->peer_chain_der[0] == leaf);
        }
    }
}

TEST_CASE("Client certificates: deferred and trusted validation", "[tls][mtls][integration]") {
    test_ca ca_a("CA A");
    test_ca ca_b("CA B");
    auto intermediate = ca_b.issue_intermediate("Intermediate B");
    auto from_a = ca_a.issue_client("device-a");
    auto from_b = intermediate->issue_client("device-b");

    SECTION("Deferred: a certificate of an unknown CA is accepted, with the chain it sent") {
        for (auto mode : {asio::client_verify::optional, asio::client_verify::required}) {
            tls_server server(verify(mode));
            REQUIRE(connect_tls(server.port(), with_certificate(from_b, 0, intermediate->pem())).accepted);
            auto session = server.last_session();
            REQUIRE(session->peer_chain_der.size() == 2);
            REQUIRE(session->peer_chain_der[0] == test_ca::der_of(from_b.first));
            REQUIRE(session->peer_chain_der[1] == test_ca::der_of(intermediate->pem()));
        }
    }

    SECTION("Deferred: a client without the key of its certificate is refused") {
        for (int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
            tls_server server(verify(asio::client_verify::optional));
            auto options = with_certificate(from_b, version);
            // The certificate it sends has another key than the one it signs with
            options.tamper = [](SSL_CTX* context) {
                X509* certificate = SSL_CTX_get0_certificate(context);
                EVP_PKEY* other = EVP_EC_gen("P-256");
                X509_set_pubkey(certificate, other);
                X509_sign(certificate, other, EVP_sha256());
                EVP_PKEY_free(other);
            };
            REQUIRE_FALSE(connect_tls(server.port(), options).accepted);
            REQUIRE(server.handled == 0);
        }
    }

    SECTION("Trusted CAs: a certificate of another CA is refused, also when optional") {
        for (auto mode : {asio::client_verify::optional, asio::client_verify::required}) {
            tls_server server(verify(mode, ca_a.pem()));
            REQUIRE_FALSE(server.opened);
            REQUIRE_FALSE(connect_tls(server.port(), with_certificate(from_b, 0, intermediate->pem())).accepted);
            REQUIRE(server.handled == 0);
        }
    }

    SECTION("Trusted CAs: a certificate of a trusted CA is accepted, and its validation reported") {
        tls_server server(verify(asio::client_verify::required, ca_a.pem() + ca_b.pem()));
        REQUIRE(connect_tls(server.port(), with_certificate(from_a)).accepted);
        REQUIRE(server.last_session()->verify_result == X509_V_OK);
        REQUIRE(connect_tls(server.port(), with_certificate(from_b, 0, intermediate->pem())).accepted);
        REQUIRE(server.last_session()->verify_result == X509_V_OK);
    }

    SECTION("Trusted CAs that cannot be read: the listener does not open") {
        for (std::string pem : {std::string("not a certificate"), ca_a.pem().substr(0, 100)}) {
            tls_server server(verify(asio::client_verify::optional, pem));
            REQUIRE(server.opened == boost::system::errc::invalid_argument);
            // nothing bound
            REQUIRE(server.port() == 0);
        }
    }
}

TEST_CASE("Client certificates: chain depth and size", "[tls][mtls][integration]") {
    test_ca root("Root");
    auto intermediate = root.issue_intermediate("Intermediate");
    auto second = intermediate->issue_intermediate("Second intermediate");
    auto leaf = intermediate->issue_client("device", key_type::rsa3072);
    auto deep_leaf = second->issue_client("deep device");
    std::string chain = intermediate->pem();
    std::string deep_chain = second->pem() + intermediate->pem();

    SECTION("max_depth: intermediate CAs allowed below the trusted CA, as OpenSSL counts them") {
        auto options = verify(asio::client_verify::required, root.pem());
        {
            tls_server server(options);
            REQUIRE(connect_tls(server.port(), with_certificate(leaf, 0, chain)).accepted);
            REQUIRE(connect_tls(server.port(), with_certificate(deep_leaf, 0, deep_chain)).accepted);
        }
        options.max_depth = 1;
        {
            tls_server server(options);
            REQUIRE(connect_tls(server.port(), with_certificate(leaf, 0, chain)).accepted);
            REQUIRE_FALSE(connect_tls(server.port(), with_certificate(deep_leaf, 0, deep_chain)).accepted);
        }
        options.max_depth = 0;
        {
            tls_server server(options);
            REQUIRE_FALSE(connect_tls(server.port(), with_certificate(leaf, 0, chain)).accepted);
        }
    }

    SECTION("max_cert_list: a larger chain fails the handshake") {
        std::string large_chain = deep_chain + root.pem();
        REQUIRE(certificate_message_size({test_ca::der_of(leaf.first), test_ca::der_of(second->pem()),
                                          test_ca::der_of(intermediate->pem()), test_ca::der_of(root.pem())},
                                         false) > 1024);
        auto options = verify(asio::client_verify::optional);
        options.max_cert_list = 1024;
        tls_server server(options);
        REQUIRE_FALSE(connect_tls(server.port(), with_certificate(leaf, 0, large_chain)).accepted);
        REQUIRE(server.handled == 0);
    }

    for (int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
        DYNAMIC_SECTION("max_cert_list: a chain of exactly the limit passes, one byte more fails, version "
                        << std::hex << version) {
            std::vector<std::string> ders{test_ca::der_of(leaf.first), test_ca::der_of(chain)};
            auto size = certificate_message_size(ders, version == TLS1_3_VERSION);
            auto options = verify(asio::client_verify::optional);
            options.max_cert_list = size;
            {
                tls_server server(options);
                REQUIRE(connect_tls(server.port(), with_certificate(leaf, version, chain)).accepted);
                REQUIRE(server.last_session()->peer_chain_der == ders);
            }
            options.max_cert_list = size - 1;
            {
                tls_server server(options);
                REQUIRE_FALSE(connect_tls(server.port(), with_certificate(leaf, version, chain)).accepted);
            }
        }
    }
}

TEST_CASE("Client certificates: tls() and the accessors of a connection", "[tls][mtls][integration]") {
    test_ca ca;
    auto intermediate = ca.issue_intermediate("Intermediate");

    SECTION("A plain TCP connection has no TLS details") {
        tls_server server({}, 60s, false);
        boost::asio::io_context io_context;
        boost::asio::ip::tcp::socket sock(io_context);
        sock.connect({boost::asio::ip::make_address("127.0.0.1"), server.port()});
        char reply[2];
        boost::asio::read(sock, boost::asio::buffer(reply));
        REQUIRE(server.handled == 1);
        std::scoped_lock lock(server.mutex);
        REQUIRE_FALSE(server.sessions.back().has_value());
    }

    SECTION("Version, cipher, server name and resumption of TLS 1.3") {
        tls_server server(verify(asio::client_verify::optional));
        tls_client_options options;
        options.version = TLS1_3_VERSION;
        options.server_name = "client.example.com";
        REQUIRE(connect_tls(server.port(), options).accepted);
        auto session = server.last_session();
        REQUIRE(session);
        REQUIRE(session->version == "TLSv1.3");
        REQUIRE_FALSE(session->cipher.empty());
        REQUIRE(session->server_name == "client.example.com");
        REQUIRE_FALSE(session->resumed);
        REQUIRE(session->peer_chain_der.empty());

        options.server_name.clear();
        REQUIRE(connect_tls(server.port(), options).accepted);
        REQUIRE(server.last_session()->server_name.empty());
    }

    SECTION("peer_certificate() and peer_chain(), without copies") {
        tls_server server(verify(asio::client_verify::optional));
        auto certificate = intermediate->issue_client("device");
        REQUIRE(connect_tls(server.port(), with_certificate(certificate, 0, intermediate->pem())).accepted);
        REQUIRE(connect_tls(server.port()).accepted);
        std::scoped_lock lock(server.mutex);
        REQUIRE(server.peer_certificates[0] == test_ca::der_of(certificate.first));
        // the server side chain has no leaf
        REQUIRE(server.peer_chain_sizes[0] == 1);
        REQUIRE(server.peer_certificates[1].empty());
        REQUIRE(server.peer_chain_sizes[1] <= 0);
    }

    for (int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
        for (auto type : {key_type::ec_p256, key_type::rsa2048, key_type::rsa3072}) {
            DYNAMIC_SECTION("Keys of the clients: version " << std::hex << version << ", type " << static_cast<int>(type)) {
                tls_server server(verify(asio::client_verify::optional));
                auto certificate = ca.issue_client("device", type);
                REQUIRE(connect_tls(server.port(), with_certificate(certificate, version)).accepted);
                auto session = server.last_session();
                REQUIRE(session->version == (version == TLS1_3_VERSION ? "TLSv1.3" : "TLSv1.2"));
                REQUIRE(session->peer_chain_der == std::vector<std::string>{test_ca::der_of(certificate.first)});
            }
        }
    }
}

TEST_CASE("Client certificates: modes per host name", "[tls][mtls][sni][integration]") {
    test_ca ca;
    auto certificate = ca.issue_client("device");
    auto manager = std::make_shared<asio::certificate_manager>();

    SECTION("A host that requires a certificate on a listener that asks for none") {
        tls_server server(manager);
        client_verify_host devices_host(manager, "devices.example.com", asio::client_verify::required);
        REQUIRE(devices_host.set);
        tls_client_options devices;
        devices.server_name = "devices.example.com";
        REQUIRE_FALSE(connect_tls(server.port(), devices).accepted);
        tls_client_options console;
        console.server_name = "console.example.com";
        REQUIRE(connect_tls(server.port(), console).accepted);
        REQUIRE(connect_tls(server.port()).accepted);
        auto with = with_certificate(certificate);
        with.server_name = "devices.example.com";
        REQUIRE(connect_tls(server.port(), with).accepted);
        REQUIRE(server.last_session()->peer_chain_der.size() == 1);
        // the name is compared as the certificates, case-insensitive
        devices.server_name = "DEVICES.Example.com";
        REQUIRE_FALSE(connect_tls(server.port(), devices).accepted);
    }

    SECTION("Wildcards, invalid names, and removal") {
        tls_server server(manager);
        client_verify_host wildcard(manager, "*.devices.example.com", asio::client_verify::optional);
        REQUIRE(wildcard.set);
        REQUIRE(manager->get_client_verify("a.devices.example.com") == asio::client_verify::optional);
        REQUIRE_FALSE(manager->get_client_verify("devices.example.com"));
        auto with = with_certificate(certificate);
        with.server_name = "a.devices.example.com";
        REQUIRE(connect_tls(server.port(), with).accepted);
        REQUIRE(server.last_session()->peer_chain_der.size() == 1);
        tls_client_options without;
        without.server_name = "a.devices.example.com";
        REQUIRE(connect_tls(server.port(), without).accepted);

        for (std::string name : {"", "bad host", "*", "*.", "a*.example.com", "a..example.com", "example.com."}) {
            REQUIRE(manager->set_client_verify(name, asio::client_verify::required) == boost::system::errc::invalid_argument);
        }

        client_verify_host removed(manager, "removed.example.com", asio::client_verify::required);
        REQUIRE(removed.set);
        REQUIRE(manager->remove_client_verify("removed.example.com"));
        REQUIRE_FALSE(manager->remove_client_verify("removed.example.com"));
        REQUIRE_FALSE(manager->remove_client_verify("never.example.com"));
        without.server_name = "removed.example.com";
        REQUIRE(connect_tls(server.port(), without).accepted);
    }

    SECTION("A host with a certificate of its own keeps the trusted CAs of the listener") {
        test_ca other("Other CA");
        http::test::server_certificate own(manager, "own.example.com", ca.issue("own.example.com"));
        tls_server server(manager, verify(asio::client_verify::optional, ca.pem()));
        client_verify_host own_host(manager, "own.example.com", asio::client_verify::required);
        REQUIRE(own_host.set);
        auto trusted = with_certificate(certificate);
        trusted.server_name = "own.example.com";
        REQUIRE(connect_tls(server.port(), trusted).accepted);
        REQUIRE(server.last_session()->verify_result == X509_V_OK);
        auto untrusted = with_certificate(other.issue_client("device"));
        untrusted.server_name = "own.example.com";
        REQUIRE_FALSE(connect_tls(server.port(), untrusted).accepted);
        tls_client_options without;
        without.server_name = "own.example.com";
        REQUIRE_FALSE(connect_tls(server.port(), without).accepted);
    }
}

namespace {

    // Certificate requests a client of with_requests_counted() got from its servers
    std::atomic<int> certificate_requests{0};

    // A client asking for `name` that has no certificate, and counts the requests of one
    tls_client_options with_requests_counted(const std::string& name) {
        tls_client_options options;
        options.server_name = name;
        options.tamper = [](SSL_CTX* context) {
            SSL_CTX_set_client_cert_cb(context, [](SSL*, X509**, EVP_PKEY**) {
                ++certificate_requests;
                return 0;
            });
        };
        return options;
    }

}

TEST_CASE("Client certificates: modes per host name of two managers", "[tls][mtls][sni][integration]") {
    auto with_mode = std::make_shared<asio::certificate_manager>();
    auto without_mode = std::make_shared<asio::certificate_manager>();
    tls_server first(with_mode);
    tls_server second(without_mode);
    client_verify_host devices(with_mode, "devices.test", asio::client_verify::required);
    REQUIRE(devices.set);
    REQUIRE_FALSE(without_mode->get_client_verify("devices.test"));

    // only the server of the manager with the mode asks for a certificate
    certificate_requests = 0;
    REQUIRE_FALSE(connect_tls(first.port(), with_requests_counted("devices.test")).accepted);
    REQUIRE(certificate_requests == 1);
    certificate_requests = 0;
    REQUIRE(connect_tls(second.port(), with_requests_counted("devices.test")).accepted);
    REQUIRE(certificate_requests == 0);
}

TEST_CASE("Client certificates: session id contexts", "[tls][mtls][resumption][integration]") {
    test_ca ca;
    auto manager = std::make_shared<asio::certificate_manager>();
    // a context given ready-made, for a host
    auto given = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
    auto host_certificate = ca.issue("given.example.com");
    REQUIRE(asio::use_certificate(*given, host_certificate.first, host_certificate.second) == asio::certificate_error::none);
    REQUIRE_FALSE(manager->set_certificate("given.example.com", given));

    SECTION("Every context of the manager, created or given, has the same one") {
        tls_server server(manager);
        REQUIRE(connect_tls(server.port()).accepted);
        tls_client_options options;
        options.server_name = "given.example.com";
        REQUIRE(connect_tls(server.port(), options).accepted);
        std::scoped_lock lock(server.mutex);
        REQUIRE(server.session_id_contexts[0] == "thinger-http");
        REQUIRE(server.session_id_contexts[1] == "thinger-http");
    }

    SECTION("A listener asking for certificates has one of its own, also once switched by SNI") {
        tls_server optional(manager, verify(asio::client_verify::optional));
        tls_server trusted(manager, verify(asio::client_verify::optional, ca.pem()));
        tls_client_options options;
        options.server_name = "given.example.com";
        REQUIRE(connect_tls(optional.port(), options).accepted);
        REQUIRE(connect_tls(trusted.port(), options).accepted);
        std::scoped_lock lock(optional.mutex, trusted.mutex);
        REQUIRE(optional.session_id_contexts[0].size() == SSL_MAX_SID_CTX_LENGTH);
        REQUIRE(trusted.session_id_contexts[0].size() == SSL_MAX_SID_CTX_LENGTH);
        REQUIRE(optional.session_id_contexts[0] != trusted.session_id_contexts[0]);
    }
}

TEST_CASE("Client certificates: session resumption", "[tls][mtls][resumption][integration]") {
    test_ca ca;
    auto certificate = ca.issue_client("device");
    auto leaf = test_ca::der_of(certificate.first);
    auto manager = std::make_shared<asio::certificate_manager>();
    http::test::server_certificate own(manager, "resume.example.com", ca.issue("resume.example.com"));

    // Resumed with tickets (the server's cache drops the sessions closed without close_notify)
    struct variant {
        const char* name;
        int version;
    };
    for (auto [name, version] : {variant{"TLS 1.2", TLS1_2_VERSION}, variant{"TLS 1.3", TLS1_3_VERSION}}) {
        for (std::string host : {"", "resume.example.com"}) {
            DYNAMIC_SECTION("Resumed with a client certificate: " << name << ", host '" << host << "'") {
                tls_server server(manager, verify(asio::client_verify::optional));
                auto options = with_certificate(certificate, version);
                options.server_name = host;
                auto first = connect_tls(server.port(), options);
                REQUIRE(first.accepted);
                REQUIRE_FALSE(first.resumed);
                options.session = first.session;
                auto second = connect_tls(server.port(), options);
                REQUIRE(second.accepted);
                REQUIRE(second.resumed);
                auto session = server.last_session();
                REQUIRE(session->resumed);
                REQUIRE_FALSE(session->peer_chain_der.empty());
                REQUIRE(session->peer_chain_der[0] == leaf);
            }
        }

        DYNAMIC_SECTION("A session without a certificate is not resumed where one is required: " << name) {
            tls_server server(manager);
            client_verify_host devices(manager, "devices.example.com", asio::client_verify::required);
            REQUIRE(devices.set);
            tls_client_options options;
            options.version = version;
            options.server_name = "console.example.com";
            auto first = connect_tls(server.port(), options);
            REQUIRE(first.accepted);
            options.session = first.session;
            options.server_name = "devices.example.com";
            REQUIRE_FALSE(connect_tls(server.port(), options).accepted);
            // where it was created it is resumed
            options.server_name = "console.example.com";
            REQUIRE(connect_tls(server.port(), options).resumed);
        }

        DYNAMIC_SECTION("A session of a listener with deferred validation is not resumed on one with trusted CAs: " << name) {
            test_ca other("Other CA");
            auto untrusted = with_certificate(other.issue_client("device"), version);
            tls_server deferred(manager, verify(asio::client_verify::optional));
            tls_server trusted(manager, verify(asio::client_verify::optional, ca.pem()));
            auto first = connect_tls(deferred.port(), untrusted);
            REQUIRE(first.accepted);
            untrusted.session = first.session;
            REQUIRE_FALSE(connect_tls(trusted.port(), untrusted).accepted);
            REQUIRE(trusted.handled == 0);
        }
    }
}

TEST_CASE("TLS handshake deadline", "[tls][handshake-timeout][integration]") {
    SECTION("60 s by default") {
        REQUIRE(asio::listener_options{}.handshake_timeout == 60s);
    }

    for (std::string sent : {std::string(), partial_client_hello}) {
        DYNAMIC_SECTION("A client that does not complete its handshake is closed in time, sending "
                        << sent.size() << " bytes") {
            tls_server server({}, 500ms);
            stalled_client client(server.port(), sent);
            auto start = std::chrono::steady_clock::now();
            REQUIRE(client.closed_within(3s));
            auto elapsed = std::chrono::steady_clock::now() - start;
            REQUIRE(elapsed >= 400ms);
            REQUIRE(elapsed <= 1500ms);
            REQUIRE(server.handled == 0);
        }
    }

    SECTION("A client that completed its handshake is not closed by the deadline") {
        tls_server server({}, 500ms);
        tls_client client;
        REQUIRE(client.connect(server.port(), {}));
        std::this_thread::sleep_for(1s);
        REQUIRE(client.write("ping"));
        REQUIRE(client.read(4) == "ping");
    }

    SECTION("0 disables it") {
        tls_server server({}, 0ms);
        stalled_client client(server.port(), partial_client_hello);
        REQUIRE_FALSE(client.closed_within(1500ms));
    }

    SECTION("Stopping the server closes the handshakes pending, which never reach the handler") {
        std::vector<std::unique_ptr<stalled_client>> clients;
        {
            tls_server server;
            for (int i = 0; i < 10; ++i) clients.push_back(std::make_unique<stalled_client>(server.port(), partial_client_hello));
            // accepted by the server before it stops
            std::this_thread::sleep_for(200ms);
            server.server->stop();
            for (const auto& client : clients) REQUIRE(client->closed_within(2s));
            REQUIRE(server.handled == 0);
        }
    }

#ifdef __linux__
    SECTION("1,000 stalled clients are all closed, and their descriptors released") {
        rlimit limit{};
        ::getrlimit(RLIMIT_NOFILE, &limit);
        if (limit.rlim_cur < 4096 && limit.rlim_max >= 4096) {
            limit.rlim_cur = 4096;
            ::setrlimit(RLIMIT_NOFILE, &limit);
        }
        auto descriptors = [] {
            return std::distance(std::filesystem::directory_iterator("/proc/self/fd"), std::filesystem::directory_iterator{});
        };
        tls_server server({}, 500ms);
        // after the server set up its io_context, timers and acceptor
        std::this_thread::sleep_for(100ms);
        auto before = descriptors();
        std::vector<std::unique_ptr<stalled_client>> clients;
        for (int i = 0; i < 1000; ++i) clients.push_back(std::make_unique<stalled_client>(server.port()));
        for (const auto& client : clients) REQUIRE(client->closed_within(3s));
        // only the descriptors of the clients are left
        auto deadline = std::chrono::steady_clock::now() + 2s;
        while (descriptors() != before + 1000 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
        }
        REQUIRE(descriptors() == before + 1000);
        clients.clear();
        REQUIRE(descriptors() == before);
        REQUIRE(server.handled == 0);
    }
#endif
}

#include <catch2/catch_test_macros.hpp>

#include <thinger/asio/listener.hpp>
#include <thinger/asio/detail/listener_access.hpp>
#include <thinger/asio/sockets/ssl_socket.hpp>
#include <thinger/asio/ssl/certificate_manager.hpp>

#include <boost/asio.hpp>
#include <openssl/ssl.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../fixtures/listener_helpers.hpp"
#include "../fixtures/test_ca.hpp"

using namespace thinger;
using namespace thinger::asio;
using namespace std::chrono_literals;
using http::test::client_hello;
using http::test::client_verify_host;
using http::test::connect_tls;
using http::test::delivered;
using http::test::io_thread;
using http::test::log_capture;
using http::test::secure_options;
using http::test::stalled_client;
using http::test::tcp_options;
using http::test::test_ca;
using http::test::tls_client;
using http::test::tls_client_options;
using http::test::tls_handshake;
using tcp = boost::asio::ip::tcp;
using outcome = stalled_client::outcome;

// The TLS of the listener: the bound of the handshakes in progress per IO thread, the client
// certificate mode of a host name over the listener's, and the session cache

namespace {

// TLS handshakes bounded per IO thread, with a deadline
listener_options bounded_options(size_t bound, std::chrono::milliseconds handshake_timeout = 5s) {
    auto options = secure_options(handshake_timeout);
    options.max_pending_handshakes = bound;
    return options;
}

// A connection whose handshake holds a slot: its ClientHello was answered
std::unique_ptr<stalled_client> holder(uint16_t port, std::chrono::milliseconds timeout = 2s) {
    auto client = std::make_unique<stalled_client>(port, client_hello());
    REQUIRE(client->connected());
    REQUIRE(client->wait(timeout) == outcome::data);
    return client;
}

// A connection that sent its ClientHello, still unanswered after `silence`
std::unique_ptr<stalled_client> waiter(uint16_t port, std::chrono::milliseconds silence = 300ms) {
    auto client = std::make_unique<stalled_client>(port, client_hello());
    REQUIRE(client->connected());
    REQUIRE(client->wait(silence) == outcome::silent);
    return client;
}

long milliseconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

TEST_CASE("listener holds a third handshake until a slot frees", "[listener][tls][handshake-bound][integration]") {
    for (bool own_thread : {false, true}) {
        DYNAMIC_SECTION((own_thread ? "acceptor thread" : "executor providers")) {
            io_thread io;
            listener server(bounded_options(2), io.provider(), io.provider());
            if (own_thread) thinger::asio::detail::listener_access::use_acceptor_thread(server);
            REQUIRE_FALSE(server.open());
            auto port = server.local_port();
            delivered sockets;
            server.start(sockets.handler());
            auto first = holder(port);
            auto second = holder(port);

            SECTION("A ClientHello is not answered while two handshakes are in progress") {
                auto third = waiter(port);
                // Its handshake starts when one of them is closed
                first->close();
                REQUIRE(third->wait(2s) == outcome::data);
                // and holds the slot it got
                auto fourth = waiter(port);
                second->close();
                REQUIRE(fourth->wait(2s) == outcome::data);
                REQUIRE(sockets.size() == 0);
            }
            SECTION("A TLS client completes once a slot frees, and reaches the handler") {
                auto started = std::chrono::steady_clock::now();
                auto third = std::async(std::launch::async, [port] { return tls_handshake(port, 5s); });
                REQUIRE(third.wait_for(300ms) == std::future_status::timeout);
                REQUIRE(sockets.size() == 0);
                second->reset();
                REQUIRE(third.get());
                REQUIRE(sockets.wait(1));
                // Well within its deadline
                INFO("handed over after " << milliseconds_since(started) << " ms");
                REQUIRE(milliseconds_since(started) < 2000);
                REQUIRE(sockets.sockets.front()->is_secure());
            }
            server.stop();
        }
    }
}

TEST_CASE("listener closes a handshake waiting for a slot at its deadline", "[listener][tls][handshake-bound][handshake-timeout][integration]") {
    // The wait counts against the deadline, from the accept: if it were added, the connection
    // that waits 250 ms would be closed at 750 ms
    for (auto waited : {0ms, 250ms}) {
        DYNAMIC_SECTION("connected " << waited.count() << " ms after the handshakes in progress") {
            io_thread io;
            listener server(bounded_options(2, 500ms), io.provider(), io.provider());
            REQUIRE_FALSE(server.open());
            auto port = server.local_port();
            delivered sockets;
            server.start(sockets.handler());
            auto first = holder(port, 200ms);
            auto second = holder(port, 200ms);
            std::this_thread::sleep_for(waited);

            stalled_client third(port, client_hello());
            REQUIRE(third.connected());
            // Unanswered while the two hold their slots (to their own deadline)
            if (waited == 0ms) REQUIRE(third.wait(300ms) == outcome::silent);
            REQUIRE(third.closed_within(2s));
            auto elapsed = milliseconds_since(third.connected_at);
            INFO("closed after " << elapsed << " ms");
            REQUIRE(elapsed >= 350);
            REQUIRE(elapsed <= 650);
            REQUIRE(sockets.size() == 0);
            server.stop();
        }
    }
}

TEST_CASE("listener goes on accepting while handshakes wait for a slot", "[listener][tls][handshake-bound][integration]") {
    log_capture logs;
    io_thread io;
    listener server(bounded_options(2), io.provider(), io.provider());
    REQUIRE_FALSE(server.open());
    auto port = server.local_port();
    delivered sockets;
    server.start(sockets.handler());
    auto first = holder(port);
    auto second = holder(port);
    auto third = waiter(port, 100ms);
    auto fourth = waiter(port, 100ms);
    // All four accepted, not left in the backlog
    REQUIRE(logs.logged("debug", "received connection from: ip: 127.0.0.1", 2s, 4));

    // A plain listener on the same IO thread is not bounded
    listener plain(tcp_options(), io.provider(), io.provider());
    REQUIRE_FALSE(plain.open());
    delivered plain_sockets;
    plain.start(plain_sockets.handler());
    boost::asio::io_context client_context;
    std::vector<tcp::socket> clients;
    for (int i = 0; i < 3; ++i) {
        clients.emplace_back(client_context);
        clients.back().connect({boost::asio::ip::make_address("127.0.0.1"), plain.local_port()});
    }
    REQUIRE(plain_sockets.wait(3));
    REQUIRE(third->wait(100ms) == outcome::silent);
    REQUIRE(fourth->wait(0ms) == outcome::silent);
    REQUIRE(sockets.size() == 0);
}

TEST_CASE("listener without a bound makes no handshake wait", "[listener][tls][handshake-bound][integration]") {
    io_thread io;
    auto options = bounded_options(0);
    REQUIRE(listener_options{}.max_pending_handshakes == 0);
    listener server(options, io.provider(), io.provider());
    REQUIRE_FALSE(server.open());
    auto port = server.local_port();
    delivered sockets;
    server.start(sockets.handler());
    std::vector<std::unique_ptr<stalled_client>> stalled;
    for (int i = 0; i < 8; ++i) stalled.push_back(holder(port));
    REQUIRE(tls_handshake(port, 2s));
    REQUIRE(sockets.wait(1));
}

TEST_CASE("listener bounds the handshakes per IO thread", "[listener][tls][handshake-bound][integration]") {
    io_thread first;
    io_thread second;
    std::atomic<int> next{0};
    executor_provider serve_on = [&]() -> boost::asio::any_io_executor {
        return (next++ % 2 == 0) ? first.io_context.get_executor() : second.io_context.get_executor();
    };
    listener server(bounded_options(2), first.provider(), serve_on);
    REQUIRE_FALSE(server.open());
    auto port = server.local_port();
    delivered sockets;
    server.start(sockets.handler());
    // Served on each thread in turn: two in progress on each
    std::vector<std::unique_ptr<stalled_client>> in_progress;
    for (int i = 0; i < 4; ++i) in_progress.push_back(holder(port));
    // The next one is on a thread at its bound
    auto fifth = waiter(port);
    REQUIRE(sockets.size() == 0);
}

TEST_CASE("listener stop() closes a handshake waiting for a slot, and leaks none", "[listener][tls][handshake-bound][stop][integration]") {
    io_thread io;
    {
        listener server(bounded_options(2), io.provider(), io.provider());
        REQUIRE_FALSE(server.open());
        auto port = server.local_port();
        delivered sockets;
        server.start(sockets.handler());
        auto first = holder(port);
        auto second = holder(port);
        auto third = waiter(port);

        server.stop();
        REQUIRE(third->closed_within(1s));
        REQUIRE(first->closed_within(1s));
        REQUIRE(second->closed_within(1s));
        REQUIRE(sockets.size() == 0);
    }
    // A new listener on the same IO thread gets its two slots
    listener server(bounded_options(2), io.provider(), io.provider());
    REQUIRE_FALSE(server.open());
    auto port = server.local_port();
    delivered sockets;
    server.start(sockets.handler());
    auto first = holder(port);
    auto second = holder(port);
    auto third = waiter(port);
    second->close();
    REQUIRE(third->wait(2s) == outcome::data);
}

TEST_CASE("listener frees the slots of handshakes timed out or reset", "[listener][tls][handshake-bound][integration]") {
    io_thread io;
    listener server(bounded_options(2, 1s), io.provider(), io.provider());
    REQUIRE_FALSE(server.open());
    auto port = server.local_port();
    delivered sockets;
    server.start(sockets.handler());

    // Freed by the deadline
    {
        auto first = holder(port);
        auto second = holder(port);
        REQUIRE(first->closed_within(2s));
        REQUIRE(second->closed_within(2s));
    }
    // The bound filled again: answered at once, a third one waits
    auto first = holder(port, 250ms);
    auto second = holder(port, 250ms);
    auto third = waiter(port, 200ms);
    // Freed by resets: the waiter gets one of them
    first->reset();
    second->reset();
    REQUIRE(third->wait(500ms) == outcome::data);
    third->close();
    // And filled a third time
    auto fourth = holder(port, 250ms);
    auto fifth = holder(port, 250ms);
    auto sixth = waiter(port, 200ms);
    REQUIRE(sockets.size() == 0);
}

namespace {

// A TLS listener on localhost, on an io_context of its own, that answers "ok" to each connection
// handed over, then echoes what it receives, and keeps what the connection told of TLS
struct tls_server {
    io_thread io;
    std::unique_ptr<listener> server;
    boost::system::error_code opened;
    std::mutex mutex;
    std::vector<std::optional<tls_session>> sessions;
    std::atomic<int> handled{0};

    explicit tls_server(listener_options options)
        : server(std::make_unique<listener>(std::move(options), io.provider(), io.provider())) {
        opened = server->open();
        if (!opened) server->start([this](std::shared_ptr<thinger::asio::socket> sock) { serve(std::move(sock)); });
    }

    ~tls_server() { server->stop(); }

    tls_server(const tls_server&) = delete;
    tls_server& operator=(const tls_server&) = delete;

    uint16_t port() const { return server->local_port(); }

    std::optional<tls_session> last_session() {
        std::scoped_lock lock(mutex);
        return sessions.empty() ? std::nullopt : sessions.back();
    }

private:
    void serve(std::shared_ptr<thinger::asio::socket> sock) {
        {
            std::scoped_lock lock(mutex);
            sessions.push_back(sock->tls());
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

listener_options verifying(std::shared_ptr<certificate_manager> manager, client_verify mode) {
    auto options = secure_options(60s, std::move(manager));
    options.client_verify.mode = mode;
    return options;
}

const char* name(client_verify mode) {
    switch (mode) {
        case client_verify::none: return "none";
        case client_verify::optional: return "optional";
        case client_verify::required: return "required";
    }
    return "?";
}

tls_client_options for_host(const std::string& host, const std::pair<std::string, std::string>* certificate = nullptr) {
    tls_client_options options;
    options.server_name = host;
    if (certificate) {
        options.certificate = certificate->first;
        options.key = certificate->second;
    }
    return options;
}

} // namespace

TEST_CASE("listener client certificates: the mode of a host over the listener's", "[listener][tls][mtls][sni][integration]") {
    // Listener mode × host mode (unset, none, optional, required), one listener per row, with
    // and without a client certificate: the host's mode applies when set, the listener's
    // otherwise. Without a certificate only `required` fails; with one, it is asked for (and
    // seen by the server) unless the mode is `none`
    test_ca ca;
    auto certificate = ca.issue_client("device");
    const std::string host = "devices.example.com";
    const std::vector<std::optional<client_verify>> host_modes{
        std::nullopt, client_verify::none, client_verify::optional, client_verify::required};

    for (auto listener_mode : {client_verify::none, client_verify::optional, client_verify::required}) {
        auto manager = std::make_shared<certificate_manager>();
        tls_server server(verifying(manager, listener_mode));
        REQUIRE_FALSE(server.opened);
        for (const auto& host_mode : host_modes) {
            std::unique_ptr<client_verify_host> mode_of_host;
            if (host_mode) {
                mode_of_host = std::make_unique<client_verify_host>(manager, host, *host_mode);
                REQUIRE(mode_of_host->set);
            }
            auto effective = host_mode.value_or(listener_mode);
            INFO("listener " << name(listener_mode) << ", host " << (host_mode ? name(*host_mode) : "unset"));

            CHECK(connect_tls(server.port(), for_host(host)).accepted == (effective != client_verify::required));
            REQUIRE(connect_tls(server.port(), for_host(host, &certificate)).accepted);
            CHECK(server.last_session()->peer_chain_der.size() == (effective == client_verify::none ? 0u : 1u));
        }
    }
}

TEST_CASE("listener client certificates: the listener's mode for other names", "[listener][tls][mtls][sni][integration]") {
    auto manager = std::make_shared<certificate_manager>();

    SECTION("A listener asking for none, a host requiring one") {
        tls_server server(verifying(manager, client_verify::none));
        client_verify_host devices(manager, "devices.example.com", client_verify::required);
        REQUIRE(devices.set);
        REQUIRE(connect_tls(server.port()).accepted);
        REQUIRE(connect_tls(server.port(), for_host("console.example.com")).accepted);
        REQUIRE_FALSE(connect_tls(server.port(), for_host("devices.example.com")).accepted);
    }
    SECTION("A listener requiring one, a host asking for none") {
        tls_server server(verifying(manager, client_verify::required));
        client_verify_host open_host(manager, "open.example.com", client_verify::none);
        REQUIRE(open_host.set);
        REQUIRE(connect_tls(server.port(), for_host("open.example.com")).accepted);
        REQUIRE_FALSE(connect_tls(server.port(), for_host("console.example.com")).accepted);
        REQUIRE_FALSE(connect_tls(server.port()).accepted);
    }
    SECTION("Two listeners of one manager keep their own modes") {
        tls_server open(verifying(manager, client_verify::none));
        tls_server closed(verifying(manager, client_verify::required));
        REQUIRE(connect_tls(open.port()).accepted);
        REQUIRE_FALSE(connect_tls(closed.port()).accepted);
        REQUIRE(connect_tls(open.port()).accepted);
        REQUIRE_FALSE(connect_tls(closed.port(), for_host("console.example.com")).accepted);
        REQUIRE(open.handled == 2);
        REQUIRE(closed.handled == 0);
    }
    SECTION("Two listeners of two managers keep their own modes, and the hosts of their own manager") {
        test_ca ca;
        auto certificate = ca.issue_client("device");
        auto other = std::make_shared<certificate_manager>();
        tls_server asking(verifying(manager, client_verify::optional));
        tls_server requiring(verifying(other, client_verify::required));
        REQUIRE_FALSE(asking.opened);
        REQUIRE_FALSE(requiring.opened);
        // A host mode of one manager is not seen by the listener of the other
        client_verify_host open_host(other, "open.example.com", client_verify::none);
        REQUIRE(open_host.set);
        REQUIRE_FALSE(manager->get_client_verify("open.example.com"));

        REQUIRE(connect_tls(asking.port()).accepted);
        REQUIRE(connect_tls(asking.port(), for_host("open.example.com", &certificate)).accepted);
        REQUIRE(asking.last_session()->peer_chain_der.size() == 1u);
        REQUIRE_FALSE(connect_tls(requiring.port()).accepted);
        REQUIRE(connect_tls(requiring.port(), for_host("open.example.com")).accepted);
        REQUIRE(connect_tls(requiring.port(), for_host("console.example.com", &certificate)).accepted);
        REQUIRE(requiring.last_session()->peer_chain_der.size() == 1u);
        REQUIRE(asking.handled == 2);
        REQUIRE(requiring.handled == 2);
    }
}

namespace {

// How a client resumes: TLS 1.2 by ticket or by session id (no tickets), TLS 1.3 by ticket
struct resumption {
    const char* name;
    int version;
    bool tickets;
};

const std::vector<resumption> resumptions{
    {"TLS 1.2, ticket", TLS1_2_VERSION, true},
    {"TLS 1.2, session id", TLS1_2_VERSION, false},
    {"TLS 1.3, ticket", TLS1_3_VERSION, true},
};

tls_client_options resuming(const resumption& how) {
    tls_client_options options;
    options.version = how.version;
    if (!how.tickets) options.tamper = [](SSL_CTX* context) { SSL_CTX_set_options(context, SSL_OP_NO_TICKET); };
    return options;
}

// Whether a second client resumes the session of a first one, connected meanwhile (the server
// keeps a session by id while its connection is open); `ticket` tells whether the first got one
struct resumed_result {
    bool resumed = false;
    bool ticket = false;
};

resumed_result resume(uint16_t port, const resumption& how) {
    auto options = resuming(how);
    tls_client first;
    REQUIRE(first.connect(port, options));
    REQUIRE_FALSE(first.resumed());
    options.session = first.session();
    tls_client second;
    REQUIRE(second.connect(port, options));
    return {second.resumed(), SSL_SESSION_has_ticket(options.session.get()) == 1};
}

listener_options with_cache(session_cache_options cache, std::shared_ptr<certificate_manager> manager = nullptr,
                            uint16_t port = 0) {
    auto options = secure_options(60s, std::move(manager));
    options.session_cache = cache;
    std::get<tcp_endpoint>(options.endpoint).port = port;
    return options;
}

uint16_t free_port() {
    boost::asio::io_context io_context;
    tcp::acceptor acceptor(io_context, {boost::asio::ip::make_address("127.0.0.1"), 0});
    return acceptor.local_endpoint().port();
}

bool can_bind(uint16_t port) {
    boost::asio::io_context io_context;
    tcp::acceptor acceptor(io_context);
    boost::system::error_code ec;
    acceptor.open(tcp::v4(), ec);
    if (!ec) acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
    if (!ec) acceptor.bind({boost::asio::ip::make_address("127.0.0.1"), port}, ec);
    if (!ec) acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
    return !ec;
}

} // namespace

TEST_CASE("listener resumes TLS sessions by default", "[listener][tls][resumption][integration]") {
    REQUIRE(listener_options{}.session_cache == session_cache_options{true, 20480});
    for (const auto& how : resumptions) {
        DYNAMIC_SECTION(how.name) {
            tls_server server(secure_options());
            REQUIRE_FALSE(server.opened);
            auto result = resume(server.port(), how);
            REQUIRE(result.resumed);
            REQUIRE(result.ticket == how.tickets);
            REQUIRE(server.last_session()->resumed);
        }
    }
}

TEST_CASE("listener without a session cache never resumes", "[listener][tls][resumption][integration]") {
    for (const auto& how : resumptions) {
        DYNAMIC_SECTION(how.name) {
            tls_server server(with_cache({false, 20480}));
            REQUIRE_FALSE(server.opened);
            for (int round = 0; round < 2; ++round) {
                auto result = resume(server.port(), how);
                REQUIRE_FALSE(result.resumed);
                // No tickets issued either
                REQUIRE_FALSE(result.ticket);
            }
            std::scoped_lock lock(server.mutex);
            REQUIRE(server.sessions.size() == 4);
            for (const auto& session : server.sessions) REQUIRE_FALSE(session->resumed);
        }
    }
}

TEST_CASE("listener refuses a session cache that differs from its manager's", "[listener][tls][resumption][integration]") {
    auto manager = std::make_shared<certificate_manager>();
    tls_server first(with_cache({}, manager));
    REQUIRE_FALSE(first.opened);

    for (auto cache : {session_cache_options{false, 20480}, session_cache_options{true, 1000}}) {
        DYNAMIC_SECTION("enabled " << cache.enabled << ", size " << cache.size) {
            auto port = free_port();
            io_thread io;
            listener second(with_cache(cache, manager, port), io.provider(), io.provider());
            REQUIRE(second.open() == boost::system::errc::invalid_argument);
            // Nothing bound
            REQUIRE(second.local_port() == 0);
            REQUIRE(second.url().empty());
            REQUIRE(can_bind(port));
            // The first one resumes as before
            REQUIRE(resume(first.port(), resumptions.front()).resumed);
        }
    }

    SECTION("An equal one is accepted") {
        tls_server second(with_cache({}, manager));
        REQUIRE_FALSE(second.opened);
        REQUIRE(resume(second.port(), resumptions.back()).resumed);
    }
}

TEST_CASE("listener holds its certificate manager until stop()", "[listener][tls][integration]") {
    auto manager = std::make_shared<certificate_manager>();
    std::weak_ptr<certificate_manager> watch = manager;
    io_thread io;
    listener secure(secure_options(60s, std::move(manager)), io.provider(), io.provider());
    REQUIRE_FALSE(watch.expired());

    SECTION("Opened, it holds it while its connections handshake") {
        delivered connections;
        REQUIRE_FALSE(secure.open());
        REQUIRE_FALSE(watch.expired());
        REQUIRE(secure.url().starts_with("https://"));
        secure.start(connections.handler());
        REQUIRE(tls_handshake(secure.local_port()));
        REQUIRE(connections.wait(1));
        REQUIRE_FALSE(watch.expired());
        secure.stop();
        REQUIRE(watch.expired());
        // Stopped again (and by the destructor): nothing left to release
        secure.stop();
    }
    SECTION("Never opened, it releases it when stopped") {
        secure.stop();
        REQUIRE(watch.expired());
    }
}

namespace {

// The DER of the certificate the server presented to a client it accepted; empty otherwise
std::string presented(uint16_t port, const tls_client_options& options) {
    tls_client client;
    if (!client.connect(port, options)) return {};
    X509* certificate = SSL_get1_peer_certificate(client.stream->native_handle());
    if (!certificate) return {};
    unsigned char* data = nullptr;
    int size = i2d_X509(certificate, &data);
    std::string der(reinterpret_cast<const char*>(data), static_cast<size_t>(size > 0 ? size : 0));
    OPENSSL_free(data);
    X509_free(certificate);
    return der;
}

// A manager with a default certificate and one for `host`, whose clients must present a
// certificate, and the options of a listener asking for none, trusting `ca`: the options hold
// the only reference to the manager, the test watches it
struct unkept_manager {
    std::pair<std::string, std::string> default_certificate;
    std::pair<std::string, std::string> host_certificate;
    listener_options options;
    std::weak_ptr<certificate_manager> watch;

    unkept_manager(const test_ca& ca, const std::string& host)
        : default_certificate(ca.issue("default.example.com")), host_certificate(ca.issue(host)) {
        auto manager = std::make_shared<certificate_manager>();
        REQUIRE_FALSE(manager->set_default_certificate(default_certificate.first, default_certificate.second));
        REQUIRE_FALSE(manager->set_certificate(host, host_certificate.first, host_certificate.second));
        REQUIRE_FALSE(manager->set_client_verify(host, client_verify::required));
        watch = manager;
        options = verifying(std::move(manager), client_verify::none);
        options.client_verify.trusted_cas = ca.pem();
    }
};

} // namespace

TEST_CASE("listener applies the hosts of a manager the application keeps no reference to", "[listener][tls][mtls][sni][integration]") {
    // R1: the host certificate and the host's client certificate mode keep applying after
    // open() returns, until stop()
    test_ca ca;
    auto certificate = ca.issue_client("device");
    const std::string host = "devices.example.com";
    unkept_manager certificates(ca, host);
    auto default_der = test_ca::der_of(certificates.default_certificate.first);
    auto host_der = test_ca::der_of(certificates.host_certificate.first);

    auto server = std::make_unique<tls_server>(std::move(certificates.options));
    REQUIRE_FALSE(server->opened);
    CHECK_FALSE(certificates.watch.expired());

    SECTION("The host requires a client certificate") {
        REQUIRE_FALSE(connect_tls(server->port(), for_host(host)).accepted);
        REQUIRE(server->handled == 0);
        REQUIRE(presented(server->port(), for_host(host, &certificate)) == host_der);
        REQUIRE(server->handled == 1);
        REQUIRE(server->last_session()->peer_chain_der.size() == 1u);
    }
    SECTION("Other names get the default certificate and the listener's mode") {
        REQUIRE(presented(server->port(), {}) == default_der);
        REQUIRE(presented(server->port(), for_host("console.example.com")) == default_der);
        REQUIRE(server->handled == 2);
        REQUIRE(server->last_session()->peer_chain_der.empty());
    }
    SECTION("Two listeners of that manager: it lives until the last one stops") {
        auto second_options = verifying(certificates.watch.lock(), client_verify::none);
        second_options.client_verify.trusted_cas = ca.pem();
        auto second = std::make_unique<tls_server>(std::move(second_options));
        REQUIRE_FALSE(second->opened);
        server.reset();
        REQUIRE_FALSE(certificates.watch.expired());
        REQUIRE_FALSE(connect_tls(second->port(), for_host(host)).accepted);
        REQUIRE(presented(second->port(), for_host(host, &certificate)) == host_der);
        second.reset();
        REQUIRE(certificates.watch.expired());
    }

    server.reset();
    REQUIRE(certificates.watch.expired());
}

TEST_CASE("listener opened again after failing to bind serves TLS", "[listener][tls][integration]") {
    // A failed open() leaves the listener neither opened nor stopped: the next one must open
    // with the manager of the options, not as a plain listener under an https URL
    io_thread io;
    auto port = free_port();
    auto holder = std::make_unique<listener>(tcp_options(port), io.provider(), io.provider());
    REQUIRE_FALSE(holder->open());

    auto manager = std::make_shared<certificate_manager>();
    std::weak_ptr<certificate_manager> watch = manager;
    auto options = secure_options(60s, std::move(manager));
    std::get<tcp_endpoint>(options.endpoint).port = port;
    REQUIRE(options.max_listening_attempts == 1);
    listener secure(std::move(options), io.provider(), io.provider());
    REQUIRE(secure.open());
    CHECK_FALSE(watch.expired());

    holder.reset();
    REQUIRE_FALSE(secure.open());
    REQUIRE(secure.url().starts_with("https://"));
    REQUIRE(secure.local_port() == port);
    delivered connections;
    secure.start(connections.handler());
    REQUIRE(tls_handshake(secure.local_port()));
    REQUIRE(connections.wait(1));
    REQUIRE(connections.sockets.front()->is_secure());
    secure.stop();
    REQUIRE(watch.expired());
}

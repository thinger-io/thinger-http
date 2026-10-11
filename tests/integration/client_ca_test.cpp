#include <catch2/catch_test_macros.hpp>
#include <thinger/http_client.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include "../fixtures/test_ca.hpp"

using namespace thinger;
using namespace std::chrono_literals;

// CAs trusted by the client besides the system ones: a server with a certificate of a private
// CA is only accepted once its CA is added (per client or per request, from a file or PEM text)

namespace {

    // A TLS server on localhost
    struct tls_server {
        http::server server;
        uint16_t port = 0;
        std::thread thread;

        tls_server() {
            server.enable_ssl(true);
            server.get("/hello", [](http::request&, http::response& res) { res.send("hello"); });
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

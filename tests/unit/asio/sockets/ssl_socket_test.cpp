#include <catch2/catch_test_macros.hpp>
#include <thinger/asio/sockets/ssl_socket.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

using namespace thinger::asio;

// Helper to create an SSL context
static std::shared_ptr<boost::asio::ssl::context> create_ssl_context() {
    auto ctx = std::make_shared<boost::asio::ssl::context>(
        boost::asio::ssl::context::sslv23_client);
    ctx->set_default_verify_paths();
    ctx->set_verify_mode(boost::asio::ssl::verify_none);
    return ctx;
}

TEST_CASE("SSL Socket Construction", "[ssl_socket][unit]") {
    boost::asio::io_context io_context;
    auto ssl_context = create_ssl_context();

    SECTION("construct with io_context and ssl_context") {
        ssl_socket sock("test_context", io_context, ssl_context);

        REQUIRE_FALSE(sock.is_open());
        REQUIRE(sock.is_secure() == true);
        REQUIRE(sock.requires_handshake() == true);
    }

    SECTION("multiple ssl sockets can be created") {
        ssl_socket sock1("ctx1", io_context, ssl_context);
        ssl_socket sock2("ctx2", io_context, ssl_context);

        REQUIRE_FALSE(sock1.is_open());
        REQUIRE_FALSE(sock2.is_open());
        REQUIRE(sock1.is_secure());
        REQUIRE(sock2.is_secure());
    }
}

TEST_CASE("SSL Socket Properties", "[ssl_socket][unit]") {
    boost::asio::io_context io_context;
    auto ssl_context = create_ssl_context();
    ssl_socket sock("test", io_context, ssl_context);

    SECTION("is_secure returns true") {
        REQUIRE(sock.is_secure() == true);
    }

    SECTION("requires_handshake returns true") {
        REQUIRE(sock.requires_handshake() == true);
    }

    SECTION("is_open returns false before connect") {
        REQUIRE(sock.is_open() == false);
    }

    SECTION("available returns 0 when not connected") {
        REQUIRE(sock.available() == 0);
    }

    SECTION("get_io_context returns the io_context") {
        REQUIRE(&sock.get_io_context() == &io_context);
    }
}

TEST_CASE("SSL Socket vs TCP Socket Properties", "[ssl_socket][tcp_socket][unit]") {
    boost::asio::io_context io_context;
    auto ssl_context = create_ssl_context();

    tcp_socket tcp_sock("tcp", io_context);
    ssl_socket ssl_sock("ssl", io_context, ssl_context);

    SECTION("is_secure differs between TCP and SSL") {
        REQUIRE(tcp_sock.is_secure() == false);
        REQUIRE(ssl_sock.is_secure() == true);
    }

    SECTION("requires_handshake differs between TCP and SSL") {
        REQUIRE(tcp_sock.requires_handshake() == false);
        REQUIRE(ssl_sock.requires_handshake() == true);
    }

    SECTION("both are not open initially") {
        REQUIRE(tcp_sock.is_open() == false);
        REQUIRE(ssl_sock.is_open() == false);
    }
}

TEST_CASE("SSL Socket Close", "[ssl_socket][unit]") {
    boost::asio::io_context io_context;
    auto ssl_context = create_ssl_context();
    ssl_socket sock("test", io_context, ssl_context);

    SECTION("close on non-connected socket doesn't throw") {
        REQUIRE_NOTHROW(sock.close());
        REQUIRE_FALSE(sock.is_open());
    }

    SECTION("multiple close calls are safe") {
        REQUIRE_NOTHROW(sock.close());
        REQUIRE_NOTHROW(sock.close());
        REQUIRE_NOTHROW(sock.close());
    }
}

TEST_CASE("SSL Context Variations", "[ssl_socket][unit]") {
    boost::asio::io_context io_context;

    SECTION("client context (sslv23_client)") {
        auto ctx = std::make_shared<boost::asio::ssl::context>(
            boost::asio::ssl::context::sslv23_client);
        ssl_socket sock("client", io_context, ctx);
        REQUIRE(sock.is_secure());
    }

    SECTION("server context (sslv23_server)") {
        auto ctx = std::make_shared<boost::asio::ssl::context>(
            boost::asio::ssl::context::sslv23_server);
        ssl_socket sock("server", io_context, ctx);
        REQUIRE(sock.is_secure());
    }

    SECTION("TLS 1.2 context") {
        auto ctx = std::make_shared<boost::asio::ssl::context>(
            boost::asio::ssl::context::tlsv12_client);
        ssl_socket sock("tls12", io_context, ctx);
        REQUIRE(sock.is_secure());
    }

    SECTION("TLS 1.3 context") {
        auto ctx = std::make_shared<boost::asio::ssl::context>(
            boost::asio::ssl::context::tlsv13_client);
        ssl_socket sock("tls13", io_context, ctx);
        REQUIRE(sock.is_secure());
    }
}

TEST_CASE("SSL Socket Inherits TCP Socket Functionality", "[ssl_socket][unit]") {
    boost::asio::io_context io_context;
    auto ssl_context = create_ssl_context();
    ssl_socket sock("test", io_context, ssl_context);

    SECTION("can access underlying TCP socket") {
        auto& underlying = sock.get_socket();
        REQUIRE_FALSE(underlying.is_open());
    }

    SECTION("cancel throws on non-connected socket") {
        // cancel() throws on a closed socket (Bad file descriptor)
        REQUIRE_THROWS_AS(sock.cancel(), boost::system::system_error);
    }
}

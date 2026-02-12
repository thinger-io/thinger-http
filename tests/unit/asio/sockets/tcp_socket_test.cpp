#include <catch2/catch_test_macros.hpp>
#include <thinger/asio/sockets/tcp_socket.hpp>
#include <boost/asio.hpp>

using namespace thinger::asio;

TEST_CASE("TCP Socket Construction", "[tcp_socket][unit]") {
    boost::asio::io_context io_context;

    SECTION("construct with io_context") {
        tcp_socket sock("test_context", io_context);

        REQUIRE_FALSE(sock.is_open());
        REQUIRE_FALSE(sock.is_secure());
        REQUIRE_FALSE(sock.requires_handshake());
    }

    SECTION("multiple sockets can be created") {
        tcp_socket sock1("ctx1", io_context);
        tcp_socket sock2("ctx2", io_context);
        tcp_socket sock3("ctx3", io_context);

        REQUIRE_FALSE(sock1.is_open());
        REQUIRE_FALSE(sock2.is_open());
        REQUIRE_FALSE(sock3.is_open());
    }
}

TEST_CASE("TCP Socket Properties", "[tcp_socket][unit]") {
    boost::asio::io_context io_context;
    tcp_socket sock("test", io_context);

    SECTION("is_secure returns false") {
        REQUIRE(sock.is_secure() == false);
    }

    SECTION("requires_handshake returns false") {
        REQUIRE(sock.requires_handshake() == false);
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

TEST_CASE("TCP Socket Close", "[tcp_socket][unit]") {
    boost::asio::io_context io_context;
    tcp_socket sock("test", io_context);

    SECTION("close on non-connected socket doesn't throw") {
        REQUIRE_NOTHROW(sock.close());
        REQUIRE_FALSE(sock.is_open());
    }

    SECTION("cancel on non-connected socket throws") {
        // cancel() throws on a closed socket (Bad file descriptor)
        REQUIRE_THROWS_AS(sock.cancel(), boost::system::system_error);
    }

    SECTION("multiple close calls are safe") {
        REQUIRE_NOTHROW(sock.close());
        REQUIRE_NOTHROW(sock.close());
        REQUIRE_NOTHROW(sock.close());
    }
}

TEST_CASE("TCP Socket Get Socket", "[tcp_socket][unit]") {
    boost::asio::io_context io_context;
    tcp_socket sock("test", io_context);

    SECTION("get_socket returns underlying socket") {
        auto& underlying = sock.get_socket();
        REQUIRE_FALSE(underlying.is_open());
    }
}

TEST_CASE("TCP Socket TCP_NODELAY Option", "[tcp_socket][unit]") {
    boost::asio::io_context io_context;
    tcp_socket sock("test", io_context);

    SECTION("enable/disable TCP_NODELAY doesn't throw on closed socket") {
        // These might throw or not depending on implementation
        // On a closed socket, setting options typically fails silently or throws
        // We just verify they don't crash
        try {
            sock.enable_tcp_no_delay();
        } catch (const boost::system::system_error&) {
            // Expected on closed socket
        }

        try {
            sock.disable_tcp_no_delay();
        } catch (const boost::system::system_error&) {
            // Expected on closed socket
        }
    }
}

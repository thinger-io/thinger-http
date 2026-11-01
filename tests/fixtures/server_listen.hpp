#ifndef THINGER_HTTP_TEST_SERVER_LISTEN_HPP
#define THINGER_HTTP_TEST_SERVER_LISTEN_HPP

#include <thinger/http/server/server_base.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

// Helpers for the tests that listen with a server: the options of a listener, and the port it
// listens on
namespace thinger::http::test {

    // Options to listen on `host` and `port` (0 for one the system chooses), over TLS with the
    // certificates of `tls` if not null
    inline asio::listener_options listen_options(std::string host, uint16_t port,
                                                 std::shared_ptr<asio::certificate_manager> tls = nullptr) {
        asio::listener_options options;
        options.endpoint = asio::tcp_endpoint{std::move(host), port};
        options.tls = std::move(tls);
        return options;
    }

    // Options to listen on a Unix domain socket at `path`
    inline asio::listener_options unix_listen_options(std::string path) {
        asio::listener_options options;
        options.endpoint = asio::unix_endpoint{std::move(path)};
        return options;
    }

    // The TCP port of the last listener `server` opened (the one the system chose for a port 0),
    // read from server_base::listening_on(): "http://127.0.0.1:41234" gives 41234. Fails the
    // test if it does not listen, or its last listener is a Unix socket
    inline uint16_t listening_port(const server_base& server) {
        auto urls = server.listening_on();
        REQUIRE_FALSE(urls.empty());
        const auto& url = urls.back();
        INFO("listening on " << url);
        REQUIRE(url.rfind("unix:", 0) != 0);
        auto colon = url.rfind(':');
        REQUIRE(colon != std::string::npos);
        return static_cast<uint16_t>(std::stoul(url.substr(colon + 1)));
    }

} // namespace thinger::http::test

#endif // THINGER_HTTP_TEST_SERVER_LISTEN_HPP

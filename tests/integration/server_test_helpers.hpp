#ifndef THINGER_TESTS_SERVER_TEST_HELPERS_HPP
#define THINGER_TESTS_SERVER_TEST_HELPERS_HPP

#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <boost/asio.hpp>
#include <boost/asio/use_future.hpp>
#include <array>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

// Helpers for server tests: a server running on its own thread, raw TCP exchanges that
// never block longer than a timeout, and in-memory dispatch
namespace server_test {

    using namespace std::chrono_literals;

    // Runs `server` (routes registered beforehand) on an ephemeral port until destroyed
    struct running_server {
        thinger::http::server& server;
        uint16_t port = 0;
        std::thread thread;

        explicit running_server(thinger::http::server& s) : server(s) {
            REQUIRE(server.listen("127.0.0.1", 0));
            port = server.local_port();
            thread = std::thread([this]() { server.wait(); });
        }

        ~running_server() {
            server.stop();
            if (thread.joinable()) thread.join();
        }
    };

    struct exchange_result {
        std::string data;
        bool closed = false;    // the server closed the connection
    };

    // Read from `sock` until the peer closes it or the timeout expires
    inline exchange_result read_until_closed(boost::asio::io_context& ioc, boost::asio::ip::tcp::socket& sock,
                                             std::chrono::milliseconds timeout) {
        exchange_result result;
        std::array<char, 4096> buffer{};
        std::function<void()> read_more = [&]() {
            sock.async_read_some(boost::asio::buffer(buffer), [&](boost::system::error_code error, size_t bytes) {
                result.data.append(buffer.data(), bytes);
                if (error) {
                    result.closed = true;
                    return;
                }
                read_more();
            });
        };
        read_more();
        ioc.run_for(timeout);
        return result;
    }

    // Send `request` on a new connection and read until the server closes it or the
    // timeout expires
    inline exchange_result raw_exchange(uint16_t port, const std::string& request,
                                        std::chrono::milliseconds timeout = 3s) {
        boost::asio::io_context ioc;
        boost::asio::ip::tcp::socket sock(ioc);
        sock.connect({boost::asio::ip::make_address("127.0.0.1"), port});
        boost::system::error_code ec;
        boost::asio::write(sock, boost::asio::buffer(request), ec);
        return read_until_closed(ioc, sock, timeout);
    }

    inline size_t count(const std::string& data, const std::string& needle) {
        size_t n = 0;
        for (auto pos = data.find(needle); pos != std::string::npos; pos = data.find(needle, pos + needle.size())) n++;
        return n;
    }

    inline std::shared_ptr<thinger::http::http_response> run_dispatch(thinger::http::server& server,
                                                                      std::shared_ptr<thinger::http::http_request> request,
                                                                      thinger::http::dispatch_options options = {}) {
        boost::asio::io_context ioc;
        auto result = co_spawn(ioc, server.dispatch(std::move(request), std::move(options)), boost::asio::use_future);
        ioc.run();
        return result.get();
    }

    inline std::shared_ptr<thinger::http::http_request> make_request(thinger::http::method method, const std::string& path,
                                                                     const std::string& body = "") {
        auto request = thinger::http::http_request::create_http_request(method, "http://localhost" + path);
        if (!body.empty()) request->set_content(body, "application/json");
        return request;
    }

}

#endif

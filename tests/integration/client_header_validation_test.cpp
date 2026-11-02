#include <catch2/catch_test_macros.hpp>
#include <thinger/http_client.hpp>
#include <boost/asio.hpp>
#include "../fixtures/running_workers.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// A client request with a header whose name is not a token, or whose value holds CR, LF, NUL
// or another control character, fails with invalid_argument before any connection is opened
// or reused: nothing reaches the network. Checked with a listener that counts the connections
// it accepts and the bytes it receives.

using namespace thinger;
using namespace std::chrono_literals;

namespace {

    using boost::asio::ip::tcp;

    // Accepts connections on 127.0.0.1, records every byte received, and answers each request
    // head with an empty 200 (requests without a body only), keeping the connection open
    struct counting_listener {
        boost::asio::io_context ioc;
        tcp::acceptor acceptor{ioc, tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), 0}};
        std::atomic<int> accepts{0};
        std::mutex mutex;
        std::string received_;
        std::thread thread;

        counting_listener() {
            co_spawn(ioc, accept_loop(), boost::asio::detached);
            thread = std::thread([this] { ioc.run(); });
        }

        ~counting_listener() {
            ioc.stop();
            if (thread.joinable()) thread.join();
        }

        std::string url(const std::string& path = "/") const {
            return "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + path;
        }

        std::string ws_url() const {
            return "ws://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/ws";
        }

        std::string received() {
            std::lock_guard lock(mutex);
            return received_;
        }

        size_t bytes() {
            std::lock_guard lock(mutex);
            return received_.size();
        }

        boost::asio::awaitable<void> accept_loop() {
            for (;;) {
                auto [ec, socket] = co_await acceptor.async_accept(boost::asio::as_tuple(boost::asio::use_awaitable));
                if (ec) co_return;
                ++accepts;
                co_spawn(ioc, serve(std::move(socket)), boost::asio::detached);
            }
        }

        boost::asio::awaitable<void> serve(tcp::socket socket) {
            static const std::string ok = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
            std::array<char, 4096> buffer{};
            std::string pending;
            for (;;) {
                auto [ec, bytes] = co_await socket.async_read_some(boost::asio::buffer(buffer),
                                                                   boost::asio::as_tuple(boost::asio::use_awaitable));
                if (ec) co_return;
                {
                    std::lock_guard lock(mutex);
                    received_.append(buffer.data(), bytes);
                }
                pending.append(buffer.data(), bytes);
                for (auto end = pending.find("\r\n\r\n"); end != std::string::npos; end = pending.find("\r\n\r\n")) {
                    pending.erase(0, end + 4);
                    auto [write_ec, written] = co_await boost::asio::async_write(
                        socket, boost::asio::buffer(ok), boost::asio::as_tuple(boost::asio::use_awaitable));
                    if (write_ec) co_return;
                }
            }
        }
    };

    const std::string bad_value = "v\r\nX-Injected: 1";

    void require_refused(const http::client_response& response) {
        REQUIRE(response.has_error());
        REQUIRE(response.error_code() == boost::asio::error::invalid_argument);
    }

    std::shared_ptr<http::http_request> request_to(const std::string& url) {
        auto request = std::make_shared<http::http_request>();
        REQUIRE(request->set_url(url));
        request->set_method(http::method::GET);
        return request;
    }

} // namespace

TEST_CASE("A client request with an invalid header fails unsent", "[client][headers][integration]") {
    counting_listener listener;
    http::client client;
    client.timeout(5s);

    SECTION("On a new client, no connection is opened") {
        require_refused(client.get(listener.url(), {{"X-A", bad_value}}));
        REQUIRE(listener.accepts == 0);
        REQUIRE(listener.bytes() == 0);
    }

    SECTION("A pooled connection is not used, and still serves the next request") {
        REQUIRE(client.get(listener.url("/first")).ok());
        REQUIRE(listener.accepts == 1);
        const auto sent = listener.bytes();
        REQUIRE(sent > 0);

        // get, with a headers_map
        require_refused(client.get(listener.url(), {{"X-A", bad_value}}));

        // request_builder::header and ::headers
        require_refused(client.request(listener.url()).header("X-A", bad_value).get());
        require_refused(client.request(listener.url()).headers({{"X-Ok", "1"}, {"X-A", bad_value}}).get());
        require_refused(client.request(listener.url()).header("X-A", bad_value).body("x", "text/plain").post());

        // The headers_map of every verb
        require_refused(client.post(listener.url(), "body", "text/plain", {{"X-A", bad_value}}));
        require_refused(client.put(listener.url(), "body", "text/plain", {{"X-A", bad_value}}));
        require_refused(client.patch(listener.url(), "body", "text/plain", {{"X-A", bad_value}}));
        require_refused(client.del(listener.url(), {{"X-A", bad_value}}));
        require_refused(client.head(listener.url(), {{"X-A", bad_value}}));
        require_refused(client.options(listener.url(), {{"X-A", bad_value}}));

        // An http_request built by hand: add_header, and set_header, also on a header it has
        auto added = request_to(listener.url());
        added->add_header("X-A", "v\nX-Injected: 1");
        require_refused(client.send(added));
        auto set = request_to(listener.url());
        set->set_header("X-A", "ok");
        set->set_header("X-A", std::string("v\0w", 3));
        REQUIRE(set->get_header("X-A") == "ok");
        require_refused(client.send(set));

        // Invalid names
        require_refused(client.get(listener.url(), {{"X A", "v"}}));
        require_refused(client.get(listener.url(), {{"X\r\nY", "v"}}));
        require_refused(client.request(listener.url()).header("X:A", "v").get());

        REQUIRE(listener.accepts == 1);
        REQUIRE(listener.bytes() == sent);

        // The pooled connection carries the next valid request
        REQUIRE(client.get(listener.url("/last")).ok());
        REQUIRE(listener.accepts == 1);
        REQUIRE(listener.received().find("GET /last ") != std::string::npos);
    }

    SECTION("A streamed request fails, and its callback is never called") {
        bool called = false;
        auto callback = [&called](const http::stream_info&) {
            called = true;
            return true;
        };
        auto request = request_to(listener.url());
        request->add_header("X-A", bad_value);
        auto result = client.send_streaming(request, callback);
        REQUIRE_FALSE(result.error.empty());
        REQUIRE_FALSE(result.ok());

        auto built = client.request(listener.url()).header("X\r\nY", "v").get(callback);
        REQUIRE_FALSE(built.error.empty());

        REQUIRE_FALSE(called);
        REQUIRE(listener.accepts == 0);
        REQUIRE(listener.bytes() == 0);

        // A valid one is sent
        auto valid = client.send_streaming(request_to(listener.url("/streamed")), callback);
        REQUIRE(valid.error.empty());
        REQUIRE(valid.status_code == 200);
        REQUIRE(listener.received().find("GET /streamed ") != std::string::npos);
    }

    SECTION("A WebSocket upgrade with an invalid subprotocol or header is not sent") {
        REQUIRE_FALSE(client.websocket(listener.ws_url(), "chat\r\nX-Injected: 1").has_value());
        REQUIRE_FALSE(client.request(listener.ws_url()).header("X-A", bad_value).websocket().has_value());
        REQUIRE_FALSE(client.request(listener.ws_url()).protocol("chat\nx").websocket().has_value());
        REQUIRE(listener.accepts == 0);
        REQUIRE(listener.bytes() == 0);

        // A valid handshake is sent (and refused: the listener answers 200, not 101)
        REQUIRE_FALSE(client.websocket(listener.ws_url(), "chat").has_value());
        REQUIRE(listener.accepts == 1);
        REQUIRE(listener.received().find("Sec-WebSocket-Protocol: chat\r\n") != std::string::npos);
    }

    SECTION("Valid values are sent byte for byte") {
        const std::string obs_text = "caf\xC3\xA9";
        auto response = client.get(listener.url("/valid"), {{"X-Tab", "a\tb"},
                                                           {"X-Obs", obs_text},
                                                           {"X-Empty", ""},
                                                           {"X-Spaces", " padded "}});
        REQUIRE(response.ok());
        auto received = listener.received();
        REQUIRE(received.find("\r\nX-Tab: a\tb\r\n") != std::string::npos);
        REQUIRE(received.find("\r\nX-Obs: " + obs_text + "\r\n") != std::string::npos);
        REQUIRE(received.find("\r\nX-Empty: \r\n") != std::string::npos);
        REQUIRE(received.find("\r\nX-Spaces:  padded \r\n") != std::string::npos);

        auto built = client.request(listener.url("/built")).header("X-Tab", "a\tb").header("X-Obs", obs_text).get();
        REQUIRE(built.ok());
        received = listener.received();
        REQUIRE(received.find("GET /built ") != std::string::npos);
        REQUIRE(received.find("X-Injected") == std::string::npos);
    }
}

TEST_CASE("The async_client callback API hands the failure to the callback", "[client][headers][integration]") {
    counting_listener listener;
    http::test::running_workers workers;
    http::async_client client(workers);

    std::promise<boost::system::error_code> verb;
    std::promise<boost::system::error_code> builder;
    std::promise<bool> websocket;
    REQUIRE_NOTHROW(client.get(listener.url(), [&verb](http::client_response& response) {
        verb.set_value(response.has_error() ? response.error_code() : boost::system::error_code{});
    }, {{"X-A", bad_value}}));
    REQUIRE_NOTHROW(client.request(listener.url()).header("X\r\nY", "v").get([&builder](http::client_response& response) {
        builder.set_value(response.has_error() ? response.error_code() : boost::system::error_code{});
    }));
    REQUIRE_NOTHROW(client.websocket(listener.ws_url(), [&websocket](auto ws) {
        websocket.set_value(static_cast<bool>(ws));
    }, "chat\r\nX-Injected: 1"));
    client.wait();

    REQUIRE(verb.get_future().get() == boost::asio::error::invalid_argument);
    REQUIRE(builder.get_future().get() == boost::asio::error::invalid_argument);
    REQUIRE_FALSE(websocket.get_future().get());
    REQUIRE(listener.accepts == 0);
    REQUIRE(listener.bytes() == 0);
}

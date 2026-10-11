#include <catch2/catch_test_macros.hpp>
#include <thinger/http_client.hpp>
#include <thinger/http/client/client_connection.hpp>
#include <thinger/asio/sockets/tcp_socket.hpp>
#include <boost/asio.hpp>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace thinger;
using namespace std::chrono_literals;

// Responses framed in every way, valid or not, from a server written by hand: the client
// reads each one as its framing says (RFC 9112 section 6.3) or fails with a clear error,
// and never reuses a connection whose next response would start at an unknown byte

namespace {

    using boost::asio::ip::tcp;

    // Answers each request (a head: the client sends no body here) with the reply `respond`
    // returns for its request line, written in parts with a pause between them, and closes
    // the connection after it if the reply says so
    struct raw_server {
        struct reply {
            std::vector<std::string> parts;
            bool close = false;
        };

        boost::asio::io_context ioc;
        tcp::acceptor acceptor{ioc, {boost::asio::ip::make_address("127.0.0.1"), 0}};
        std::function<reply(const std::string& request_line)> respond;
        std::atomic<int> connections{0};
        std::mutex mutex;
        std::vector<std::pair<int, std::string>> requests;  // connection number, request line
        std::thread thread;

        raw_server() {
            co_spawn(ioc, accept_loop(), boost::asio::detached);
            thread = std::thread([this] { ioc.run(); });
        }

        ~raw_server() {
            ioc.stop();
            if (thread.joinable()) thread.join();
        }

        std::string url(const std::string& path) const {
            return "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + path;
        }

        std::vector<std::pair<int, std::string>> received() {
            std::lock_guard lock(mutex);
            return requests;
        }

        awaitable<void> accept_loop() {
            while (true) {
                auto [ec, socket] = co_await acceptor.async_accept(use_nothrow_awaitable);
                if (ec) co_return;
                co_spawn(ioc, session(std::move(socket), ++connections), boost::asio::detached);
            }
        }

        awaitable<void> session(tcp::socket socket, int connection) {
            std::string data;
            char buffer[4096];
            while (true) {
                size_t head_end;
                while ((head_end = data.find("\r\n\r\n")) == std::string::npos) {
                    auto [ec, bytes] = co_await socket.async_read_some(boost::asio::buffer(buffer), use_nothrow_awaitable);
                    if (ec) co_return;
                    data.append(buffer, bytes);
                }
                std::string request_line = data.substr(0, data.find("\r\n"));
                data.erase(0, head_end + 4);
                {
                    std::lock_guard lock(mutex);
                    requests.emplace_back(connection, request_line);
                }
                auto answer = respond(request_line);
                for (size_t i = 0; i < answer.parts.size(); ++i) {
                    if (i > 0) {
                        boost::asio::steady_timer pause(ioc, 50ms);
                        co_await pause.async_wait(use_nothrow_awaitable);
                    }
                    auto [ec, bytes] = co_await boost::asio::async_write(socket, boost::asio::buffer(answer.parts[i]),
                                                                         use_nothrow_awaitable);
                    if (ec) co_return;
                }
                if (answer.close) {
                    boost::system::error_code ignored;
                    socket.shutdown(tcp::socket::shutdown_both, ignored);
                    socket.close(ignored);
                    co_return;
                }
            }
        }
    };

    // Path of a request line ("GET /path HTTP/1.1")
    std::string path_of(const std::string& request_line) {
        auto start = request_line.find(' ') + 1;
        return request_line.substr(start, request_line.find(' ', start) - start);
    }

    http::client_response async_get(const std::string& url) {
        http::async_client client;
        client.timeout(5s);
        http::client_response result;
        client.get(url, [&](http::client_response& res) { result = res; });
        client.wait();
        return result;
    }

    http::client_response sync_get(const std::string& url) {
        http::client client;
        client.timeout(5s);
        return client.get(url);
    }

    const std::string final_response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfinal";
    const std::string smuggled_response = "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nsmuggled";
}

TEST_CASE("HTTP client rejects malformed responses", "[http][client][framing][integration]") {
    struct malformed_case {
        std::string response;
        http::client_error error;
    };
    const std::vector<malformed_case> cases{
        {"HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n", http::client_error::invalid_framing},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n3\r\nabc\r\n0\r\n\r\n", http::client_error::invalid_framing},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n", http::client_error::invalid_framing},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nabc", http::client_error::invalid_framing},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", http::client_error::invalid_framing},
        {"HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd", http::client_error::invalid_framing},
        {"HTTP/1.1 200 OK\r\nContent-Length: 3abc\r\n\r\nabc", http::client_error::invalid_framing},
        {"HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", http::client_error::invalid_framing},
        {"HTTP/1.1 2000 OK\r\nContent-Length: 0\r\n\r\n", http::client_error::invalid_status_line},
        {"HTTP/1.10 200 OK\r\nContent-Length: 0\r\n\r\n", http::client_error::invalid_status_line},
        {"HTTP/2.0 200 OK\r\nContent-Length: 0\r\n\r\n", http::client_error::invalid_status_line},
        {"HTTP/1.1 200 OK\nContent-Length: 0\n\n", http::client_error::invalid_status_line},
        {"SSH-2.0-OpenSSH_9.6\r\n", http::client_error::invalid_status_line},
        {"HTTP/1.1 200 OK\r\nX-Folded: a\r\n b\r\nContent-Length: 0\r\n\r\n", http::client_error::invalid_header},
        {"HTTP/1.1 200 OK\r\nContent-Length : 0\r\n\r\n", http::client_error::invalid_header},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nfffffffffffffffffff\r\n", http::client_error::malformed_body},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0x5\r\nhello\r\n0\r\n\r\n", http::client_error::malformed_body},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloXX0\r\n\r\n", http::client_error::malformed_body},
        {"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", http::client_error::incomplete_response},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n", http::client_error::incomplete_response},
        {"", http::client_error::incomplete_response},
        {"HTTP/1.1 101 Switching Protocols\r\nUpgrade: other\r\nConnection: Upgrade\r\n\r\n", http::client_error::unexpected_upgrade},
    };

    raw_server server;
    std::string response;
    server.respond = [&](const std::string&) { return raw_server::reply{{response}, true}; };

    for (const auto& [raw, error] : cases) {
        response = raw;
        INFO("response: " << raw);
        for (auto get : {sync_get, async_get}) {
            auto res = get(server.url("/"));
            REQUIRE_FALSE(res);
            REQUIRE(res.has_network_error());
            REQUIRE(res.error_code() == error);
            REQUIRE(res.error() == boost::system::error_code(error).message());
        }
    }
}

TEST_CASE("HTTP client reads bodies as their framing says", "[http][client][framing][integration]") {
    raw_server server;
    server.respond = [](const std::string& request_line) -> raw_server::reply {
        auto path = path_of(request_line);
        if (path == "/chunked") {
            return {{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=\"v\"\r\nhello\r\n",
                     "6\r\n world\r\n0\r\nX-Trailer: t\r\n\r\n"}};
        }
        if (path == "/until-close") {
            return {{"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nuntil ", "close"}, true};
        }
        if (path == "/interim") {
            return {{"HTTP/1.1 100 Continue\r\n\r\n", "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n",
                     "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfinal"}};
        }
        if (path == "/no-content") {
            return {{"HTTP/1.1 204 No Content\r\nContent-Length: 5\r\n\r\n"}};
        }
        if (path == "/ows") {
            return {{"HTTP/1.1 200\r\nX-Value:\t value \r\nContent-Length:5 \r\n\r\nhello"}};
        }
        return {{final_response}};
    };

    http::client client;
    client.timeout(5s);

    SECTION("Chunked, with extensions and trailers") {
        auto res = client.get(server.url("/chunked"));
        REQUIRE(res.ok());
        REQUIRE(res.body() == "hello world");
        REQUIRE_FALSE(res.has_header("X-Trailer"));
    }
    SECTION("Until the server closes the connection") {
        auto res = client.get(server.url("/until-close"));
        REQUIRE(res.ok());
        REQUIRE(res.body() == "until close");
    }
    SECTION("After interim responses") {
        auto res = client.get(server.url("/interim"));
        REQUIRE(res.status() == 200);
        REQUIRE(res.body() == "final");
        REQUIRE_FALSE(res.has_header("Link"));
    }
    SECTION("No body for 204, whatever its headers") {
        auto res = client.get(server.url("/no-content"));
        REQUIRE(res.status() == 204);
        REQUIRE(res.body().empty());
    }
    SECTION("Optional whitespace, and no reason phrase") {
        auto res = client.get(server.url("/ows"));
        REQUIRE(res.ok());
        REQUIRE(res.header("X-Value") == "value");
        REQUIRE(res.body() == "hello");
    }
    SECTION("No body for responses to HEAD") {
        server.respond = [](const std::string&) { return raw_server::reply{{"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"}}; };
        auto res = client.head(server.url("/"));
        REQUIRE(res.ok());
        REQUIRE(res.body().empty());
        REQUIRE(res.header("Content-Length") == "5");
    }
}

TEST_CASE("HTTP client does not reuse a connection left out of sync", "[http][client][framing][integration]") {
    // A redirect to the same server is sent on the connection of the redirect response,
    // if that connection can carry another response
    raw_server server;
    std::vector<std::string> redirect;
    bool close_redirect = false;
    server.respond = [&](const std::string& request_line) -> raw_server::reply {
        if (path_of(request_line) == "/start") return {redirect, close_redirect};
        return {{final_response}};
    };
    auto location = "Location: " + server.url("/final") + "\r\n";

    auto follow = [&]() {
        http::client client;
        client.timeout(5s);
        return client.get(server.url("/start"));
    };
    using requests = std::vector<std::pair<int, std::string>>;
    const requests same_connection{{1, "GET /start HTTP/1.1"}, {1, "GET /final HTTP/1.1"}};
    const requests new_connection{{1, "GET /start HTTP/1.1"}, {2, "GET /final HTTP/1.1"}};

    SECTION("Reused after a response framed by its length") {
        redirect = {"HTTP/1.1 302 Found\r\n" + location + "Content-Length: 0\r\n\r\n"};
        auto res = follow();
        REQUIRE(res.body() == "final");
        REQUIRE(server.received() == same_connection);
    }
    SECTION("Reused after a chunked response") {
        redirect = {"HTTP/1.1 302 Found\r\n" + location + "Transfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\n\r\n"};
        auto res = follow();
        REQUIRE(res.body() == "final");
        REQUIRE(server.received() == same_connection);
    }
    SECTION("Not after a body that ends with the connection") {
        // Read as having no body, the redirect would be sent on the same connection, and
        // its response would be the rest of this body
        redirect = {"HTTP/1.1 302 Found\r\n" + location + "\r\n", smuggled_response};
        close_redirect = true;
        auto res = follow();
        REQUIRE(res.body() == "final");
        REQUIRE(server.received() == new_connection);
    }
    SECTION("Not after Connection: close") {
        redirect = {"HTTP/1.1 302 Found\r\n" + location + "Connection: close\r\nContent-Length: 0\r\n\r\n"};
        auto res = follow();
        REQUIRE(res.body() == "final");
        REQUIRE(server.received() == new_connection);
    }
    SECTION("Not when more than the response was received") {
        redirect = {"HTTP/1.1 302 Found\r\n" + location + "Content-Length: 0\r\n\r\n" + smuggled_response};
        auto res = follow();
        REQUIRE(res.body() == "final");
        REQUIRE(server.received() == new_connection);
    }
    SECTION("Not followed after a framing error") {
        redirect = {"HTTP/1.1 302 Found\r\n" + location + "Content-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n" + smuggled_response};
        auto res = follow();
        REQUIRE(res.error_code() == http::client_error::invalid_framing);
        REQUIRE(server.received() == requests{{1, "GET /start HTTP/1.1"}});
    }
}

TEST_CASE("HTTP client connection is closed after an invalid response", "[http][client][framing][integration]") {
    raw_server server;
    server.respond = [](const std::string& request_line) -> raw_server::reply {
        // kept open: only the client can decide not to use it again
        if (path_of(request_line) == "/bad") return {{"HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!"}};
        return {{final_response}};
    };

    boost::asio::io_context ioc;
    auto connection = std::make_shared<http::client_connection>(std::make_shared<asio::tcp_socket>("test", ioc), 5s);
    std::shared_ptr<http::http_response> first, second;
    bool open_after_first = true;
    boost::system::error_code first_error;

    auto exchange = [&](const std::string& path) -> awaitable<std::shared_ptr<http::http_response>> {
        auto request = http::http_request::create_http_request(http::method::GET, server.url(path));
        co_return co_await connection->send_request(request);
    };

    SECTION("Valid responses keep it open") {
        co_spawn(ioc, [&]() -> awaitable<void> {
            first = co_await exchange("/good");
            open_after_first = connection->is_open();
            second = co_await exchange("/good");
        }, boost::asio::detached);
        ioc.run();
        REQUIRE(first);
        REQUIRE(open_after_first);
        REQUIRE(second);
        REQUIRE(server.connections == 1);
    }
    SECTION("An invalid one closes it") {
        co_spawn(ioc, [&]() -> awaitable<void> {
            first = co_await exchange("/bad");
            first_error = connection->last_error();
            open_after_first = connection->is_open();
            second = co_await exchange("/good");
        }, boost::asio::detached);
        ioc.run();
        REQUIRE_FALSE(first);
        REQUIRE(first_error == http::client_error::invalid_framing);
        REQUIRE_FALSE(open_after_first);
        REQUIRE(second);
        REQUIRE(second->get_content() == "final");
        REQUIRE(server.connections == 2);
    }
}

TEST_CASE("HTTP client streaming reports malformed responses", "[http][client][framing][integration]") {
    raw_server server;
    server.respond = [](const std::string& request_line) -> raw_server::reply {
        if (path_of(request_line) == "/bad") {
            return {{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n", "zz\r\n"}, true};
        }
        return {{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n", "6\r\n world\r\n0\r\n\r\n"}};
    };

    http::client client;
    client.timeout(5s);
    std::string streamed;
    auto collect = [&](const http::stream_info& info) {
        streamed.append(info.data);
        return true;
    };

    SECTION("Valid") {
        auto result = client.send_streaming(http::http_request::create_http_request(http::method::GET, server.url("/good")), collect);
        REQUIRE(result.ok());
        REQUIRE(streamed == "hello world");
    }
    SECTION("Malformed") {
        auto result = client.send_streaming(http::http_request::create_http_request(http::method::GET, server.url("/bad")), collect);
        REQUIRE_FALSE(result.ok());
        REQUIRE(result.error == boost::system::error_code(http::client_error::malformed_body).message());
        REQUIRE(streamed == "hello");
    }
}

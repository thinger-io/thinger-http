#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/memory_response.hpp>
#include <thinger/http/server/request.hpp>
#include <boost/asio.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <memory>
#include <string>

// detail::request_source: where a request reads its body from. memory_source serves the body of
// a request dispatched in memory, then the end; a request without a source reads nothing and
// knows no connection. The network source is tested in integration/request_source_test.cpp

using namespace thinger;
using thinger::http::body_error;
using thinger::http::detail::memory_source;

namespace {

    template<typename T>
    T run(thinger::awaitable<T> awaitable) {
        boost::asio::io_context ioc;
        auto result = co_spawn(ioc, std::move(awaitable), boost::asio::use_future);
        ioc.run();
        return result.get();
    }

    // What one read of the source gave
    struct read_result {
        boost::system::error_code ec;
        std::string data;
    };

    read_result read_from(http::detail::request_source& source, size_t size) {
        std::string buffer(size, '\0');
        auto [ec, bytes] = run(source.read_body_some(reinterpret_cast<uint8_t*>(buffer.data()), size));
        buffer.resize(bytes);
        return {ec, buffer};
    }

    // A request whose head announces a body of `length` bytes (Content-Length), not received
    std::shared_ptr<http::http_request> post_head(size_t length) {
        auto request = http::http_request::create_http_request(http::method::POST, "http://localhost/upload");
        request->process_header(http::header::content_length, std::to_string(length));
        return request;
    }

    std::string read_all(http::request& req) {
        return run([&req]() -> thinger::awaitable<std::string> {
            std::string body;
            uint8_t buffer[3];
            while (size_t bytes = co_await req.read_some(buffer, sizeof(buffer))) {
                body.append(reinterpret_cast<char*>(buffer), bytes);
            }
            co_return body;
        }());
    }

}

TEST_CASE("memory_source serves its body, then the end", "[request_source][memory][unit]") {

    SECTION("The body, in reads of any size") {
        memory_source source("hello world", "10.0.0.7");
        auto first = read_from(source, 4);
        REQUIRE_FALSE(first.ec);
        REQUIRE(first.data == "hell");
        auto second = read_from(source, 4);
        REQUIRE_FALSE(second.ec);
        REQUIRE(second.data == "o wo");
        auto last = read_from(source, 64);
        REQUIRE_FALSE(last.ec);
        REQUIRE(last.data == "rld");
    }

    SECTION("EOF after the body, on every read") {
        memory_source source("abc", "");
        REQUIRE(read_from(source, 64).data == "abc");
        for (int i = 0; i < 3; i++) {
            auto end = read_from(source, 64);
            REQUIRE(end.ec == boost::asio::error::eof);
            REQUIRE(end.data.empty());
        }
    }

    SECTION("An empty body: EOF at once") {
        memory_source source("", "10.0.0.7");
        auto end = read_from(source, 64);
        REQUIRE(end.ec == boost::asio::error::eof);
        REQUIRE(end.data.empty());
    }
}

TEST_CASE("memory_source reports the peer of the dispatch, without TLS or a connection", "[request_source][memory][unit]") {
    memory_source source("body", "10.0.0.7");

    auto info = source.info();
    REQUIRE(info.peer_ip == "10.0.0.7");
    REQUIRE(info.peer_port.empty());
    REQUIRE(info.local_port.empty());
    REQUIRE_FALSE(info.secure);
    REQUIRE_FALSE(source.tls().has_value());

    // Nothing behind it: no connection to hold, to write to, or to open sockets on
    source.hold_until(std::chrono::steady_clock::now() + std::chrono::seconds(1));
    REQUIRE(source.connection().expired());
    REQUIRE(source.io_context() == nullptr);

    SECTION("No peer given: empty") {
        memory_source anonymous("", "");
        REQUIRE(anonymous.info().peer_ip.empty());
    }
}

TEST_CASE("A request reads its body from its memory_source", "[request_source][memory][unit]") {

    SECTION("read_some() to the end of the body") {
        http::request req(std::make_unique<memory_source>("hello world", "10.0.0.7"), nullptr, post_head(11));
        REQUIRE(req.has_pending_body());
        REQUIRE(read_all(req) == "hello world");
        REQUIRE_FALSE(req.has_pending_body());
        REQUIRE(req.get_body_error() == body_error::none);
    }

    SECTION("read_body()") {
        http::request req(std::make_unique<memory_source>("hello world", "10.0.0.7"), nullptr, post_head(11));
        REQUIRE(run(req.read_body()));
        REQUIRE(req.body() == "hello world");
    }

    SECTION("An empty body: nothing to read") {
        http::request req(std::make_unique<memory_source>("", "10.0.0.7"), nullptr, post_head(0));
        REQUIRE_FALSE(req.has_pending_body());
        REQUIRE(read_all(req).empty());
        REQUIRE(req.get_body_error() == body_error::none);
    }

    SECTION("A source that ends before the body announced: incomplete") {
        http::request req(std::make_unique<memory_source>("abc", ""), nullptr, post_head(10));
        REQUIRE(read_all(req) == "abc");
        REQUIRE(req.get_body_error() == body_error::incomplete);
    }

    SECTION("The peer and the connection come from the source") {
        http::request req(std::make_unique<memory_source>("", "10.0.0.7"), nullptr, post_head(0));
        REQUIRE(req.get_peer_ip() == "10.0.0.7");
        REQUIRE(req.connection().peer_ip == "10.0.0.7");
        REQUIRE_FALSE(req.connection().secure);
        REQUIRE_FALSE(req.tls().has_value());
        // an address set explicitly wins
        req.set_peer_ip("192.0.2.1");
        REQUIRE(req.get_peer_ip() == "192.0.2.1");
    }
}

TEST_CASE("A request without a source reads nothing and knows no connection", "[request_source][unit]") {
    http::request req(nullptr, nullptr, post_head(5));

    SECTION("Reading its body ends at once, incomplete, without crashing") {
        REQUIRE(req.has_pending_body());
        REQUIRE(read_all(req).empty());
        REQUIRE(req.get_body_error() == body_error::incomplete);
    }

    SECTION("read_body() fails") {
        REQUIRE_FALSE(run(req.read_body()));
    }

    SECTION("Its peer: the one set explicitly, or none") {
        REQUIRE(req.get_peer_ip().empty());
        auto info = req.connection();
        REQUIRE(info.peer_ip.empty());
        REQUIRE(info.peer_port.empty());
        REQUIRE(info.local_port.empty());
        REQUIRE_FALSE(info.secure);
        REQUIRE_FALSE(req.tls().has_value());
        req.set_peer_ip("192.0.2.1");
        REQUIRE(req.get_peer_ip() == "192.0.2.1");
    }

    SECTION("Without a stream either") {
        REQUIRE(req.get_http_stream() == nullptr);
    }
}

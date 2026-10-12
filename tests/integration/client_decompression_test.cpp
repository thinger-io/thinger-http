#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <thinger/http_client.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/util/compression.hpp>
#include <thinger/asio/workers.hpp>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "../fixtures/compression_bomb.hpp"

using namespace thinger;
using namespace std::chrono_literals;
using compression_bomb::mib;

// Responses with Content-Encoding: gzip or deflate are decoded within the maximum content
// size of the client: a body expanding past it (a decompression bomb) fails the request with
// body_too_large as soon as inflating goes past the limit, and one that is not valid gzip or
// deflate fails it with bad_content_encoding. The connection is not reused after either

namespace {

    using compression_bomb::format;

    const char* name_of(format type) { return type == format::gzip ? "gzip" : "deflate"; }

    std::string compress(format type, const std::string& data) {
        auto compressed = type == format::gzip ? util::gzip::compress(data) : util::deflate::compress(data);
        REQUIRE(compressed);
        return *compressed;
    }

    // A server answering GET /<name> with the body set for it, as it is, with its
    // Content-Encoding (none if empty), and telling the connections (client ports) and the
    // Accept-Encoding of the requests
    struct encoded_server {
        struct body {
            std::string encoding;
            std::string data;
        };

        http::server server;
        uint16_t port = 0;
        std::thread thread;

        // (shared with the thread of the server: TSan does not see the order sockets give)
        std::mutex mutex;
        std::map<std::string, body> bodies;
        std::set<std::string> client_ports;
        std::vector<std::string> accept_encodings;

        encoded_server() {
            server.get("/:name", [this](http::request& req, http::response& res) {
                body answer;
                {
                    std::lock_guard lock(mutex);
                    client_ports.insert(req.get_http_connection()->get_socket()->get_remote_port());
                    accept_encodings.push_back(req.header("Accept-Encoding"));
                    auto it = bodies.find(req["name"]);
                    if (it == bodies.end()) {
                        res.status(http::http_response::status::not_found);
                        res.send("not found");
                        return;
                    }
                    answer = it->second;
                }
                if (!answer.encoding.empty()) res.header("Content-Encoding", answer.encoding);
                res.send(answer.data, "application/octet-stream");
            });
            REQUIRE(server.listen("127.0.0.1", 0));
            port = server.local_port();
            thread = std::thread([this] { server.wait(); });
        }

        ~encoded_server() {
            server.stop();
            if (thread.joinable()) thread.join();
        }

        void set(const std::string& name, std::string encoding, std::string data) {
            std::lock_guard lock(mutex);
            bodies[name] = {std::move(encoding), std::move(data)};
        }

        size_t connections() {
            std::lock_guard lock(mutex);
            return client_ports.size();
        }

        std::vector<std::string> received_accept_encodings() {
            std::lock_guard lock(mutex);
            return accept_encodings;
        }

        std::string url(const std::string& name) const {
            return "http://127.0.0.1:" + std::to_string(port) + "/" + name;
        }
    };

}

TEST_CASE("Client decodes compressed responses within its maximum content size", "[http][client][compression][integration]") {
    auto type = GENERATE(format::gzip, format::deflate);
    CAPTURE(name_of(type));

    encoded_server server;
    http::client client;
    client.timeout(5s);
    const size_t limit = mib;
    client.max_content_size(limit);

    SECTION("A small response") {
        const std::string original = "{\"hello\":\"world\"}";
        server.set("small", name_of(type), compress(type, original));
        auto res = client.get(server.url("small"));
        REQUIRE(res.ok());
        REQUIRE(res.body() == original);
        REQUIRE_FALSE(res.has_header("Content-Encoding"));
    }

    SECTION("Exactly at the limit") {
        const std::string original(limit, 'a');
        server.set("exact", name_of(type), compress(type, original));
        auto res = client.get(server.url("exact"));
        REQUIRE(res.ok());
        REQUIRE(res.body() == original);
    }

    SECTION("One byte over the limit") {
        server.set("over", name_of(type), compress(type, std::string(limit + 1, 'a')));
        auto res = client.get(server.url("over"));
        REQUIRE_FALSE(res);
        REQUIRE(res.error_code() == http::client_error::body_too_large);
    }

    SECTION("An empty body keeps its Content-Encoding (a response to HEAD)") {
        server.set("head", name_of(type), compress(type, "some data"));
        auto res = client.head(server.url("head"));
        REQUIRE(res.ok());
        REQUIRE(res.body().empty());
        REQUIRE(res.header("Content-Encoding") == name_of(type));
    }

    SECTION("Other codings are kept as they are") {
        server.set("br", "br", "not decoded");
        auto res = client.get(server.url("br"));
        REQUIRE(res.ok());
        REQUIRE(res.body() == "not decoded");
        REQUIRE(res.header("Content-Encoding") == "br");
    }
}

TEST_CASE("Client stops inflating decompression bombs at its maximum content size", "[http][client][compression][integration]") {
    auto type = GENERATE(format::gzip, format::deflate);
    CAPTURE(name_of(type));

    encoded_server server;
    http::client client;
    client.timeout(10s);

    SECTION("A few KB per MiB, expanding to 1 GiB, with the default limit (8 MB)") {
        auto bomb = compression_bomb::zeros(type, 1024);
        REQUIRE(bomb.size() < 2 * mib);
        server.set("bomb", name_of(type), std::move(bomb));
        auto start = std::chrono::steady_clock::now();
        auto res = client.get(server.url("bomb"));
        REQUIRE_FALSE(res);
        REQUIRE(res.error_code() == http::client_error::body_too_large);
        REQUIRE(std::chrono::steady_clock::now() - start < 5s);
    }

    SECTION("Inflating stops at the limit, before the end of the body") {
        // Its broken checksum is at the end: past the limit it is too large, not invalid
        auto bomb = compression_bomb::zeros(type, 64);
        bomb.back() = static_cast<char>(bomb.back() ^ 0x55);
        server.set("bomb", name_of(type), bomb);
        client.max_content_size(mib);
        auto res = client.get(server.url("bomb"));
        REQUIRE(res.error_code() == http::client_error::body_too_large);
        client.max_content_size(128 * mib);
        res = client.get(server.url("bomb"));
        REQUIRE(res.error_code() == http::client_error::bad_content_encoding);
    }

    SECTION("The asynchronous client too") {
        server.set("bomb", name_of(type), compression_bomb::zeros(type, 64));
        asio::get_workers().start(2);
        boost::system::error_code error;
        {
            http::async_client async;
            async.timeout(5s);
            async.max_content_size(mib);
            std::promise<boost::system::error_code> done;
            async.get(server.url("bomb"), [&](const http::client_response& res) { done.set_value(res.error_code()); });
            error = done.get_future().get();
            async.wait();
        }
        asio::get_workers().stop();
        REQUIRE(error == http::client_error::body_too_large);
    }
}

TEST_CASE("Client fails on malformed compressed responses", "[http][client][compression][integration]") {
    auto type = GENERATE(format::gzip, format::deflate);
    CAPTURE(name_of(type));

    encoded_server server;
    http::client client;
    client.timeout(5s);
    auto compressed = compress(type, std::string(10000, 'x') + "end");

    std::string body;
    SECTION("Truncated") { body = compressed.substr(0, compressed.size() / 2); }
    SECTION("Without its trailer") { body = compressed.substr(0, compressed.size() - 4); }
    SECTION("Not compressed") { body = "plain text, not compressed"; }
    SECTION("Corrupted") {
        body = compressed;
        body[body.size() / 2] = static_cast<char>(body[body.size() / 2] ^ 0xff);
    }
    CAPTURE(body.size());

    server.set("bad", name_of(type), body);
    auto res = client.get(server.url("bad"));
    REQUIRE_FALSE(res);
    REQUIRE(res.error_code() == http::client_error::bad_content_encoding);
    REQUIRE(res.get() == nullptr);
}

TEST_CASE("Client does not reuse a connection after a compressed response fails", "[http][client][compression][pool][integration]") {
    auto type = GENERATE(format::gzip, format::deflate);
    CAPTURE(name_of(type));

    encoded_server server;
    http::client client;
    client.timeout(5s);
    client.max_content_size(mib);
    server.set("ok", name_of(type), compress(type, "fine"));
    server.set("bomb", name_of(type), compression_bomb::zeros(type, 16));
    server.set("bad", name_of(type), "not compressed");

    auto error = GENERATE(as<std::string>{}, "bomb", "bad");
    CAPTURE(error);

    REQUIRE(client.get(server.url("ok")).body() == "fine");
    REQUIRE(client.get(server.url("ok")).body() == "fine");
    REQUIRE(server.connections() == 1);

    // On the same connection, closed after it fails
    auto res = client.get(server.url(error));
    REQUIRE_FALSE(res);
    auto stats = client.pool_stats();
    REQUIRE(stats.idle == 0);
    REQUIRE(stats.created == 1);
    REQUIRE(stats.reused == 2);

    REQUIRE(client.get(server.url("ok")).body() == "fine");
    REQUIRE(server.connections() == 2);
    stats = client.pool_stats();
    REQUIRE(stats.created == 2);
    REQUIRE(stats.idle == 1);
}

TEST_CASE("Client streams compressed responses as received", "[http][client][compression][integration]") {
    // A stream callback gets the body as it arrives: it is not decoded, so the client never
    // inflates it. The request asks for identity unless it sets its own Accept-Encoding
    encoded_server server;
    http::client client;
    client.timeout(5s);
    client.max_content_size(mib);
    auto bomb = compression_bomb::zeros(format::gzip, 64);
    server.set("bomb", "gzip", bomb);

    SECTION("Accepting gzip: the compressed bytes") {
        std::string streamed;
        auto result = client.request(server.url("bomb"))
            .header("Accept-Encoding", "gzip")
            .get([&](const http::stream_info& info) {
                streamed.append(info.data);
                return true;
            });
        REQUIRE(result.ok());
        REQUIRE(streamed == bomb);
        REQUIRE(server.received_accept_encodings() == std::vector<std::string>{"gzip"});
    }

    SECTION("By default, identity is asked for") {
        auto result = client.request(server.url("bomb")).get([](const http::stream_info&) { return true; });
        REQUIRE(result.ok());
        REQUIRE(server.received_accept_encodings() == std::vector<std::string>{"identity"});
    }
}

#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/reverse_proxy.hpp>
#include <thinger/http/server/websocket_connection.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/asio/ssl/certificate_manager.hpp>
#include <thinger/util/types.hpp>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <boost/asio/use_future.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <unistd.h>
#include "../../examples/http_server/device_proxy_policy.hpp"
#include <atomic>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

using namespace thinger;
using namespace std::chrono_literals;

namespace {

    // An HTTP server running on its own thread
    struct test_server {
        http::server server;
        std::thread thread;
        uint16_t port = 0;

        void start() {
            REQUIRE(server.listen("127.0.0.1", 0));
            port = server.local_port();
            thread = std::thread([this] { server.wait(); });
        }

        void start_unix(const std::string& path) {
            REQUIRE(server.listen_unix(path));
            thread = std::thread([this] { server.wait(); });
        }

        std::string url(const std::string& path = "") const {
            return "http://127.0.0.1:" + std::to_string(port) + path;
        }

        ~test_server() {
            server.stop();
            if (thread.joinable()) thread.join();
        }
    };

    // Joins a thread when leaving the scope
    struct joiner {
        std::thread thread;
        ~joiner() {
            if (thread.joinable()) thread.join();
        }
    };

    // A value set by the test and read by the server threads
    template<typename T>
    struct shared_value {
        std::mutex mutex;
        T value{};

        void set(T new_value) {
            std::lock_guard lock(mutex);
            value = std::move(new_value);
        }

        T get() {
            std::lock_guard lock(mutex);
            return value;
        }
    };

    bool wait_for(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 5s) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!condition()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    std::string lower(std::string value) {
        for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    }

    // A response read from a raw connection
    struct raw_response {
        int status = 0;
        std::string status_line;
        std::map<std::string, std::string> headers;    // lowercase names
        std::string body;
        bool complete = false;

        bool has(const std::string& name) const { return headers.contains(lower(name)); }
        std::string header(const std::string& name) const {
            auto it = headers.find(lower(name));
            return it == headers.end() ? "" : it->second;
        }
        nlohmann::json json() const { return nlohmann::json::parse(body); }
    };

    // A client connection sending and reading raw bytes
    struct raw_connection {
        boost::asio::io_context ioc;
        boost::asio::ip::tcp::socket socket{ioc};
        std::string pending;
        bool closed = false;

        explicit raw_connection(uint16_t port) {
            socket.connect({boost::asio::ip::make_address("127.0.0.1"), port});
        }

        void send(std::string_view data) {
            boost::asio::write(socket, boost::asio::buffer(data.data(), data.size()));
        }

        // Read once more into `pending`; false on timeout or once the connection is closed
        bool read_more(std::chrono::milliseconds timeout) {
            if (closed) return false;
            char data[16384];
            boost::system::error_code result;
            size_t bytes = 0;
            bool done = false;
            socket.async_read_some(boost::asio::buffer(data), [&](boost::system::error_code ec, size_t n) {
                result = ec;
                bytes = n;
                done = true;
            });
            ioc.restart();
            ioc.run_for(timeout);
            if (!done) {
                socket.cancel();
                ioc.restart();
                ioc.run();
                return false;
            }
            pending.append(data, bytes);
            if (result) closed = true;
            return !result;
        }

        bool read_until(const std::string& needle, std::chrono::milliseconds timeout = 5s) {
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (pending.find(needle) == std::string::npos) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (left.count() <= 0 || !read_more(left)) return pending.find(needle) != std::string::npos;
            }
            return true;
        }

        // Whether the peer closes the connection within the timeout
        bool read_until_closed(std::chrono::milliseconds timeout = 5s) {
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!closed) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (left.count() <= 0) return false;
                read_more(left);
            }
            return true;
        }

        // Read `size` bytes at the start of `pending`
        bool need(size_t size, std::chrono::steady_clock::time_point deadline) {
            while (pending.size() < size) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (left.count() <= 0 || !read_more(left)) return pending.size() >= size;
            }
            return true;
        }

        raw_response read_response(bool head_request = false, std::chrono::milliseconds timeout = 5s) {
            raw_response response;
            auto deadline = std::chrono::steady_clock::now() + timeout;
            if (!read_until("\r\n\r\n", timeout)) return response;
            auto end = pending.find("\r\n\r\n");
            std::string head = pending.substr(0, end + 2);
            pending.erase(0, end + 4);

            auto line_end = head.find("\r\n");
            response.status_line = head.substr(0, line_end);
            response.status = std::stoi(response.status_line.substr(9, 3));
            size_t position = line_end + 2;
            while (position < head.size()) {
                auto next = head.find("\r\n", position);
                auto line = head.substr(position, next - position);
                auto colon = line.find(':');
                auto name = lower(line.substr(0, colon));
                auto value = line.substr(colon + 1);
                while (!value.empty() && value.front() == ' ') value.erase(0, 1);
                if (response.headers.contains(name)) value = response.headers[name] + ", " + value;
                response.headers[name] = value;
                position = next + 2;
            }

            int code = response.status;
            if (head_request || code < 200 || code == 204 || code == 304) {
                response.complete = true;
                return response;
            }
            if (lower(response.header("Transfer-Encoding")) == "chunked") {
                while (true) {
                    if (!read_until("\r\n", std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now()))) {
                        return response;
                    }
                    auto line = pending.find("\r\n");
                    size_t size = std::stoul(pending.substr(0, line), nullptr, 16);
                    if (!need(line + 2 + size + 2, deadline)) return response;
                    response.body += pending.substr(line + 2, size);
                    pending.erase(0, line + 2 + size + 2);
                    if (size == 0) {
                        response.complete = true;
                        return response;
                    }
                }
            }
            if (response.has("Content-Length")) {
                size_t size = std::stoul(response.header("Content-Length"));
                bool whole = need(size, deadline);
                response.body = pending.substr(0, std::min(size, pending.size()));
                pending.erase(0, response.body.size());
                response.complete = whole;
                return response;
            }
            response.complete = read_until_closed(std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()));
            response.body = std::exchange(pending, {});
            return response;
        }
    };

    raw_response raw_request(uint16_t port, const std::string& request, bool head_request = false) {
        raw_connection connection(port);
        connection.send(request);
        return connection.read_response(head_request);
    }

    // A target written by hand: reads each request (with its body) on a new connection,
    // records it, and answers what `respond` returns, closing the connection afterwards
    // (or nothing, keeping the connection open, without `respond`)
    struct fake_target {
        boost::asio::io_context ioc;
        boost::asio::ip::tcp::acceptor acceptor{ioc, {boost::asio::ip::make_address("127.0.0.1"), 0}};
        std::thread thread;
        std::mutex mutex;
        std::vector<std::string> requests;
        std::function<std::string(const std::string&)> respond;

        explicit fake_target(std::function<std::string(const std::string&)> responder = {})
            : respond(std::move(responder)) {
            co_spawn(ioc, accept_loop(), detached);
            thread = std::thread([this] { ioc.run(); });
        }

        ~fake_target() {
            ioc.stop();
            if (thread.joinable()) thread.join();
        }

        uint16_t port() const { return acceptor.local_endpoint().port(); }

        size_t count() {
            std::lock_guard lock(mutex);
            return requests.size();
        }

        std::string request(size_t index) {
            std::lock_guard lock(mutex);
            return index < requests.size() ? requests[index] : std::string{};
        }

        static bool complete_request(const std::string& data) {
            auto end = data.find("\r\n\r\n");
            if (end == std::string::npos) return false;
            auto head = lower(data.substr(0, end + 2));
            auto body = data.size() - end - 4;
            if (auto position = head.find("\r\ncontent-length: "); position != std::string::npos) {
                return body >= std::stoul(head.substr(position + 18));
            }
            if (head.find("\r\ntransfer-encoding: chunked") != std::string::npos) {
                return data.ends_with("0\r\n\r\n");
            }
            return true;
        }

        awaitable<void> accept_loop() {
            while (true) {
                auto [ec, socket] = co_await acceptor.async_accept(use_nothrow_awaitable);
                if (ec) co_return;
                co_spawn(ioc, session(std::move(socket)), detached);
            }
        }

        awaitable<void> session(boost::asio::ip::tcp::socket socket) {
            std::string data;
            char buffer[16384];
            while (!complete_request(data)) {
                auto [ec, bytes] = co_await socket.async_read_some(boost::asio::buffer(buffer), use_nothrow_awaitable);
                if (ec) co_return;
                data.append(buffer, bytes);
            }
            {
                std::lock_guard lock(mutex);
                requests.push_back(data);
            }
            // without an answer: keep the connection open, silent
            if (!respond) {
                boost::asio::steady_timer timer(ioc, 60s);
                co_await timer.async_wait(use_nothrow_awaitable);
                co_return;
            }
            auto answer = respond(data);
            co_await boost::asio::async_write(socket, boost::asio::buffer(answer), use_nothrow_awaitable);
            boost::system::error_code ignored;
            socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
            socket.close(ignored);
        }
    };

    // Target answering any request with what it received: method, path, headers and body
    void add_inspect_handler(http::server& server) {
        server.set_not_found_handler([](http::request& req, http::response& res) -> awaitable<void> {
            std::string body;
            uint8_t buffer[8192];
            while (size_t bytes = co_await req.read_some(buffer, sizeof(buffer))) {
                body.append(reinterpret_cast<const char*>(buffer), bytes);
            }
            auto http_request = req.get_http_request();
            nlohmann::json headers = nlohmann::json::object();
            for (const auto& [name, value] : http_request->get_headers()) {
                auto key = lower(name);
                headers[key] = headers.contains(key) ? headers[key].get<std::string>() + ", " + value : value;
            }
            res.json({{"method", http::get_method(http_request->get_method())},
                      {"uri", http_request->get_uri()},
                      {"headers", headers},
                      {"body", body}});
        });
    }

    // Proxy server forwarding every request to `target`, with `options` or those of the proxy
    void forward_all(http::server& server, http::reverse_proxy& proxy, http::proxy_target target,
                     std::optional<http::proxy_options> options = std::nullopt) {
        server.set_not_found_handler([&proxy, target, options](http::request& req, http::response& res) -> awaitable<void> {
            if (options) {
                co_await proxy.forward(req, res, target, *options);
            } else {
                co_await proxy.forward(req, res, target);
            }
        });
    }

    http::proxy_target target_of(uint16_t port) {
        auto target = http::proxy_target::from_url("http://127.0.0.1:" + std::to_string(port));
        REQUIRE(target);
        return *target;
    }

    std::string get(const std::string& path, const std::string& extra_headers = "") {
        return "GET " + path + " HTTP/1.1\r\nHost: front.example\r\n" + extra_headers + "\r\n";
    }

    // A self-signed certificate for `name`, valid as its own CA
    std::pair<std::string, std::string> make_certificate(const std::string& name) {
        EVP_PKEY* key = EVP_EC_gen("P-256");
        X509* certificate = X509_new();
        X509_set_version(certificate, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(certificate), static_cast<long>(std::time(nullptr)));
        X509_gmtime_adj(X509_getm_notBefore(certificate), -3600);
        X509_gmtime_adj(X509_getm_notAfter(certificate), 24 * 3600);
        X509_set_pubkey(certificate, key);
        X509_NAME* subject = X509_get_subject_name(certificate);
        X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(name.c_str()), -1, -1, 0);
        X509_set_issuer_name(certificate, subject);

        X509V3_CTX context;
        X509V3_set_ctx_nodb(&context);
        X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);
        std::string san = "DNS:" + name;
        for (auto [nid, value] : {std::pair{NID_subject_alt_name, san.c_str()},
                                  std::pair{NID_basic_constraints, "critical,CA:TRUE"},
                                  std::pair{NID_key_usage, "critical,digitalSignature,keyCertSign"}}) {
            X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value);
            X509_add_ext(certificate, extension, -1);
            X509_EXTENSION_free(extension);
        }
        X509_sign(certificate, key, EVP_sha256());

        auto to_pem = [](auto write) {
            BIO* bio = BIO_new(BIO_s_mem());
            write(bio);
            char* data = nullptr;
            long size = BIO_get_mem_data(bio, &data);
            std::string pem(data, static_cast<size_t>(size));
            BIO_free(bio);
            return pem;
        };
        auto certificate_pem = to_pem([&](BIO* bio) { PEM_write_bio_X509(bio, certificate); });
        auto key_pem = to_pem([&](BIO* bio) { PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr); });
        X509_free(certificate);
        EVP_PKEY_free(key);
        return {certificate_pem, key_pem};
    }

    awaitable<void> echo_session(std::shared_ptr<asio::socket> socket, std::string buffered) {
        std::string welcome = "welcome[" + buffered + "]";
        co_await socket->write(welcome);
        uint8_t buffer[1024];
        while (true) {
            auto [ec, bytes] = co_await socket->read_some(buffer, sizeof(buffer));
            if (ec || bytes == 0) break;
            co_await socket->write(buffer, bytes);
        }
    }

}

TEST_CASE("Reverse proxy target URLs", "[proxy][unit]") {
    auto target = http::proxy_target::from_url("https://backend.example:8443/api/");
    REQUIRE(target);
    REQUIRE(target->type == http::proxy_target::transport::tls);
    REQUIRE(target->host == "backend.example");
    REQUIRE(target->port == 8443);
    REQUIRE(target->base_path == "/api");
    REQUIRE(target->authority() == "backend.example:8443");

    target = http::proxy_target::from_url("ws://[::1]");
    REQUIRE(target);
    REQUIRE(target->type == http::proxy_target::transport::tcp);
    REQUIRE(target->host == "::1");
    REQUIRE(target->port == 80);
    REQUIRE(target->authority() == "[::1]");

    REQUIRE(http::proxy_target::unix_socket("/run/app.sock").authority() == "localhost");

    for (auto invalid : {"ftp://host", "http://", "http://host:", "http://host:99999", "http://user@host",
                         "http://host/path?query", "http://ho st", "http://[::1", "host:80"}) {
        INFO(invalid);
        REQUIRE_FALSE(http::proxy_target::from_url(invalid));
    }
}

TEST_CASE("Reverse proxy forwards requests and responses", "[proxy][integration]") {
    test_server target;
    add_inspect_handler(target.server);
    target.server.get("/teapot", [](http::response& res) {
        res.json({{"tea", true}}, static_cast<http::http_response::status>(418));
    });
    target.start();

    http::reverse_proxy reverse_proxy;
    std::mutex mutex;
    http::proxy_result last;
    std::atomic<int> results{0};
    auto destination = target_of(target.port);
    test_server proxy;
    proxy.server.set_not_found_handler([&](http::request& req, http::response& res) -> awaitable<void> {
        auto result = co_await reverse_proxy.forward(req, res, destination);
        std::lock_guard lock(mutex);
        last = result;
        results++;
    });
    proxy.start();

    http::client client;

    SECTION("GET with query") {
        auto res = client.get(proxy.url("/items/7?sort=desc&q=a%20b"));
        REQUIRE(res.status() == 200);
        auto json = res.json();
        REQUIRE(json["method"] == "GET");
        REQUIRE(json["uri"] == "/items/7?sort=desc&q=a%20b");
        REQUIRE(json["headers"]["host"] == "127.0.0.1:" + std::to_string(target.port));

        // (the handler gets the result once the response is sent)
        REQUIRE(wait_for([&] { return results == 1; }));
        std::lock_guard lock(mutex);
        REQUIRE(last.status == 200);
        REQUIRE(last.from_target);
        REQUIRE(last.complete);
        REQUIRE(last.error.empty());
        REQUIRE(last.response_bytes > 0);
    }

    SECTION("POST with a body") {
        std::string body(300000, 'x');
        auto res = client.post(proxy.url("/upload"), body, "text/plain");
        REQUIRE(res.status() == 200);
        auto json = res.json();
        REQUIRE(json["method"] == "POST");
        REQUIRE(json["body"] == body);
        REQUIRE(json["headers"]["content-length"] == std::to_string(body.size()));

        REQUIRE(wait_for([&] { return results == 1; }));
        std::lock_guard lock(mutex);
        REQUIRE(last.request_bytes == body.size());
    }

    SECTION("Any status of the target") {
        auto res = client.get(proxy.url("/teapot"));
        REQUIRE(res.status() == 418);
        REQUIRE(res.json()["tea"] == true);
    }

    SECTION("Several requests on a keep-alive connection") {
        raw_connection connection(proxy.port);
        for (int i = 0; i < 3; ++i) {
            connection.send(get("/n/" + std::to_string(i)));
            auto res = connection.read_response();
            REQUIRE(res.complete);
            REQUIRE(res.json()["uri"] == "/n/" + std::to_string(i));
        }
    }
}

TEST_CASE("Reverse proxy streams request bodies as they arrive", "[proxy][integration]") {
    test_server target;
    std::atomic<size_t> received{0};
    target.server.post("/upload", [&received](http::request& req, http::response& res) -> awaitable<void> {
        uint8_t buffer[8192];
        size_t total = 0;
        while (size_t bytes = co_await req.read_some(buffer, sizeof(buffer))) {
            total += bytes;
            received = total;
        }
        res.json({{"received", total}, {"failed", req.get_body_error() != http::body_error::none}});
    });
    target.start();

    http::reverse_proxy reverse_proxy;
    test_server proxy;
    forward_all(proxy.server, reverse_proxy, target_of(target.port));
    proxy.start();

    const size_t part = 512 * 1024;
    raw_connection connection(proxy.port);

    SECTION("Content-Length") {
        connection.send("POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(2 * part) + "\r\n\r\n");
        connection.send(std::string(part, 'a'));
        // the target gets the first half before the client sends the rest
        REQUIRE(wait_for([&] { return received >= part; }));
        connection.send(std::string(part, 'b'));
    }

    SECTION("Chunked") {
        connection.send("POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
        auto chunk = [](size_t size, char c) {
            std::ostringstream hex;
            hex << std::hex << size;
            return hex.str() + "\r\n" + std::string(size, c) + "\r\n";
        };
        connection.send(chunk(part, 'a'));
        REQUIRE(wait_for([&] { return received >= part; }));
        connection.send(chunk(part, 'b') + "0\r\n\r\n");
    }

    auto res = connection.read_response();
    REQUIRE(res.status == 200);
    REQUIRE(res.json()["received"] == 2 * part);
    REQUIRE(res.json()["failed"] == false);
}

TEST_CASE("Reverse proxy streams responses as they arrive", "[proxy][integration]") {
    test_server target;
    std::atomic<bool> client_got_first{false};
    std::atomic<bool> first_seen_in_time{false};

    // writes the second part once the client got the first one (or gives up after 5s)
    auto wait_client = [&]() -> awaitable<void> {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
        for (int i = 0; i < 500 && !client_got_first; ++i) {
            timer.expires_after(10ms);
            co_await timer.async_wait(use_nothrow_awaitable);
        }
        first_seen_in_time = client_got_first.load();
    };

    target.server.get("/chunks", [&](http::response& res) -> awaitable<void> {
        res.start_chunked("text/plain");
        res.write_chunk("first-part;");
        co_await wait_client();
        res.write_chunk("second-part");
        res.end_chunked();
    });

    std::string large(4 * 1024 * 1024, '\0');
    for (size_t i = 0; i < large.size(); ++i) large[i] = static_cast<char>('a' + i % 26);
    target.server.get("/large", [&large](http::response& res) {
        res.send(large, "application/octet-stream");
    });

    target.server.get("/events", [&](http::response& res) {
        res.start_sse([&](std::shared_ptr<http::sse_connection> sse) {
            sse->send_data("first");
            co_spawn(target.server.get_io_context(), [&, sse]() -> awaitable<void> {
                co_await wait_client();
                sse->send_data("second");
                boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, 50ms);
                co_await timer.async_wait(use_nothrow_awaitable);
                sse->stop();
            }, detached);
        });
    });
    target.start();

    http::reverse_proxy reverse_proxy;
    test_server proxy;
    forward_all(proxy.server, reverse_proxy, target_of(target.port));
    proxy.start();

    SECTION("Chunked response") {
        raw_connection connection(proxy.port);
        connection.send(get("/chunks"));
        REQUIRE(connection.read_until("first-part;"));
        client_got_first = true;
        auto res = connection.read_response();
        REQUIRE(res.complete);
        REQUIRE(res.header("Transfer-Encoding") == "chunked");
        REQUIRE(res.body == "first-part;second-part");
        REQUIRE(first_seen_in_time);
    }

    SECTION("Server-sent events, ended by the target closing the connection") {
        raw_connection connection(proxy.port);
        connection.send(get("/events"));
        REQUIRE(connection.read_until("data: first\n\n"));
        client_got_first = true;
        auto res = connection.read_response();
        REQUIRE(res.complete);
        REQUIRE(res.header("Content-Type") == "text/event-stream");
        REQUIRE(res.body == "data: first\n\ndata: second\n\n");
        REQUIRE(first_seen_in_time);
    }

    SECTION("Large response with Content-Length") {
        raw_connection connection(proxy.port);
        connection.send(get("/large"));
        auto res = connection.read_response(false, 20s);
        REQUIRE(res.complete);
        REQUIRE(res.header("Content-Length") == std::to_string(large.size()));
        REQUIRE(res.body == large);
    }

    SECTION("HTTP/1.0 client: body delimited by closing the connection") {
        client_got_first = true;
        raw_connection connection(proxy.port);
        connection.send("GET /chunks HTTP/1.0\r\nHost: x\r\n\r\n");
        auto res = connection.read_response();
        REQUIRE(res.complete);
        REQUIRE_FALSE(res.has("Transfer-Encoding"));
        REQUIRE_FALSE(res.has("Content-Length"));
        REQUIRE(res.body == "first-part;second-part");
    }
}

TEST_CASE("Reverse proxy tunnels WebSocket connections", "[proxy][websocket][integration]") {
    test_server target;
    target.server.get("/ws", [](http::response& res) {
        res.upgrade_websocket([](std::shared_ptr<http::websocket_connection> ws) {
            // sent right after the handshake: it may reach the proxy with the 101
            ws->send_text("welcome");
            ws->on_message([weak = std::weak_ptr<http::websocket_connection>(ws)](const std::string& message, bool) {
                if (auto ws = weak.lock()) ws->send_text("echo:" + message);
            });
        });
    });
    target.server.get("/raw", [](http::response& res) {
        res.status(http::http_response::status::switching_protocols);
        res.header("Upgrade", "echo");
        res.header("Connection", "Upgrade");
        res.take_over([](std::shared_ptr<asio::socket> socket, std::string buffered) {
            co_spawn(socket->get_io_context(), echo_session(socket, std::move(buffered)), detached);
        });
    });
    target.start();

    http::reverse_proxy reverse_proxy;
    std::atomic<bool> upgraded{false};
    auto destination = target_of(target.port);
    test_server proxy;
    proxy.server.set_not_found_handler([&](http::request& req, http::response& res) -> awaitable<void> {
        auto result = co_await reverse_proxy.forward(req, res, destination);
        upgraded = result.upgraded;
    });
    proxy.start();

    SECTION("WebSocket echo") {
        http::client client;
        auto ws = client.websocket("ws://127.0.0.1:" + std::to_string(proxy.port) + "/ws");
        REQUIRE(ws);
        REQUIRE(ws->receive().first == "welcome");
        for (const std::string& message : std::vector<std::string>{"hello", "world", std::string(1000, 'x')}) {
            REQUIRE(ws->send_text(message));
            REQUIRE(ws->receive().first == "echo:" + message);
        }
        ws->close();
        REQUIRE(wait_for([&] { return upgraded.load(); }));
    }

    SECTION("Bytes sent right after the handshake, on both sides") {
        raw_connection connection(proxy.port);
        connection.send("GET /raw HTTP/1.1\r\nHost: x\r\nUpgrade: echo\r\nConnection: Upgrade\r\n\r\nEARLY");
        REQUIRE(connection.read_until("EARLY"));
        REQUIRE(connection.pending.starts_with("HTTP/1.1 101"));
        REQUIRE(connection.pending.find("welcome[") != std::string::npos);
        auto head = connection.pending.substr(0, connection.pending.find("\r\n\r\n"));
        REQUIRE(lower(head).find("upgrade: echo") != std::string::npos);
        REQUIRE(lower(head).find("connection: upgrade") != std::string::npos);

        connection.pending.clear();
        connection.send("ping");
        REQUIRE(connection.read_until("ping"));

        // bulk data both ways
        connection.pending.clear();
        std::string bulk;
        auto deadline = std::chrono::steady_clock::now() + 20s;
        for (int i = 0; i < 32; ++i) {
            std::string part(64 * 1024, static_cast<char>('a' + i % 26));
            connection.send(part);
            bulk += part;
            REQUIRE(connection.need(bulk.size(), deadline));
        }
        REQUIRE(connection.pending == bulk);
    }

    SECTION("Upgrades can be disabled") {
        http::reverse_proxy no_upgrades({.allow_upgrade = false});
        test_server other;
        forward_all(other.server, no_upgrades, destination);
        other.start();
        auto res = raw_request(other.port, get("/raw", "Upgrade: echo\r\nConnection: Upgrade\r\n"));
        // the target sees a plain request: its 101 is not expected
        REQUIRE(res.status == 502);
    }
}

TEST_CASE("Reverse proxy forwards to Unix socket targets", "[proxy][unix][integration]") {
    auto path = (std::filesystem::temp_directory_path() /
                 ("thinger_proxy_test_" + std::to_string(::getpid()) + ".sock")).string();
    std::filesystem::remove(path);

    test_server target;
    add_inspect_handler(target.server);
    target.start_unix(path);

    http::reverse_proxy reverse_proxy;
    test_server proxy;
    forward_all(proxy.server, reverse_proxy, http::proxy_target::unix_socket(path, "/plugin"));
    proxy.start();

    auto res = raw_request(proxy.port, "POST /data?x=1 HTTP/1.1\r\nHost: front.example\r\nContent-Length: 4\r\n\r\nbody");
    REQUIRE(res.status == 200);
    auto json = res.json();
    REQUIRE(json["uri"] == "/plugin/data?x=1");
    REQUIRE(json["body"] == "body");
    REQUIRE(json["headers"]["host"] == "localhost");
    REQUIRE(json["headers"]["x-forwarded-host"] == "front.example");

    std::filesystem::remove(path);
}

TEST_CASE("Reverse proxy verifies TLS targets", "[proxy][tls][integration]") {
    auto [certificate, key] = make_certificate("backend.test");
    REQUIRE(asio::certificate_manager::instance().set_certificate("backend.test", certificate, key));

    test_server target;
    target.server.enable_ssl(true);
    add_inspect_handler(target.server);
    target.start();

    http::reverse_proxy reverse_proxy;
    http::proxy_target destination;
    destination.type = http::proxy_target::transport::tls;
    destination.host = "127.0.0.1";
    destination.port = target.port;
    destination.tls.server_name = "backend.test";
    test_server proxy;
    proxy.server.set_not_found_handler([&](http::request& req, http::response& res) -> awaitable<void> {
        co_await reverse_proxy.forward(req, res, destination);
    });

    SECTION("A private CA is not trusted by default") {
        proxy.start();
        REQUIRE(raw_request(proxy.port, get("/secure")).status == 502);
    }

    SECTION("A private CA is trusted when configured") {
        destination.tls.ca_pem = certificate;
        proxy.start();
        auto res = raw_request(proxy.port, get("/secure"));
        REQUIRE(res.status == 200);
        REQUIRE(res.json()["uri"] == "/secure");
        REQUIRE(res.json()["headers"]["x-forwarded-proto"] == "http");
    }

    SECTION("The certificate must be issued for the name of the target") {
        destination.tls.ca_pem = certificate;
        destination.tls.server_name = "other.test";
        proxy.start();
        REQUIRE(raw_request(proxy.port, get("/secure")).status == 502);
    }

    SECTION("Verification can be disabled") {
        destination.tls.verify = false;
        destination.tls.server_name.clear();
        proxy.start();
        REQUIRE(raw_request(proxy.port, get("/secure")).status == 200);
    }

    asio::certificate_manager::instance().remove_certificate("backend.test");
}

TEST_CASE("Reverse proxy removes hop-by-hop headers and adds forwarding headers", "[proxy][headers][integration]") {
    test_server target;
    add_inspect_handler(target.server);
    target.start();

    http::reverse_proxy reverse_proxy;
    auto destination = target_of(target.port);
    http::proxy_options options;
    test_server proxy;

    SECTION("Hop-by-hop headers of the request") {
        forward_all(proxy.server, reverse_proxy, destination);
        proxy.start();
        auto res = raw_request(proxy.port, get("/h",
            "Connection: keep-alive, X-Hop\r\nX-Hop: 1\r\nKeep-Alive: timeout=5\r\nTE: trailers\r\n"
            "Proxy-Authorization: Basic eDp5\r\nProxy-Connection: keep-alive\r\nUpgrade: other\r\n"
            "Trailer: X-T\r\nX-End: kept\r\n"));
        REQUIRE(res.status == 200);
        auto headers = res.json()["headers"];
        REQUIRE(headers["x-end"] == "kept");
        for (auto name : {"x-hop", "keep-alive", "te", "proxy-authorization", "proxy-connection", "upgrade", "trailer"}) {
            INFO(name);
            REQUIRE_FALSE(headers.contains(name));
        }
        REQUIRE(headers["connection"] == "close");
    }

    SECTION("Forwarding headers from an untrusted client are replaced") {
        forward_all(proxy.server, reverse_proxy, destination);
        proxy.start();
        auto headers = raw_request(proxy.port, get("/h",
            "X-Forwarded-For: 6.6.6.6\r\nX-Forwarded-Proto: https\r\nX-Forwarded-Host: evil.example\r\n"
            "Forwarded: for=6.6.6.6\r\n")).json()["headers"];
        REQUIRE(headers["x-forwarded-for"] == "127.0.0.1");
        REQUIRE(headers["x-forwarded-proto"] == "http");
        REQUIRE(headers["x-forwarded-host"] == "front.example");
        REQUIRE_FALSE(headers.contains("forwarded"));
    }

    SECTION("Forwarding headers from a trusted proxy are extended") {
        REQUIRE(proxy.server.set_trusted_proxies({"127.0.0.1"}));
        forward_all(proxy.server, reverse_proxy, destination);
        proxy.start();
        auto headers = raw_request(proxy.port, get("/h",
            "X-Forwarded-For: 6.6.6.6\r\nX-Forwarded-Proto: https\r\nX-Forwarded-Host: public.example\r\n"
            "Forwarded: for=7.7.7.7\r\n")).json()["headers"];
        REQUIRE(headers["x-forwarded-for"] == "6.6.6.6, 127.0.0.1");
        REQUIRE(headers["x-forwarded-proto"] == "https");
        REQUIRE(headers["x-forwarded-host"] == "public.example");
        // only the header the trusted proxy sets is trusted
        REQUIRE_FALSE(headers.contains("forwarded"));
    }

    SECTION("Forwarded header") {
        options.forwarding = http::forwarding_headers::forwarded;
        forward_all(proxy.server, reverse_proxy, destination, options);
        proxy.start();
        auto headers = raw_request(proxy.port,
            "GET /h HTTP/1.1\r\nHost: front.example:8080\r\nForwarded: for=6.6.6.6\r\nX-Forwarded-For: 6.6.6.6\r\n\r\n")
            .json()["headers"];
        REQUIRE(headers["forwarded"] == "for=127.0.0.1;proto=http;host=\"front.example:8080\"");
        REQUIRE_FALSE(headers.contains("x-forwarded-for"));
    }

    SECTION("No forwarding headers") {
        options.forwarding = http::forwarding_headers::none;
        forward_all(proxy.server, reverse_proxy, destination, options);
        proxy.start();
        auto headers = raw_request(proxy.port, get("/h", "X-Forwarded-For: 6.6.6.6\r\n")).json()["headers"];
        REQUIRE_FALSE(headers.contains("x-forwarded-for"));
        REQUIRE_FALSE(headers.contains("x-forwarded-proto"));
    }
}

TEST_CASE("Reverse proxy rewrites paths and hosts, and runs hooks", "[proxy][hooks][integration]") {
    test_server target;
    add_inspect_handler(target.server);
    target.start();

    http::reverse_proxy reverse_proxy;
    auto destination = http::proxy_target::from_url("http://127.0.0.1:" + std::to_string(target.port) + "/base/");
    REQUIRE(destination);
    shared_value<std::string> mode;
    test_server proxy;
    proxy.server.get("/api/:path(.*)", [&](http::request& req, http::response& res) -> awaitable<void> {
        http::proxy_options options;
        options.path = "/v1/" + req["path"];
        if (mode.get() == "preserve") options.preserve_host = true;
        if (mode.get() == "custom") options.host = "internal.example";
        options.on_request = [](http::request&, http::http_request& outgoing) {
            outgoing.remove_headers("Authorization");
            outgoing.add_header("Authorization", "Bearer inner");
        };
        options.on_response = [](http::request&, http::http_response& response) {
            response.add_header("X-Proxied", "yes");
            response.remove_headers("X-Internal");
        };
        co_await reverse_proxy.forward(req, res, *destination, options);
    });
    proxy.start();

    auto request = get("/api/items/7?x=1", "Authorization: Bearer outer\r\n");

    SECTION("Default host") {
        auto res = raw_request(proxy.port, request);
        REQUIRE(res.status == 200);
        REQUIRE(res.header("X-Proxied") == "yes");
        auto json = res.json();
        REQUIRE(json["uri"] == "/base/v1/items/7?x=1");
        REQUIRE(json["headers"]["authorization"] == "Bearer inner");
        REQUIRE(json["headers"]["host"] == "127.0.0.1:" + std::to_string(target.port));
    }

    SECTION("Preserved host") {
        mode.set("preserve");
        REQUIRE(raw_request(proxy.port, request).json()["headers"]["host"] == "front.example");
    }

    SECTION("Custom host") {
        mode.set("custom");
        REQUIRE(raw_request(proxy.port, request).json()["headers"]["host"] == "internal.example");
    }
}

TEST_CASE("Reverse proxy hooks express a device web service policy", "[proxy][hooks][integration]") {
    std::atomic<uint16_t> target_port{0};
    test_server target;
    target.server.get("/", [](http::response& res) {
        res.header("X-Frame-Options", "DENY");
        res.header("Content-Security-Policy", "default-src 'self'; frame-ancestors 'none'");
        res.header("Set-Cookie", "session=abc; Domain=device.local; Path=/; HttpOnly");
        res.header("Set-Cookie", "theme=dark; domain=device.local; SameSite=Lax");
        res.send("device page");
    });
    auto redirect = [&](const std::string& path, std::function<std::string()> location) {
        target.server.get(path, [location](http::response& res) { res.redirect(location()); });
    };
    redirect("/login", [] { return "/home"; });
    redirect("/absolute", [&] { return "http://127.0.0.1:" + std::to_string(target_port) + "/home?x=1"; });
    redirect("/external", [] { return "https://other.example/x"; });
    redirect("/inside", [] { return "/devices/d1/services/web/home"; });
    redirect("/userinfo", [&] { return "http://127.0.0.1:" + std::to_string(target_port) + "@evil.example/"; });
    target.start();
    target_port = target.port;

    // 0: no policy (the defaults of the proxy); 1: recommended; 2: frames from anywhere, cross-site cookies
    shared_value<int> mode;
    http::reverse_proxy reverse_proxy;
    auto destination = target_of(target.port);
    test_server proxy;
    proxy.server.get("/devices/:device/services/web/:path(.*)", [&](http::request& req, http::response& res) -> awaitable<void> {
        device_proxy::policy policy;
        policy.base_path = "/devices/" + req["device"] + "/services/web";
        int current = mode.get();
        if (current == 1) policy.console_origin = "https://console.example.com";
        if (current == 2) {
            policy.allow_any_frame = true;
            policy.cross_site_cookies = true;
        }
        http::proxy_options options;
        options.path = "/" + req["path"];
        if (current != 0) {
            options.on_response = [policy, destination](http::request& req, http::http_response& response) {
                device_proxy::apply(policy, {destination.authority(), req.header("Host")}, response);
            };
        }
        co_await reverse_proxy.forward(req, res, destination, options);
    });
    proxy.start();

    auto page = [&] { return raw_request(proxy.port, get("/devices/d1/services/web/")); };
    auto location = [&](const std::string& path) {
        auto res = raw_request(proxy.port, get("/devices/d1/services/web" + path));
        REQUIRE(res.status == 302);
        return res.header("Location");
    };

    SECTION("The proxy changes nothing by default") {
        auto res = page();
        REQUIRE(res.body == "device page");
        REQUIRE(res.header("X-Frame-Options") == "DENY");
        REQUIRE(res.header("Content-Security-Policy") == "default-src 'self'; frame-ancestors 'none'");
        REQUIRE(res.header("Set-Cookie").find("Domain=device.local") != std::string::npos);
        REQUIRE(location("/login") == "/home");
    }

    SECTION("Recommended: only the console may embed the device") {
        mode.set(1);
        auto res = page();
        REQUIRE_FALSE(res.has("X-Frame-Options"));
        REQUIRE(res.header("Content-Security-Policy") == "default-src 'self'; frame-ancestors https://console.example.com");
        REQUIRE(res.header("Set-Cookie") == "session=abc; Path=/; HttpOnly, theme=dark; SameSite=Lax");
        REQUIRE(location("/login") == "/devices/d1/services/web/home");
        REQUIRE(location("/absolute") == "/devices/d1/services/web/home?x=1");
        REQUIRE(location("/inside") == "/devices/d1/services/web/home");
        REQUIRE(location("/external") == "https://other.example/x");
        REQUIRE(location("/userinfo") == "http://127.0.0.1:" + std::to_string(target.port) + "@evil.example/");
    }

    SECTION("Frames from any site and cross-site cookies") {
        mode.set(2);
        auto res = page();
        REQUIRE_FALSE(res.has("X-Frame-Options"));
        REQUIRE_FALSE(res.has("Content-Security-Policy"));
        REQUIRE(res.header("Set-Cookie") ==
                "session=abc; Path=/; HttpOnly; Secure; SameSite=None, theme=dark; Secure; SameSite=None");
    }
}

TEST_CASE("Reverse proxy cleans the responses of the target", "[proxy][headers][integration]") {
    fake_target target([](const std::string& request) -> std::string {
        if (request.starts_with("HEAD ")) {
            return "HTTP/1.1 200 OK\r\nContent-Length: 1234\r\nContent-Type: text/plain\r\n\r\n";
        }
        if (request.starts_with("GET /204 ")) return "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n";
        if (request.starts_with("GET /304 ")) return "HTTP/1.1 304 Not Modified\r\nETag: \"v1\"\r\nContent-Length: 50\r\n\r\n";
        if (request.starts_with("GET /custom ")) return "HTTP/1.1 299 Custom Thing\r\nContent-Length: 2\r\n\r\nok";
        if (request.starts_with("GET /interim ")) {
            return "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n"
                   "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        }
        if (request.starts_with("GET /close ")) return "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nuntil the end";
        return "HTTP/1.1 200 OK\r\nConnection: close, X-Drop\r\nX-Drop: 1\r\nKeep-Alive: timeout=5\r\n"
               "Proxy-Authenticate: Basic\r\nTrailer: X-T\r\nX-Keep: 1\r\nContent-Length: 2\r\n\r\nok";
    });

    http::reverse_proxy reverse_proxy;
    test_server proxy;
    forward_all(proxy.server, reverse_proxy, target_of(target.port()));
    proxy.start();

    raw_connection connection(proxy.port);

    SECTION("Hop-by-hop headers") {
        connection.send(get("/"));
        auto res = connection.read_response();
        REQUIRE(res.status == 200);
        REQUIRE(res.body == "ok");
        REQUIRE(res.header("X-Keep") == "1");
        for (auto name : {"X-Drop", "Keep-Alive", "Proxy-Authenticate", "Trailer"}) {
            INFO(name);
            REQUIRE_FALSE(res.has(name));
        }
        // the connection with the client is not closed by the target's
        REQUIRE(lower(res.header("Connection")) == "keep-alive");
    }

    SECTION("Responses without body keep the connection in sync") {
        connection.send("HEAD /resource HTTP/1.1\r\nHost: x\r\n\r\n");
        auto res = connection.read_response(true);
        REQUIRE(res.status == 200);
        REQUIRE(res.header("Content-Length") == "1234");

        connection.send(get("/204"));
        res = connection.read_response();
        REQUIRE(res.status == 204);
        REQUIRE_FALSE(res.has("Content-Length"));
        REQUIRE_FALSE(res.has("Transfer-Encoding"));

        connection.send(get("/304"));
        res = connection.read_response();
        REQUIRE(res.status == 304);
        REQUIRE(res.header("ETag") == "\"v1\"");

        connection.send(get("/custom"));
        res = connection.read_response();
        REQUIRE(res.status_line == "HTTP/1.1 299 Custom Thing");
        REQUIRE(res.body == "ok");
        REQUIRE(connection.pending.empty());
    }

    SECTION("Interim responses are not relayed") {
        connection.send(get("/interim"));
        auto res = connection.read_response();
        REQUIRE(res.status == 200);
        REQUIRE(res.body == "ok");
    }

    SECTION("Body delimited by the end of the connection") {
        connection.send(get("/close"));
        auto res = connection.read_response();
        REQUIRE(res.status == 200);
        REQUIRE(res.header("Transfer-Encoding") == "chunked");
        REQUIRE(res.body == "until the end");
    }
}

TEST_CASE("Reverse proxy answers 502 when the target fails", "[proxy][errors][integration]") {
    http::reverse_proxy reverse_proxy;
    shared_value<http::proxy_target> destination;
    test_server proxy;
    proxy.server.set_not_found_handler([&](http::request& req, http::response& res) -> awaitable<void> {
        auto target = destination.get();
        co_await reverse_proxy.forward(req, res, target);
    });
    proxy.server.set_error_formatter([](const http::http_error& error, http::http_response& response) {
        response.set_content(nlohmann::json({{"error", error.status == http::http_response::status::bad_gateway
                                                           ? "bad gateway" : "other"}}).dump(), "application/json");
    });
    proxy.start();

    SECTION("Nothing listening") {
        uint16_t port;
        {
            boost::asio::io_context ioc;
            boost::asio::ip::tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
            port = acceptor.local_endpoint().port();
        }
        destination.set(target_of(port));
        auto res = raw_request(proxy.port, get("/"));
        REQUIRE(res.status == 502);
        REQUIRE(res.json()["error"] == "bad gateway");
    }

    SECTION("Missing Unix socket") {
        destination.set(http::proxy_target::unix_socket("/nonexistent/thinger.sock"));
        REQUIRE(raw_request(proxy.port, get("/")).status == 502);
    }

    SECTION("Unknown host") {
        destination.set(*http::proxy_target::from_url("http://nonexistent.invalid"));
        REQUIRE(raw_request(proxy.port, get("/")).status == 502);
    }

    SECTION("Malformed responses") {
        for (std::string response : {
                 "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n",
                 "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
                 "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
                 "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd",
                 "HTTP/1.1 200 OK\r\nContent-Length: 3abc\r\n\r\nabc",
                 "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
                 "HTTP/1.1 2000 OK\r\nContent-Length: 0\r\n\r\n",
                 "HTTP/1.1 20 OK\r\nContent-Length: 0\r\n\r\n",
                 "HTTP/2.0 200 OK\r\nContent-Length: 0\r\n\r\n",
                 "HTTP/1.1 200 OK\r\nX-Folded: a\r\n b\r\nContent-Length: 0\r\n\r\n",
                 "HTTP/1.1 200 OK\nContent-Length: 0\n\n",
                 "HTTP/1.1 101 Switching Protocols\r\nUpgrade: other\r\n\r\n",
                 "garbage\r\n\r\n",
                 ""}) {
            INFO(response);
            fake_target target([response](const std::string&) { return response; });
            destination.set(target_of(target.port()));
            auto res = raw_request(proxy.port, get("/"));
            REQUIRE(res.status == 502);
        }
    }

    SECTION("Broken bodies leave the response incomplete") {
        for (std::string response : {
                 "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\nzz\r\n",
                 "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n",
                 "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc"}) {
            INFO(response);
            fake_target target([response](const std::string&) { return response; });
            destination.set(target_of(target.port()));
            raw_connection connection(proxy.port);
            connection.send(get("/"));
            auto res = connection.read_response();
            REQUIRE(res.status == 200);
            REQUIRE_FALSE(res.complete);
            REQUIRE(connection.read_until_closed());
        }
    }

    SECTION("Data after the response is not relayed") {
        fake_target target([](const std::string&) {
            return "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"
                   "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nevil";
        });
        destination.set(target_of(target.port()));
        raw_connection connection(proxy.port);
        for (int i = 0; i < 2; ++i) {
            connection.send(get("/"));
            auto res = connection.read_response();
            REQUIRE(res.status == 200);
            REQUIRE(res.body == "ok");
        }
        REQUIRE_FALSE(connection.read_more(200ms));
        REQUIRE(connection.pending.empty());
    }
}

TEST_CASE("Reverse proxy answers 504 on timeouts", "[proxy][timeouts][integration]") {
    fake_target silent;

    // a target that sends its head and part of the body, then stays silent
    boost::asio::io_context stalled_ioc;
    boost::asio::ip::tcp::acceptor stalled(stalled_ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});

    http::proxy_target destination = target_of(silent.port());
    http::proxy_options options;
    options.connect_timeout = 300ms;
    options.idle_timeout = 300ms;

    SECTION("No response") {}

    SECTION("TLS handshake never answered") {
        destination.type = http::proxy_target::transport::tls;
        destination.tls.verify = false;
    }

    SECTION("Total timeout") {
        options.idle_timeout = 0ms;
        options.total_timeout = 300ms;
    }

    joiner stalled_target;
    bool stall = false;
    SECTION("Body stalled after the response started") {
        stall = true;
        destination = target_of(stalled.local_endpoint().port());
        stalled_target.thread = std::thread([&] {
            boost::asio::ip::tcp::socket socket(stalled_ioc);
            stalled.accept(socket);
            char buffer[4096];
            socket.read_some(boost::asio::buffer(buffer));
            boost::asio::write(socket, boost::asio::buffer(std::string("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nabc")));
            std::this_thread::sleep_for(1s);
        });
    }

    http::reverse_proxy reverse_proxy;
    test_server proxy;
    proxy.server.set_not_found_handler([&](http::request& req, http::response& res) -> awaitable<void> {
        co_await reverse_proxy.forward(req, res, destination, options);
    });
    proxy.start();

    auto start = std::chrono::steady_clock::now();
    raw_connection connection(proxy.port);
    connection.send(get("/"));
    auto res = connection.read_response();
    if (stall) {
        // the response started: the client sees it incomplete
        REQUIRE(res.status == 200);
        REQUIRE_FALSE(res.complete);
        REQUIRE(res.body == "abc");
    } else {
        REQUIRE(res.status == 504);
    }
    REQUIRE(std::chrono::steady_clock::now() - start < 3s);
}

TEST_CASE("Reverse proxy closes the target when the client leaves", "[proxy][integration]") {
    // a target streaming a chunked body until it cannot write any more
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    std::atomic<bool> target_closed{false};
    joiner streaming_target;
    streaming_target.thread = std::thread([&] {
        boost::asio::ip::tcp::socket socket(ioc);
        acceptor.accept(socket);
        char buffer[4096];
        socket.read_some(boost::asio::buffer(buffer));
        boost::system::error_code ec;
        boost::asio::write(socket, boost::asio::buffer(std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n")), ec);
        auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!ec && std::chrono::steady_clock::now() < deadline) {
            boost::asio::write(socket, boost::asio::buffer(std::string("5\r\nhello\r\n")), ec);
            std::this_thread::sleep_for(2ms);
        }
        target_closed = ec.failed();
    });

    http::reverse_proxy reverse_proxy;
    test_server proxy;
    forward_all(proxy.server, reverse_proxy, target_of(acceptor.local_endpoint().port()));
    proxy.start();

    {
        raw_connection connection(proxy.port);
        connection.send(get("/stream"));
        REQUIRE(connection.read_until("hello"));
    }
    REQUIRE(wait_for([&] { return target_closed.load(); }));
}

TEST_CASE("Reverse proxy closes the target when the client leaves a silent exchange", "[proxy][integration]") {
    // a target that reads the request and stays silent, telling when its connection closes
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    std::atomic<bool> request_received{false};
    std::atomic<bool> target_closed{false};
    joiner silent_target;
    silent_target.thread = std::thread([&] {
        boost::asio::ip::tcp::socket socket(ioc);
        acceptor.accept(socket);
        char buffer[4096];
        boost::system::error_code ec;
        socket.read_some(boost::asio::buffer(buffer), ec);
        request_received = true;
        // the next read ends when the proxy closes the connection
        socket.read_some(boost::asio::buffer(buffer), ec);
        target_closed = ec.failed();
    });

    // without timeouts, as for event streams that may stay silent
    http::reverse_proxy reverse_proxy({.idle_timeout = 0ms});
    test_server proxy;
    forward_all(proxy.server, reverse_proxy, target_of(acceptor.local_endpoint().port()));
    proxy.start();

    {
        raw_connection connection(proxy.port);
        connection.send(get("/events"));
        REQUIRE(wait_for([&] { return request_received.load(); }));
    }
    REQUIRE(wait_for([&] { return target_closed.load(); }));
}

TEST_CASE("Reverse proxy relays responses sent before the end of the request body", "[proxy][integration]") {
    // a target that answers once it has the head, without reading the body
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    std::atomic<bool> done{false};
    joiner early_target;
    early_target.thread = std::thread([&] {
        boost::asio::ip::tcp::socket socket(ioc);
        acceptor.accept(socket);
        std::string request;
        char buffer[4096];
        boost::system::error_code ec;
        while (!ec && request.find("\r\n\r\n") == std::string::npos) {
            auto bytes = socket.read_some(boost::asio::buffer(buffer), ec);
            request.append(buffer, bytes);
        }
        boost::asio::write(socket, boost::asio::buffer(std::string(
            "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 8\r\n\r\ntoo much")), ec);
        // the connection stays open, the body unread
        wait_for([&] { return done.load(); });
    });

    http::reverse_proxy reverse_proxy;
    test_server proxy;
    // (a body over the maximum body size would be rejected before reaching the target)
    proxy.server.set_max_body_size(200 * 1000 * 1000);
    forward_all(proxy.server, reverse_proxy, target_of(acceptor.local_endpoint().port()));
    proxy.start();

    raw_connection connection(proxy.port);
    connection.send("POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 100000000\r\n\r\n" + std::string(65536, 'a'));
    auto res = connection.read_response(false, 3s);
    done = true;
    REQUIRE(res.status == 413);
    REQUIRE(res.body == "too much");
    // the rest of the body is not read: the connection is closed
    REQUIRE(lower(res.header("Connection")) == "close");
    REQUIRE(connection.read_until_closed());
}

TEST_CASE("Reverse proxy never forwards ambiguous request framing", "[proxy][smuggling][integration]") {
    fake_target target([](const std::string&) { return std::string("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"); });

    http::reverse_proxy reverse_proxy;
    test_server proxy;
    forward_all(proxy.server, reverse_proxy, target_of(target.port()));
    proxy.start();

    SECTION("Requests the server rejects never reach the target") {
        for (std::string request : {
                 "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
                 "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked, chunked\r\n\r\n0\r\n\r\n",
                 "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n",
                 "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd",
                 "POST / HTTP/1.0\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
                 "GET / HTTP/1.1\r\nHost: x\r\nX-Folded: a\r\n b\r\n\r\n"}) {
            INFO(request);
            REQUIRE(raw_request(proxy.port, request).status == 400);
        }
        REQUIRE(target.count() == 0);
    }

    SECTION("Malformed chunked bodies are not completed for the target") {
        raw_connection connection(proxy.port);
        connection.send("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloXX0\r\n\r\n");
        auto res = connection.read_response();
        REQUIRE(res.status == 400);
        REQUIRE(target.count() == 0);
    }

    SECTION("Chunked bodies are framed again, without extensions and trailers") {
        auto res = raw_request(proxy.port,
            "POST /b HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
            "5;ext=\"x\"\r\nhello\r\n6\r\n world\r\n0\r\nX-Trailer: evil\r\n\r\n");
        REQUIRE(res.status == 200);
        REQUIRE(target.count() == 1);
        auto request = target.request(0);
        auto head = lower(request.substr(0, request.find("\r\n\r\n")));
        REQUIRE(head.find("transfer-encoding: chunked") != std::string::npos);
        REQUIRE(head.find("content-length") == std::string::npos);
        // the decoded body, chunked as the proxy read it, and ended with a plain last chunk
        auto body = request.substr(request.find("\r\n\r\n") + 4);
        std::string decoded;
        for (size_t position = 0;;) {
            auto line = body.find("\r\n", position);
            REQUIRE(line != std::string::npos);
            size_t size = std::stoul(body.substr(position, line - position), nullptr, 16);
            REQUIRE(body.substr(position, line - position).find_first_not_of("0123456789abcdef") == std::string::npos);
            if (size == 0) {
                REQUIRE(body.substr(line) == "\r\n\r\n");
                break;
            }
            decoded += body.substr(line + 2, size);
            REQUIRE(body.substr(line + 2 + size, 2) == "\r\n");
            position = line + 2 + size + 2;
        }
        REQUIRE(decoded == "hello world");
        REQUIRE(request.find("evil") == std::string::npos);
        REQUIRE(request.find("ext") == std::string::npos);
    }

    SECTION("A pipelined request is not part of the body") {
        raw_connection connection(proxy.port);
        connection.send("POST /c HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello"
                        "GET /d HTTP/1.1\r\nHost: x\r\n\r\n");
        REQUIRE(connection.read_response().status == 200);
        REQUIRE(connection.read_response().status == 200);
        REQUIRE(target.count() == 2);
        auto first = target.request(0);
        REQUIRE(first.starts_with("POST /c HTTP/1.1\r\n"));
        REQUIRE(lower(first).find("content-length: 5\r\n") != std::string::npos);
        REQUIRE(first.ends_with("\r\n\r\nhello"));
        REQUIRE(target.request(1).starts_with("GET /d HTTP/1.1\r\n"));
    }

    SECTION("Hooks cannot inject headers") {
        test_server other;
        http::proxy_options options;
        options.on_request = [](http::request&, http::http_request& outgoing) {
            outgoing.add_header("X-Injected", "a\r\nContent-Length: 0");
        };
        forward_all(other.server, reverse_proxy, target_of(target.port()), options);
        other.start();
        REQUIRE(raw_request(other.port, get("/")).status == 500);
        REQUIRE(target.count() == 0);
    }
}

TEST_CASE("Reverse proxy limits request bodies", "[proxy][integration]") {
    fake_target target([](const std::string&) { return std::string("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"); });
    http::reverse_proxy reverse_proxy({.max_body_size = 10});
    test_server proxy;
    forward_all(proxy.server, reverse_proxy, target_of(target.port()));
    proxy.start();

    SECTION("Content-Length over the limit") {
        auto res = raw_request(proxy.port, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 20\r\n\r\n" + std::string(20, 'a'));
        REQUIRE(res.status == 413);
        REQUIRE(target.count() == 0);
    }

    SECTION("Chunked body over the limit") {
        auto res = raw_request(proxy.port, "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n14\r\n" +
                                           std::string(20, 'a') + "\r\n0\r\n\r\n");
        REQUIRE(res.status == 413);
    }

    SECTION("Within the limit") {
        auto res = raw_request(proxy.port, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello");
        REQUIRE(res.status == 200);
    }
}

TEST_CASE("Reverse proxy serves requests dispatched in memory", "[proxy][memory][integration]") {
    test_server target;
    add_inspect_handler(target.server);
    target.start();

    http::server application;
    http::reverse_proxy reverse_proxy;
    forward_all(application, reverse_proxy, target_of(target.port));

    auto request = http::http_request::create_http_request(http::method::POST, "http://localhost/memory");
    request->set_content("hello", "text/plain");
    boost::asio::io_context ioc;
    auto result = co_spawn(ioc, application.dispatch(request, {.remote_ip = "10.1.2.3"}), boost::asio::use_future);
    ioc.run();
    auto response = result.get();
    REQUIRE(response->get_status_code() == 200);
    auto json = nlohmann::json::parse(response->get_content());
    REQUIRE(json["uri"] == "/memory");
    REQUIRE(json["body"] == "hello");
    REQUIRE(json["headers"]["x-forwarded-for"] == "10.1.2.3");
}

TEST_CASE("Streamed responses check their framing", "[server][stream][integration]") {
    // checks made by the handlers (on the server thread): how many passed
    std::atomic<int> passed{0};
    test_server server;
    server.server.get("/short", [&passed](http::response& res) -> awaitable<void> {
        http::http_response head;
        if (res.start_stream(head, 10)) passed++;
        if (co_await res.write_stream("abc")) passed++;
        // more than announced is never written
        if (!co_await res.write_stream(std::string(10, 'x'))) passed++;
        // fewer than announced: the connection is closed
        if (!co_await res.end_stream()) passed++;
    });
    server.server.get("/ok", [&passed](http::response& res) -> awaitable<void> {
        http::http_response head;
        head.set_status(static_cast<uint16_t>(201));
        head.add_header("X-Stream", "1");
        head.add_header("Content-Length", "999");
        if (res.start_stream(head)) passed++;
        if (co_await res.write_stream("hello ")) passed++;
        if (co_await res.write_stream("")) passed++;
        if (co_await res.write_stream("world")) passed++;
        if (co_await res.end_stream()) passed++;
    });
    server.start();

    SECTION("Shorter than its Content-Length") {
        raw_connection connection(server.port);
        connection.send(get("/short"));
        auto res = connection.read_response();
        REQUIRE(res.header("Content-Length") == "10");
        REQUIRE_FALSE(res.complete);
        REQUIRE(res.body == "abc");
        REQUIRE(wait_for([&] { return passed == 4; }));
    }

    SECTION("Chunked") {
        raw_connection connection(server.port);
        connection.send(get("/ok"));
        auto res = connection.read_response();
        REQUIRE(res.status == 201);
        REQUIRE(res.complete);
        REQUIRE(res.header("X-Stream") == "1");
        REQUIRE_FALSE(res.has("Content-Length"));
        REQUIRE(res.body == "hello world");
        // and the connection is still usable
        connection.send(get("/ok"));
        REQUIRE(connection.read_response().body == "hello world");
        REQUIRE(wait_for([&] { return passed == 10; }));
    }
}

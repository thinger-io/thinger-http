#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/websocket_connection.hpp>
#include <thinger/asio/ssl/certificate_manager.hpp>
#include <thinger/util/ascii.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_future.hpp>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../fixtures/listener_helpers.hpp"
#include "../fixtures/server_listen.hpp"
#include "../fixtures/test_ca.hpp"

using namespace thinger;
using namespace std::chrono_literals;
using http::test::test_ca;
using http::test::tls_client;
using http::test::tls_client_options;

// Requests whose Host gets another TLS configuration (certificate entry, client certificate
// mode) than the handshake of their connection chose by its SNI: 421 Misdirected Request
// before anything of their host runs (US1), and the traffic served as before (US2): the same
// name, the same configuration, no SNI with the listener's mode, plain connections, Unix
// sockets and in-memory dispatch

namespace {

    struct reply {
        int status = 0;
        std::string head;
        std::string body;
    };

    // A keep-alive HTTPS connection to localhost, with the SNI, the client certificate and the
    // requests of the test
    struct https_connection {
        tls_client client;
        boost::asio::streambuf buffer;
        bool open = false;

        https_connection(uint16_t port, tls_client_options options) {
            // An HTTPS server says nothing before the request
            options.greeting.clear();
            open = client.connect(port, options);
        }

        // Send `request` and read its response (status 0 if none)
        reply send(const std::string& request) {
            reply result;
            if (!client.write(request)) return result;
            boost::system::error_code ec;
            auto end = boost::asio::read_until(*client.stream, buffer, "\r\n\r\n", ec);
            if (ec) return result;
            auto data = buffer.data();
            result.head.assign(boost::asio::buffers_begin(data), boost::asio::buffers_begin(data) + end);
            buffer.consume(end);
            result.status = std::stoi(result.head.substr(9, 3));
            auto head = result.head;
            util::ascii::to_lower(head);
            size_t length = 0;
            if (auto at = head.find("\r\ncontent-length:"); at != std::string::npos) length = std::stoul(head.substr(at + 17));
            if (buffer.size() < length) {
                boost::asio::read(*client.stream, buffer, boost::asio::transfer_exactly(length - buffer.size()), ec);
            }
            data = buffer.data();
            result.body.assign(boost::asio::buffers_begin(data),
                               boost::asio::buffers_begin(data) + std::min(length, buffer.size()));
            buffer.consume(result.body.size());
            return result;
        }

        reply get(const std::string& host, const std::string& path = "/", const std::string& headers = "") {
            return send("GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\n" + headers + "\r\n");
        }

        // Whether the server closed the connection within `timeout`, sending nothing more
        bool closed_within(std::chrono::milliseconds timeout) {
            if (buffer.size() > 0) return false;
            pollfd descriptor{client.stream->next_layer().native_handle(), POLLIN, 0};
            if (::poll(&descriptor, 1, static_cast<int>(timeout.count())) <= 0) return false;
            char byte;
            boost::system::error_code ec;
            client.stream->read_some(boost::asio::buffer(&byte, 1), ec);
            return static_cast<bool>(ec);
        }
    };

    tls_client_options sni(const std::string& name) {
        tls_client_options options;
        options.server_name = name;
        return options;
    }

    // The status of a request with `Host: host` on a new plain connection (TCP or Unix)
    template<typename Protocol>
    int plain_status(const typename Protocol::endpoint& endpoint, const std::string& host) {
        boost::asio::io_context io_context;
        typename Protocol::socket sock(io_context);
        sock.connect(endpoint);
        boost::asio::write(sock, boost::asio::buffer("GET / HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n"));
        boost::system::error_code ec;
        std::string data;
        boost::asio::read(sock, boost::asio::dynamic_buffer(data), ec);
        return data.size() > 12 ? std::stoi(data.substr(9, 3)) : 0;
    }

    // The server of the scenarios, its certificates in one manager:
    //  - a default certificate;
    //  - devices.example.com, requiring a client certificate (deferred validation);
    //  - *.devices.example.com, requiring one, without a certificate of its own;
    //  - *.example.com and api.example.com, with no mode of their own;
    // on a TLS listener asking for client certificates in `listener_mode`, and the same
    // application on plain TCP and on a Unix socket. A virtual host devices.example.com and
    // the default host; every route and a global middleware count their calls, and the
    // response hook keeps the statuses
    struct misdirect_server {
        test_ca ca;
        std::shared_ptr<asio::certificate_manager> manager = std::make_shared<asio::certificate_manager>();
        std::pair<std::string, std::string> client_certificate = ca.issue_client("device");
        http::server server;
        std::thread thread;
        uint16_t tls_port = 0;
        uint16_t tcp_port = 0;
        std::string unix_path;

        std::atomic<int> handled{0};
        std::atomic<int> middleware{0};
        std::atomic<int> upgraded{0};
        std::mutex mutex;
        std::vector<int> statuses;

        explicit misdirect_server(asio::client_verify listener_mode = asio::client_verify::none,
                                  bool error_formatter = false) {
            auto set = [this](const std::string& name) {
                auto certificate = ca.issue(name);
                REQUIRE_FALSE(manager->set_certificate(name, certificate.first, certificate.second));
            };
            auto certificate = ca.issue("default.example.net");
            REQUIRE_FALSE(manager->set_default_certificate(certificate.first, certificate.second));
            set("devices.example.com");
            set("*.example.com");
            set("api.example.com");
            REQUIRE_FALSE(manager->set_client_verify("devices.example.com", asio::client_verify::required));
            REQUIRE_FALSE(manager->set_client_verify("*.devices.example.com", asio::client_verify::required));

            server.use([this](http::request&, http::response&, std::function<void()> next) {
                ++middleware;
                next();
            });
            server.on_response([this](const http::request&, const http::response_info& info) {
                std::scoped_lock lock(mutex);
                statuses.push_back(static_cast<int>(info.status));
            });
            if (error_formatter) {
                server.set_error_formatter([](const http::http_error& error, http::http_response& response) {
                    response.set_content("formatted " + std::to_string(static_cast<int>(error.status)) + " " +
                                         error.message, "text/plain");
                });
            }
            server.get("/", [this](http::response& res) {
                ++handled;
                res.send("default");
            });
            auto& devices = server.host("devices.example.com");
            devices.get("/", [this](http::response& res) {
                ++handled;
                res.send("devices");
            });
            devices.post("/", [this](http::request& req, http::response& res) {
                ++handled;
                res.send(req.body());
            });
            devices.get("/ws", [this](http::response& res) {
                ++handled;
                res.upgrade_websocket({}, [this](std::shared_ptr<http::websocket_connection>) { ++upgraded; });
            });

            auto options = http::test::listen_options("127.0.0.1", 0, manager);
            options.client_verify.mode = listener_mode;
            REQUIRE_FALSE(server.listen(std::move(options)));
            tls_port = http::test::listening_port(server);
            REQUIRE_FALSE(server.listen("127.0.0.1", 0));
            tcp_port = http::test::listening_port(server);
            unix_path = (std::filesystem::temp_directory_path() /
                         ("thinger_misdirected_" + std::to_string(getpid()) + ".sock")).string();
            REQUIRE_FALSE(server.listen(http::test::unix_listen_options(unix_path)));
            thread = std::thread([this] { server.wait(); });
        }

        ~misdirect_server() {
            server.stop();
            if (thread.joinable()) thread.join();
        }

        misdirect_server(const misdirect_server&) = delete;
        misdirect_server& operator=(const misdirect_server&) = delete;

        // A connection with SNI `name` (none if empty), with the client certificate or not
        https_connection connect(const std::string& name, bool certificate = false) {
            auto options = sni(name);
            if (certificate) {
                options.certificate = client_certificate.first;
                options.key = client_certificate.second;
            }
            return https_connection(tls_port, options);
        }

        // Whether the response hook saw `status` (written by the server thread, maybe after
        // the client read the response)
        bool hook_saw(int status) {
            for (auto deadline = std::chrono::steady_clock::now() + 2s; std::chrono::steady_clock::now() < deadline;) {
                {
                    std::scoped_lock lock(mutex);
                    if (std::find(statuses.begin(), statuses.end(), status) != statuses.end()) return true;
                }
                std::this_thread::sleep_for(1ms);
            }
            return false;
        }
    };

    // The status of a mismatching request on a new connection with SNI `name`, the client
    // certificate presented or not; none of the host must have run
    int misdirected_status(misdirect_server& s, const std::string& name, const std::string& host,
                           bool certificate = false) {
        auto connection = s.connect(name, certificate);
        REQUIRE(connection.open);
        return connection.get(host).status;
    }

}

TEST_CASE("Misdirected requests: a per-host mode cannot be bypassed through Host", "[tls][mtls][sni][integration]") {
    misdirect_server s;
    // Requests served by the section (the others are all answered 421)
    int served = 0;

    SECTION("US1 1: another SNI, Host of a host that requires a certificate") {
        REQUIRE(misdirected_status(s, "www.example.com", "devices.example.com") == 421);
    }

    SECTION("US1 2: no SNI") {
        REQUIRE(misdirected_status(s, "", "devices.example.com") == 421);
    }

    SECTION("US1 3: the Host normalized before the comparison") {
        REQUIRE(misdirected_status(s, "www.example.com", "DEVICES.Example.com:443") == 421);
    }

    SECTION("US1 4: a mode applied that the Host does not have, the other direction") {
        REQUIRE(misdirected_status(s, "devices.example.com", "www.example.com", true) == 421);
    }

    SECTION("US1 6: a wildcard mode") {
        REQUIRE(misdirected_status(s, "www.example.com", "a.devices.example.com") == 421);
    }

    SECTION("US1 7: the connection serves the next request") {
        auto connection = s.connect("www.example.com");
        REQUIRE(connection.open);
        REQUIRE(connection.get("devices.example.com").status == 421);
        auto next = connection.get("www.example.com");
        REQUIRE(next.status == 200);
        REQUIRE(next.body == "default");
        served = 1;
    }

    SECTION("US1 8: a body is not read, and the connection closes") {
        auto connection = s.connect("www.example.com");
        REQUIRE(connection.open);
        auto answer = connection.send("POST / HTTP/1.1\r\nHost: devices.example.com\r\nContent-Length: 10\r\n\r\n0123456789");
        REQUIRE(answer.status == 421);
        REQUIRE(connection.closed_within(5s));
    }

    SECTION("US1 9: an upgrade is not made") {
        auto connection = s.connect("www.example.com");
        REQUIRE(connection.open);
        auto answer = connection.get("devices.example.com", "/ws",
                                     "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n");
        REQUIRE(answer.status == 421);
        REQUIRE(s.upgraded == 0);
    }

    SECTION("The response hook sees the 421") {
        REQUIRE(misdirected_status(s, "www.example.com", "devices.example.com") == 421);
        REQUIRE(s.hook_saw(421));
    }

    // Neither a handler nor the middleware ran for a 421
    REQUIRE(s.handled == served);
    REQUIRE(s.middleware == served);
}

TEST_CASE("Misdirected requests: logged at debug level with both names", "[tls][mtls][sni][integration]") {
    // Captured before the server starts and after it stops: its threads log until then
    http::test::log_capture logs;
    misdirect_server s;

    // US1 1
    REQUIRE(misdirected_status(s, "www.example.com", "devices.example.com") == 421);
    REQUIRE(logs.logged("debug", "misdirected request (421): host 'devices.example.com' on a TLS connection for 'www.example.com'"));
    REQUIRE(logs.count("error", "misdirected") == 0);
    REQUIRE(logs.count("warning", "misdirected") == 0);
    REQUIRE(s.handled == 0);
    REQUIRE(s.middleware == 0);
}

TEST_CASE("Misdirected requests: a listener that requires certificates", "[tls][mtls][sni][integration]") {
    misdirect_server s(asio::client_verify::required);
    REQUIRE_FALSE(s.manager->set_client_verify("public.example.com", asio::client_verify::none));

    SECTION("US1 5: the Host falls back to the listener's mode") {
        REQUIRE(misdirected_status(s, "public.example.com", "api.example.com") == 421);
        REQUIRE(s.handled == 0);
        REQUIRE(s.middleware == 0);
    }

    SECTION("The host of the SNI is served") {
        auto connection = s.connect("public.example.com");
        REQUIRE(connection.open);
        REQUIRE(connection.get("public.example.com").status == 200);
    }
}

TEST_CASE("Misdirected requests: the error formatter writes the 421", "[tls][sni][integration]") {
    misdirect_server s(asio::client_verify::none, true);

    // US1 10
    auto connection = s.connect("www.example.com");
    REQUIRE(connection.open);
    auto answer = connection.get("devices.example.com");
    REQUIRE(answer.status == 421);
    REQUIRE(answer.head.starts_with("HTTP/1.1 421 Misdirected Request\r\n"));
    REQUIRE(answer.body == "formatted 421 Misdirected Request");
    REQUIRE(s.handled == 0);
    REQUIRE(s.middleware == 0);
}

TEST_CASE("Misdirected requests: legitimate traffic is served", "[tls][mtls][sni][integration]") {
    misdirect_server s;

    SECTION("US2 1: the same name, with a certificate") {
        auto connection = s.connect("devices.example.com", true);
        REQUIRE(connection.open);
        auto answer = connection.get("devices.example.com");
        REQUIRE(answer.status == 200);
        REQUIRE(answer.body == "devices");
    }

    SECTION("US2 2: case and trailing dot") {
        auto connection = s.connect("DEVICES.EXAMPLE.COM", true);
        REQUIRE(connection.open);
        auto answer = connection.get("devices.example.com.");
        REQUIRE(answer.status == 200);
        REQUIRE(answer.body == "devices");
    }

    SECTION("US2 3: two names of one wildcard") {
        auto connection = s.connect("a.example.com");
        REQUIRE(connection.open);
        REQUIRE(connection.get("b.example.com").status == 200);
    }

    SECTION("US2 4: two names of the default certificate") {
        auto connection = s.connect("one.example.org");
        REQUIRE(connection.open);
        REQUIRE(connection.get("two.example.org").status == 200);
    }

    SECTION("US2 5: no SNI, a host with a certificate of its own and the listener's mode") {
        auto connection = s.connect("");
        REQUIRE(connection.open);
        REQUIRE(connection.get("api.example.com").status == 200);
    }

    SECTION("US2 6: SNI of the default certificate, Host with one of its own") {
        REQUIRE(misdirected_status(s, "www.other.org", "api.example.com") == 421);
        REQUIRE(s.handled == 0);
        REQUIRE(s.middleware == 0);
    }

    SECTION("US2 7: plain TCP and Unix sockets are not checked") {
        using boost::asio::ip::tcp;
        using boost::asio::local::stream_protocol;
        REQUIRE(plain_status<tcp>({boost::asio::ip::make_address("127.0.0.1"), s.tcp_port}, "devices.example.com") == 200);
        REQUIRE(plain_status<stream_protocol>(stream_protocol::endpoint(s.unix_path), "devices.example.com") == 200);
    }

    SECTION("US2 8: in-memory dispatch is not checked") {
        boost::asio::io_context io_context;
        auto request = http::http_request::create_http_request(http::method::GET, "https://devices.example.com/");
        auto result = boost::asio::co_spawn(io_context, s.server.dispatch(request), boost::asio::use_future);
        io_context.run();
        auto response = result.get();
        REQUIRE(response->get_status_code() == 200);
        REQUIRE(response->get_content() == "devices");
    }

    SECTION("US2 9: replacing certificates makes no open connection misdirected") {
        auto same = s.connect("api.example.com");
        auto wildcard = s.connect("a.example.com");
        auto fallback = s.connect("one.example.org");
        REQUIRE((same.open && wildcard.open && fallback.open));
        REQUIRE(same.get("api.example.com").status == 200);
        REQUIRE(wildcard.get("b.example.com").status == 200);
        REQUIRE(fallback.get("two.example.org").status == 200);

        for (std::string name : {"api.example.com", "*.example.com"}) {
            auto certificate = s.ca.issue(name);
            REQUIRE_FALSE(s.manager->set_certificate(name, certificate.first, certificate.second));
        }
        auto certificate = s.ca.issue("default.example.net");
        REQUIRE_FALSE(s.manager->set_default_certificate(certificate.first, certificate.second));

        REQUIRE(same.get("api.example.com").status == 200);
        REQUIRE(wildcard.get("b.example.com").status == 200);
        REQUIRE(fallback.get("two.example.org").status == 200);
    }

    REQUIRE(s.middleware == s.handled);
}

TEST_CASE("Misdirected requests: no Host, and IP literals", "[tls][mtls][sni][integration]") {
    misdirect_server s;

    SECTION("No Host: the default entry and the listener's mode") {
        auto devices = s.connect("devices.example.com", true);
        REQUIRE(devices.open);
        REQUIRE(devices.send("GET / HTTP/1.0\r\n\r\n").status == 421);
        auto none = s.connect("");
        REQUIRE(none.open);
        REQUIRE(none.send("GET / HTTP/1.0\r\n\r\n").status == 200);
    }

    SECTION("An IP literal: as a name without an entry") {
        auto none = s.connect("");
        REQUIRE(none.open);
        REQUIRE(none.get("127.0.0.1:" + std::to_string(s.tls_port)).status == 200);
        REQUIRE(none.get("[::1]:8443").status == 200);
        REQUIRE(misdirected_status(s, "devices.example.com", "10.0.0.1", true) == 421);
    }
}

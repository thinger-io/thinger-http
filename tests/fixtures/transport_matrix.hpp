#ifndef THINGER_HTTP_TEST_TRANSPORT_MATRIX_HPP
#define THINGER_HTTP_TEST_TRANSPORT_MATRIX_HPP

#include <thinger/http/server/server_standalone.hpp>
#include <thinger/asio/ssl/certificate_manager.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <openssl/ssl.h>
#include <stdlib.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include "server_listen.hpp"
#include "test_ca.hpp"

// The transport matrix (target design §7.1): one test body run on TCP, TLS and a Unix socket.
// A server listens with a transport_listener, clients reach it through its url() and
// configure(), and raw bytes go through a raw_connection. MATRIX_TRANSPORT(kind) runs the rest
// of a TEST_CASE once per transport, naming it in the failure messages.
namespace thinger::http::test {

    using namespace std::chrono_literals;

    enum class transport { tcp, tls, unix };

    inline const char* to_string(transport kind) {
        switch (kind) {
            case transport::tcp: return "tcp";
            case transport::tls: return "tls";
            case transport::unix: return "unix";
        }
        return "?";
    }

    inline std::ostream& operator<<(std::ostream& out, transport kind) { return out << to_string(kind); }

    // Stopping a Unix listener unlinks its socket file, synchronously and on the way to the wait
    // of a shutdown. On a disk that other builds are writing to, that alone can stall for hundreds
    // of milliseconds, so a test timing a window that includes it allows this much more, on that
    // transport only. It is the margin of the test and changes nothing in the library; the sockets
    // of these tests are in memory where they can be (temporary_directory), which is what removes
    // the stall on Linux
    inline constexpr auto unix_unlink_margin = 100ms;

    constexpr std::chrono::milliseconds stop_margin(transport kind) {
        return kind == transport::unix ? unix_unlink_margin : 0ms;
    }

    // The CA that issues the certificates of the TLS listeners, one per process
    inline const test_ca& matrix_ca() {
        static const test_ca ca{"Thinger Matrix CA"};
        return ca;
    }

    // Where a server listens and how a client reaches it. Over TLS the name is localhost (the
    // name of the certificate, sent as SNI) on 127.0.0.1; over a Unix socket the host is a
    // placeholder, localhost
    struct transport_endpoint {
        transport kind = transport::tcp;
        uint16_t port = 0;
        std::string unix_path;

        std::string scheme() const { return kind == transport::tls ? "https" : "http"; }

        std::string host() const { return kind == transport::tcp ? "127.0.0.1" : "localhost"; }

        // The Host header a raw request sends
        std::string host_header() const {
            return kind == transport::unix ? host() : host() + ":" + std::to_string(port);
        }

        std::string base_url() const { return scheme() + "://" + host_header(); }

        std::string url(const std::string& path = "/") const { return base_url() + path; }

        // Point a client (http::client, http::async_client) at this endpoint: the CA of the
        // matrix over TLS (the server is verified), the socket path over Unix
        template<typename Client>
        Client& configure(Client& client) const {
            if (kind == transport::tls) client.add_ca_pem(matrix_ca().pem());
            if (kind == transport::unix) client.unix_socket(unix_path);
            return client;
        }
    };

    // The certificates of a TLS listener, a manager of its own: the certificate of `host` (the
    // name clients send as SNI), issued by the matrix CA, as its default (no self-signed one is
    // generated). Only as the default: an entry of its own would make the requests whose Host
    // is another name (an address, a placeholder) misdirected (421). Nothing is shared between
    // listeners (a proxy and its target)
    inline std::shared_ptr<asio::certificate_manager> matrix_certificates(const std::string& host = "localhost") {
        auto manager = std::make_shared<asio::certificate_manager>();
        auto [certificate, key] = matrix_ca().issue(host);
        REQUIRE_FALSE(manager->set_default_certificate(certificate, key));
        return manager;
    }

    // A directory of its own, removed with everything in it when destroyed. On Linux it is in
    // /dev/shm if that can be written, so unlinking a socket at shutdown never waits for a busy
    // disk (keep large files out: /dev/shm is 64 MiB in a container). Otherwise it is under the
    // temporary directory, with a short name: a socket path must fit in sun_path (104 bytes on
    // macOS)
    class temporary_directory {
    public:
        temporary_directory() {
#if defined(__linux__)
            std::string in_memory = "/dev/shm/thttp-XXXXXX";
            if (::mkdtemp(in_memory.data()) != nullptr) {
                path_ = in_memory;
                return;
            }
#endif
            auto base = std::filesystem::temp_directory_path();
            if (base.string().size() > 60) base = "/tmp";
            std::string pattern = (base / "thttp-XXXXXX").string();
            REQUIRE(::mkdtemp(pattern.data()) != nullptr);
            path_ = pattern;
        }

        ~temporary_directory() {
            std::error_code ec;
            std::filesystem::remove_all(path_, ec);
        }

        temporary_directory(const temporary_directory&) = delete;
        temporary_directory& operator=(const temporary_directory&) = delete;

        const std::filesystem::path& path() const { return path_; }

    private:
        std::filesystem::path path_;
    };

    // `server` (its routes registered beforehand) listening on `kind`: 127.0.0.1 and an
    // ephemeral port over TCP and TLS (with the certificates of matrix_certificates()), a socket
    // in a temporary directory over Unix. The directory lasts as long as the listener: destroy
    // it after the server stops (declare it after the server, or let running_server own it)
    class transport_listener {
    public:
        transport_listener(http::server_base& server, transport kind) {
            endpoint_.kind = kind;
            asio::listener_options options;
            if (kind == transport::unix) {
                directory_ = std::make_unique<temporary_directory>();
                endpoint_.unix_path = (directory_->path() / "s.sock").string();
                options = unix_listen_options(endpoint_.unix_path);
            } else {
                if (kind == transport::tls) certificates_ = matrix_certificates();
                options = listen_options("127.0.0.1", 0, certificates_);
            }
            auto ec = server.listen(std::move(options));
            INFO("listen: " << ec.message());
            REQUIRE_FALSE(ec);
            if (kind != transport::unix) endpoint_.port = listening_port(server);
        }

        const transport_endpoint& endpoint() const { return endpoint_; }

        // The certificates the server listens with over TLS (none on the other transports)
        const std::shared_ptr<asio::certificate_manager>& certificates() const { return certificates_; }

    private:
        std::shared_ptr<asio::certificate_manager> certificates_;
        std::unique_ptr<temporary_directory> directory_;
        transport_endpoint endpoint_;
    };

    // What a raw exchange read
    struct exchange_result {
        std::string data;
        bool closed = false;    // the peer closed the connection (or it failed)
    };

    // A client connection that writes and reads raw bytes over any transport, never waiting
    // longer than a timeout. Over TLS the handshake is done (verifying the server against the
    // matrix CA) before any byte is written; the end of the connection is any read error: a FIN,
    // a reset or a TLS close_notify (with or without the TCP close after it). Over TCP and TLS
    // a `receive_buffer` (bytes, 0: the system's) is set before connecting, so it bounds the
    // window the server may fill however the system tunes it
    class raw_connection {
    public:
        explicit raw_connection(const transport_endpoint& to, bool no_delay = false,
                                std::chrono::milliseconds timeout = 10s, int receive_buffer = 0)
            : kind_(to.kind) {
            namespace net = boost::asio;
            boost::system::error_code ec;
            if (kind_ == transport::unix) {
                auto& sock = stream_.emplace<unix_stream>(ioc_);
                sock.connect(net::local::stream_protocol::endpoint(to.unix_path), ec);
                REQUIRE_FALSE(ec);
                return;
            }
            net::ip::tcp::endpoint address{net::ip::make_address("127.0.0.1"), to.port};
            auto connect = [&](auto& sock) {
                sock.open(net::ip::tcp::v4());
                if (receive_buffer > 0) sock.set_option(net::socket_base::receive_buffer_size(receive_buffer));
                sock.connect(address, ec);
                REQUIRE_FALSE(ec);
                if (no_delay) sock.set_option(net::ip::tcp::no_delay(true));
            };
            if (kind_ == transport::tcp) {
                connect(stream_.emplace<tcp_stream>(ioc_));
                return;
            }
            context_ = std::make_unique<net::ssl::context>(net::ssl::context::tls_client);
            auto ca = matrix_ca().pem();
            context_->add_certificate_authority(net::buffer(ca));
            context_->set_verify_mode(net::ssl::verify_peer);
            auto& stream = stream_.emplace<tls_stream>(ioc_, *context_);
            stream.set_verify_callback(net::ssl::host_name_verification(to.host()));
            SSL_set_tlsext_host_name(stream.native_handle(), to.host().c_str());
            connect(stream.lowest_layer());
            bool done = false;
            stream.async_handshake(net::ssl::stream_base::client, [&](boost::system::error_code error) {
                ec = error;
                done = true;
            });
            ioc_.run_for(timeout);
            if (!done) {
                cancel();
                ioc_.restart();
                ioc_.run();
            }
            INFO("TLS handshake: " << ec.message());
            REQUIRE(done);
            REQUIRE_FALSE(ec);
        }

        raw_connection(const raw_connection&) = delete;
        raw_connection& operator=(const raw_connection&) = delete;

        transport kind() const { return kind_; }

        boost::asio::io_context& io_context() { return ioc_; }

        // Call `function` with the stream of the connection (a TCP socket, a TLS stream or a
        // Unix socket), for a test that drives it itself on io_context()
        template<typename Function>
        void visit_stream(Function&& function) {
            std::visit([&](auto& stream) {
                if constexpr (!std::is_same_v<std::decay_t<decltype(stream)>, std::monostate>) function(stream);
            }, stream_);
        }

        // Write all of `data`: false if the connection failed
        bool write(std::string_view data, std::chrono::milliseconds timeout = 10s) {
            boost::system::error_code ec;
            bool done = false;
            std::visit([&](auto& stream) {
                if constexpr (!std::is_same_v<std::decay_t<decltype(stream)>, std::monostate>) {
                    boost::asio::async_write(stream, boost::asio::buffer(data.data(), data.size()),
                                             [&](boost::system::error_code error, size_t) {
                                                 ec = error;
                                                 done = true;
                                             });
                }
            }, stream_);
            run(done, timeout);
            return done && !ec;
        }

        // Read what arrives within the timeout (at most one read of up to `most` bytes),
        // appending it to `data`. Returns the bytes read; eof() tells whether the connection ended
        size_t read_some(std::string& data, std::chrono::milliseconds timeout, size_t most = SIZE_MAX) {
            if (eof_ || timeout <= 0ms || most == 0) return 0;
            boost::system::error_code ec;
            size_t bytes = 0;
            bool done = false;
            std::visit([&](auto& stream) {
                if constexpr (!std::is_same_v<std::decay_t<decltype(stream)>, std::monostate>) {
                    stream.async_read_some(boost::asio::buffer(chunk_, most), [&](boost::system::error_code error, size_t n) {
                        ec = error;
                        bytes = n;
                        done = true;
                    });
                }
            }, stream_);
            bool in_time = run(done, timeout);
            data.append(chunk_.data(), bytes);
            if (ec && (in_time || ec != boost::asio::error::operation_aborted)) eof_ = true;
            return bytes;
        }

        // Read until the peer closes the connection or the timeout expires
        exchange_result read_until_closed(std::chrono::milliseconds timeout) {
            exchange_result result;
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!eof_) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (left <= 0ms) break;
                read_some(result.data, left);
            }
            result.closed = eof_;
            return result;
        }

        // Read, appending to `data`, until it holds `needle`, the connection ends or the timeout
        // expires: whether it holds it
        bool read_until(std::string& data, std::string_view needle, std::chrono::milliseconds timeout) {
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (data.find(needle) == std::string::npos && !eof_) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (read_some(data, left) == 0 && !eof_) break;
            }
            return data.find(needle) != std::string::npos;
        }

        // Whether the connection ended (a read failed)
        bool eof() const { return eof_; }

        // Close the sending direction: the server reads the end of the stream (over TLS, the
        // TCP half close without a close_notify, as the TCP and Unix peers do)
        void shutdown_send() {
            boost::system::error_code ec;
            if (auto* sock = std::get_if<tcp_stream>(&stream_)) sock->shutdown(tcp_stream::shutdown_send, ec);
            if (auto* sock = std::get_if<unix_stream>(&stream_)) sock->shutdown(unix_stream::shutdown_send, ec);
            if (auto* stream = std::get_if<tls_stream>(&stream_)) {
                stream->lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);
            }
        }

        // Over TLS, send a close_notify without closing the TCP connection, which stays readable
        // (OpenSSL ends what it reads then at the first record that is not data: read the
        // session tickets of TLS 1.3 first)
        void close_notify() {
            auto* stream = std::get_if<tls_stream>(&stream_);
            REQUIRE(stream);
            // Writes the alert (at once on a local connection), then reads for the peer's,
            // dropping the data that comes meanwhile: that read is cancelled
            stream->async_shutdown([](boost::system::error_code) {});
            ioc_.restart();
            ioc_.run_for(20ms);
            cancel();
            ioc_.restart();
            ioc_.run();
        }

        void close() {
            boost::system::error_code ec;
            std::visit([&](auto& stream) {
                if constexpr (!std::is_same_v<std::decay_t<decltype(stream)>, std::monostate>) {
                    auto& sock = stream.lowest_layer();
                    sock.shutdown(boost::asio::socket_base::shutdown_both, ec);
                    sock.close(ec);
                }
            }, stream_);
        }

        // Close the connection with a reset (RST) as a peer that is lost (a plain close over
        // Unix: it has no reset)
        void reset() {
            boost::system::error_code ec;
            if (auto* sock = std::get_if<tcp_stream>(&stream_)) {
                sock->set_option(boost::asio::socket_base::linger(true, 0), ec);
                sock->close(ec);
            }
            if (auto* stream = std::get_if<tls_stream>(&stream_)) {
                stream->lowest_layer().set_option(boost::asio::socket_base::linger(true, 0), ec);
                stream->lowest_layer().close(ec);
            }
            if (auto* sock = std::get_if<unix_stream>(&stream_)) sock->close(ec);
        }

    private:
        using tcp_stream = boost::asio::ip::tcp::socket;
        using tls_stream = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;
        using unix_stream = boost::asio::local::stream_protocol::socket;

        // Run the io_context until `done` or the timeout; an operation still pending is
        // cancelled (and completes with operation_aborted). Whether it finished in time
        bool run(bool& done, std::chrono::milliseconds timeout) {
            ioc_.restart();
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!done && std::chrono::steady_clock::now() < deadline) {
                if (ioc_.run_one_until(deadline) == 0 && ioc_.stopped()) break;
            }
            if (done) return true;
            cancel();
            ioc_.restart();
            while (!done && ioc_.run_one() != 0) {}
            return false;
        }

        void cancel() {
            boost::system::error_code ec;
            std::visit([&](auto& stream) {
                if constexpr (!std::is_same_v<std::decay_t<decltype(stream)>, std::monostate>) {
                    stream.lowest_layer().cancel(ec);
                }
            }, stream_);
        }

        transport kind_;
        boost::asio::io_context ioc_;
        std::unique_ptr<boost::asio::ssl::context> context_;
        std::variant<std::monostate, tcp_stream, tls_stream, unix_stream> stream_;
        std::array<char, 65536> chunk_{};
        bool eof_ = false;
    };

}

// Run the rest of the TEST_CASE once per transport, as `name`, naming it in failure messages
#define MATRIX_TRANSPORT(name)                                                                             \
    const auto name = GENERATE(::thinger::http::test::transport::tcp, ::thinger::http::test::transport::tls, \
                               ::thinger::http::test::transport::unix);                                    \
    INFO("transport: " << ::thinger::http::test::to_string(name))

#endif

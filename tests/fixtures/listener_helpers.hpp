#ifndef THINGER_HTTP_TEST_LISTENER_HELPERS_HPP
#define THINGER_HTTP_TEST_LISTENER_HELPERS_HPP

#include <thinger/asio/listener.hpp>
#include <thinger/asio/sockets/socket.hpp>
#include <thinger/asio/ssl/certificate_manager.hpp>
#include <thinger/util/logger.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <spdlog/sinks/ringbuffer_sink.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

// Helpers of the listener tests: an io_context on a thread of its own to accept and serve on,
// options on localhost, the connections handed over and the messages logged, TLS clients, and
// raw clients that stall a handshake

namespace thinger::http::test {

// An io_context run on a thread of its own
struct io_thread {
    boost::asio::io_context io_context;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{io_context.get_executor()};
    std::thread thread{[this] { io_context.run(); }};

    asio::executor_provider provider() {
        return [this] { return boost::asio::any_io_executor(io_context.get_executor()); };
    }

    ~io_thread() {
        work.reset();
        io_context.stop();
        thread.join();
    }
};

// Plain TCP on localhost, binding at the first attempt
inline asio::listener_options tcp_options(uint16_t port = 0) {
    asio::listener_options options;
    options.endpoint = asio::tcp_endpoint{"127.0.0.1", port};
    options.max_listening_attempts = 1;
    return options;
}

// TLS on localhost with the default (self-signed) certificate of `certificates`, a manager of
// its own if none is given
inline asio::listener_options secure_options(std::chrono::milliseconds handshake_timeout = std::chrono::seconds(60),
                                             std::shared_ptr<asio::certificate_manager> certificates = nullptr) {
    auto options = tcp_options();
    options.tls = certificates ? std::move(certificates) : std::make_shared<asio::certificate_manager>();
    options.handshake_timeout = handshake_timeout;
    return options;
}

// The sockets handed to a listener's handler, waited for by the test
struct delivered {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::shared_ptr<asio::socket>> sockets;

    sync_callback<void(std::shared_ptr<asio::socket>)> handler() {
        return [this](std::shared_ptr<asio::socket> sock) {
            std::scoped_lock lock(mutex);
            sockets.push_back(std::move(sock));
            changed.notify_all();
        };
    }

    bool wait(size_t count, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return sockets.size() >= count; });
    }

    size_t size() {
        std::scoped_lock lock(mutex);
        return sockets.size();
    }
};

// Every message of the library logger, as "[level] message", while alive
struct log_capture {
    std::shared_ptr<spdlog::logger> previous = logging::get_logger();
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(1024);

    log_capture() {
        auto logger = std::make_shared<spdlog::logger>("http", sink);
        logger->set_pattern("[%l] %v");
        logger->set_level(spdlog::level::trace);
        logging::set_logger(logger);
    }
    ~log_capture() { logging::set_logger(previous); }

    log_capture(const log_capture&) = delete;
    log_capture& operator=(const log_capture&) = delete;

    size_t count(const std::string& level, const std::string& text) const {
        auto lines = sink->last_formatted();
        return std::count_if(lines.begin(), lines.end(), [&](const std::string& line) {
            return line.starts_with("[" + level + "] ") && line.find(text) != std::string::npos;
        });
    }

    // Whether a message is logged `times` within `timeout` (by a thread of the listener, maybe
    // after the client saw its effect)
    bool logged(const std::string& level, const std::string& text,
                std::chrono::milliseconds timeout = std::chrono::seconds(2), size_t times = 1) const {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (count(level, text) < times) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }
};

// A TLS client on localhost (no certificate verified), bounded by `timeout`: whether its
// handshake succeeded
inline bool tls_handshake(uint16_t port, std::chrono::milliseconds timeout = std::chrono::seconds(5),
                          const std::string& server_name = "") {
    boost::asio::io_context io_context;
    boost::asio::ssl::context context(boost::asio::ssl::context::tls_client);
    context.set_verify_mode(boost::asio::ssl::verify_none);
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream(io_context, context);
    if (!server_name.empty()) SSL_set_tlsext_host_name(stream.native_handle(), server_name.c_str());
    bool done = false;
    stream.next_layer().async_connect({boost::asio::ip::make_address("127.0.0.1"), port},
                                      [&](const boost::system::error_code& ec) {
        if (ec) return;
        stream.async_handshake(boost::asio::ssl::stream_base::client,
                               [&](const boost::system::error_code& e) { done = !e; });
    });
    io_context.run_for(timeout);
    return done;
}

// The first flight of a TLS client: a complete ClientHello record, as OpenSSL writes it
inline std::string client_hello(const std::string& server_name = "") {
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(context.get()), SSL_free);
    BIO* input = BIO_new(BIO_s_mem());
    BIO* output = BIO_new(BIO_s_mem());
    SSL_set_bio(ssl.get(), input, output);   // owned by ssl
    if (!server_name.empty()) SSL_set_tlsext_host_name(ssl.get(), server_name.c_str());
    SSL_set_connect_state(ssl.get());
    SSL_do_handshake(ssl.get());             // wants to read the ServerHello
    char* data = nullptr;
    auto size = BIO_get_mem_data(output, &data);
    return {data, static_cast<size_t>(size)};
}

// A TCP connection to localhost that sends `bytes` (for instance client_hello()) and then
// nothing, stalling its handshake (POSIX, so many can be opened at once)
struct stalled_client {
    int fd = -1;
    std::chrono::steady_clock::time_point connected_at;

    explicit stalled_client(uint16_t port, const std::string& bytes = "") {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(fd);
            fd = -1;
            return;
        }
        connected_at = std::chrono::steady_clock::now();
        if (!bytes.empty()) (void)!::send(fd, bytes.data(), bytes.size(), 0);
    }

    ~stalled_client() {
        if (fd >= 0) ::close(fd);
    }

    stalled_client(const stalled_client&) = delete;
    stalled_client& operator=(const stalled_client&) = delete;

    bool connected() const { return fd >= 0; }

    // Close the connection: with a FIN, or with a reset (RST)
    void close() {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
    void reset() {
        linger abort{1, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &abort, sizeof(abort));
        close();
    }

    // What the server did within `timeout`: nothing, sent something (a ServerHello), or closed
    // the connection (EOF or reset). Data read is consumed
    enum class outcome { silent, data, closed };
    outcome wait(std::chrono::milliseconds timeout) const {
        pollfd descriptor{fd, POLLIN, 0};
        if (::poll(&descriptor, 1, static_cast<int>(timeout.count())) <= 0) return outcome::silent;
        char buffer[4096];
        auto received = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        return received > 0 ? outcome::data : outcome::closed;
    }

    // Whether the server closed the connection within `timeout`, skipping what it sent before
    bool closed_within(std::chrono::milliseconds timeout) const {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() < 0) return false;
            switch (wait(left)) {
                case outcome::closed: return true;
                case outcome::silent: return false;
                case outcome::data: break;
            }
        }
    }
};

using session_ptr = std::shared_ptr<SSL_SESSION>;

struct tls_client_options {
    std::string certificate;            // PEM: the certificate first, then its chain
    std::string key;
    std::string server_name;            // SNI (none if empty)
    int version = 0;                    // TLS1_2_VERSION or TLS1_3_VERSION (0: any)
    session_ptr session;                // an earlier session to resume
    std::function<void(SSL_CTX*)> tamper;
    std::string greeting = "ok";        // what the server sends once it accepted (none: nothing)
};

// A TLS client on localhost (OpenSSL through Boost.Asio, synchronous), of a server that answers
// "ok" once it accepted the connection (TLS 1.3 refuses a client certificate after the client
// finished its handshake), or of an HTTPS server (no greeting)
struct tls_client {
    boost::asio::io_context io_context;
    boost::asio::ssl::context context{boost::asio::ssl::context::tls_client};
    std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> stream;

    tls_client() = default;
    tls_client(const tls_client&) = delete;
    tls_client& operator=(const tls_client&) = delete;

    // As a client closing cleanly (close_notify, written to the stream's buffer only): OpenSSL
    // makes the session of a connection freed without it not resumable
    ~tls_client() {
        if (stream && SSL_is_init_finished(stream->native_handle())) SSL_shutdown(stream->native_handle());
    }

    bool connect(uint16_t port, const tls_client_options& options) {
        auto* handle = context.native_handle();
        if (options.version) {
            SSL_CTX_set_min_proto_version(handle, options.version);
            SSL_CTX_set_max_proto_version(handle, options.version);
        }
        if (!options.certificate.empty()) {
            context.use_certificate_chain(boost::asio::buffer(options.certificate));
            context.use_private_key(boost::asio::buffer(options.key), boost::asio::ssl::context::pem);
        }
        if (options.tamper) options.tamper(handle);
        stream = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(io_context, context);
        boost::system::error_code ec;
        stream->next_layer().connect({boost::asio::ip::make_address("127.0.0.1"), port}, ec);
        if (ec) return false;
        if (!options.server_name.empty()) SSL_set_tlsext_host_name(stream->native_handle(), options.server_name.c_str());
        if (options.session) {
            // A copy: a connection that fails makes its session not resumable
            SSL_SESSION* copy = SSL_SESSION_dup(options.session.get());
            SSL_set_session(stream->native_handle(), copy);
            SSL_SESSION_free(copy);
        }
        stream->handshake(boost::asio::ssl::stream_base::client, ec);
        return !ec && read(options.greeting.size()) == options.greeting;
    }

    // Up to `size` bytes, fewer if the connection ends first
    std::string read(size_t size) {
        std::string data(size, '\0');
        boost::system::error_code ec;
        auto bytes = boost::asio::read(*stream, boost::asio::buffer(data), ec);
        data.resize(bytes);
        return data;
    }

    bool write(const std::string& data) {
        boost::system::error_code ec;
        boost::asio::write(*stream, boost::asio::buffer(data), ec);
        return !ec;
    }

    bool resumed() const { return SSL_session_reused(stream->native_handle()) == 1; }
    session_ptr session() const { return {SSL_get1_session(stream->native_handle()), SSL_SESSION_free}; }
};

// What a client got from the server: whether it was accepted, whether it resumed a session, and
// its session (to resume it)
struct tls_attempt {
    bool accepted = false;
    bool resumed = false;
    session_ptr session;
};

inline tls_attempt connect_tls(uint16_t port, const tls_client_options& options = {}) {
    tls_client client;
    tls_attempt attempt;
    attempt.accepted = client.connect(port, options);
    if (attempt.accepted) {
        attempt.resumed = client.resumed();
        attempt.session = client.session();
    }
    return attempt;
}

} // namespace thinger::http::test

#endif // THINGER_HTTP_TEST_LISTENER_HELPERS_HPP

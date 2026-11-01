#ifndef THINGER_HTTP_TEST_HOSTILE_PEER_HPP
#define THINGER_HTTP_TEST_HOSTILE_PEER_HPP

#include <thinger/http/client/websocket_util.hpp>
#include <thinger/util/types.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>
#include "transport_matrix.hpp"

// A hostile peer: a server, on its own io_context and thread, that accepts the connections of
// the client under test and misbehaves from a given point of the exchange (see hostile_start),
// so that a client operation waiting on it must end at its own deadline. Its endpoint() works
// with transport_endpoint::configure() and url() as a transport_listener's does. It stays a
// fixture: the cases are written with it, one per operation and behaviour. A peer that never
// accepts (a connect left pending) is pending_connect_peer, below. The server role has its
// hostile client, hostile_client, below: silent, trickling a head or a body, not reading, or
// reading slowly; or, past an upgrade, silent, trickling, or sending without reading.
namespace thinger::http::test {

    using namespace std::chrono_literals;

    // How the peer misbehaves once it starts to
    enum class hostile_behaviour {
        silent,     // reads (and drops) what it gets, never writes
        trickle,    // writes one byte per period, too slowly for anything to complete
        no_read     // neither reads nor writes: the client's send buffers fill up
    };

    inline const char* to_string(hostile_behaviour behaviour) {
        switch (behaviour) {
            case hostile_behaviour::silent: return "silent";
            case hostile_behaviour::trickle: return "trickle";
            case hostile_behaviour::no_read: return "no-read";
        }
        return "?";
    }

    // Where in the exchange the peer starts to misbehave, behaving well before it. Each point
    // follows the one before it
    enum class hostile_start {
        // Right after accepting, before any byte; over TLS before the handshake, on the raw
        // TCP connection: a trickle there is the start of a TLS record never completed
        after_accept,
        // Over TLS once the handshake is done (with the certificate of matrix_certificates()),
        // before any byte of HTTP; on the other transports the same as after_accept
        after_tls_handshake,
        // Once the head of the request is read (up to its blank line)
        after_request_head,
        // Once the head of a response is sent, announcing a Content-Length larger than the
        // body bytes that follow it (a trickle writes the rest of the body)
        after_response_headers,
        // Once a valid 101 answers the WebSocket upgrade of the request (a trickle writes a
        // text frame of 64 KB, never completed)
        after_101
    };

    inline const char* to_string(hostile_start start) {
        switch (start) {
            case hostile_start::after_accept: return "after accept";
            case hostile_start::after_tls_handshake: return "after the TLS handshake";
            case hostile_start::after_request_head: return "after the request head";
            case hostile_start::after_response_headers: return "after the response headers";
            case hostile_start::after_101: return "after the 101";
        }
        return "?";
    }

    class hostile_peer {
    public:
        // Body bytes sent along with the response head of after_response_headers, and the
        // Content-Length it announces
        static constexpr std::string_view partial_body = "partial";
        static constexpr size_t announced_length = 1000000;

        explicit hostile_peer(transport kind, hostile_behaviour behaviour,
                              hostile_start start = hostile_start::after_accept,
                              std::chrono::milliseconds trickle_period = 200ms)
            : behaviour_(behaviour), start_(start), trickle_period_(trickle_period) {
            endpoint_.kind = kind;
            if (kind == transport::tls) {
                certificates_ = matrix_certificates();
                tls_context_ = certificates_->get_default_certificate();
                REQUIRE(tls_context_);
            }
            if (kind == transport::unix) {
                directory_ = std::make_unique<temporary_directory>();
                endpoint_.unix_path = (directory_->path() / "h.sock").string();
                auto& acceptor = acceptor_.emplace<unix_acceptor>(
                    context_, boost::asio::local::stream_protocol::endpoint(endpoint_.unix_path));
                co_spawn(context_, accept_loop(acceptor), detached);
            } else {
                auto& acceptor = acceptor_.emplace<tcp_acceptor>(
                    context_, boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), 0});
                endpoint_.port = acceptor.local_endpoint().port();
                co_spawn(context_, accept_loop(acceptor), detached);
            }
            thread_ = std::thread([this] { context_.run(); });
        }

        // Closes the acceptor and every connection, and waits for its thread to end
        ~hostile_peer() {
            boost::asio::post(context_, [this] {
                boost::system::error_code ec;
                std::visit([&](auto& acceptor) {
                    if constexpr (!std::is_same_v<std::decay_t<decltype(acceptor)>, std::monostate>) acceptor.close(ec);
                }, acceptor_);
                for (auto& close : closers_) close();
                stop_.cancel();
            });
            thread_.join();
        }

        hostile_peer(const hostile_peer&) = delete;
        hostile_peer& operator=(const hostile_peer&) = delete;

        const transport_endpoint& endpoint() const { return endpoint_; }

        std::string url(const std::string& path = "/") const { return endpoint_.url(path); }

        // The ws:// (wss:// over TLS) url of `path`
        std::string websocket_url(const std::string& path = "/") const {
            return (endpoint_.kind == transport::tls ? "wss://" : "ws://") + endpoint_.host_header() + path;
        }

        // Connections accepted, those that reached the start point (the handshake, the request
        // and the responses before it went well), and those the client closed (seen by a
        // reading peer: not by a no-read one)
        size_t accepted() const { return accepted_; }
        size_t started() const { return started_; }
        size_t closed() const { return closed_; }

        // Bytes written by a trickling peer, over all its connections
        size_t trickled() const { return trickled_; }

        // A trickling peer writes no more bytes from now on: it goes silent
        void stop_trickle() { trickle_stopped_ = true; }

    private:
        using tcp_acceptor = boost::asio::ip::tcp::acceptor;
        using unix_acceptor = boost::asio::local::stream_protocol::acceptor;
        using tls_stream = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

        template<typename Acceptor>
        awaitable<void> accept_loop(Acceptor& acceptor) {
            using socket_type = typename Acceptor::protocol_type::socket;
            for (;;) {
                socket_type socket(context_);
                auto [ec] = co_await acceptor.async_accept(socket, use_nothrow_awaitable);
                if (ec) co_return;
                ++accepted_;
                if constexpr (std::is_same_v<socket_type, boost::asio::ip::tcp::socket>) {
                    if (endpoint_.kind == transport::tls && start_ != hostile_start::after_accept) {
                        co_spawn(context_, serve(own(std::make_shared<tls_stream>(std::move(socket), *tls_context_))),
                                 detached);
                        continue;
                    }
                }
                co_spawn(context_, serve(own(std::make_shared<socket_type>(std::move(socket)))), detached);
            }
        }

        // Keep a way to close the connection when the peer is destroyed
        template<typename Stream>
        std::shared_ptr<Stream> own(std::shared_ptr<Stream> stream) {
            std::weak_ptr<Stream> weak = stream;
            closers_.emplace_back([weak] {
                boost::system::error_code ignored;
                if (auto alive = weak.lock()) alive->lowest_layer().close(ignored);
            });
            return stream;
        }

        // Behave well up to the start point, then misbehave
        template<typename Stream>
        awaitable<void> serve(std::shared_ptr<Stream> peer) {
            if constexpr (std::is_same_v<Stream, tls_stream>) {
                auto [ec] = co_await peer->async_handshake(boost::asio::ssl::stream_base::server, use_nothrow_awaitable);
                if (ec) {
                    ++closed_;
                    co_return;
                }
            }
            if (start_ >= hostile_start::after_request_head) {
                std::string head;
                auto [ec, bytes] = co_await boost::asio::async_read_until(*peer, boost::asio::dynamic_buffer(head),
                                                                          "\r\n\r\n", use_nothrow_awaitable);
                if (ec) {
                    ++closed_;
                    co_return;
                }
                std::string response;
                if (start_ == hostile_start::after_response_headers) {
                    response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " +
                               std::to_string(announced_length) + "\r\n\r\n" + std::string(partial_body);
                } else if (start_ == hostile_start::after_101) {
                    response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                               "Sec-WebSocket-Accept: " + http::websocket_util::accept_key(header_of(head, "sec-websocket-key")) +
                               "\r\n\r\n";
                }
                if (!response.empty()) {
                    auto [write_ec, written] = co_await boost::asio::async_write(*peer, boost::asio::buffer(response),
                                                                                 use_nothrow_awaitable);
                    if (write_ec) co_return;
                }
            }
            ++started_;
            co_await misbehave(*peer);
        }

        template<typename Stream>
        awaitable<void> misbehave(Stream& peer) {
            switch (behaviour_) {
                case hostile_behaviour::silent:
                    co_await read_until_closed(peer);
                    break;
                case hostile_behaviour::trickle:
                    // the reads tell when the client closes, which ends the trickle
                    co_await (read_until_closed(peer) || trickle(peer));
                    break;
                case hostile_behaviour::no_read:
                    co_await stop_.async_wait(use_nothrow_awaitable);
                    break;
            }
        }

        template<typename Stream>
        awaitable<void> read_until_closed(Stream& peer) {
            std::array<char, 4096> buffer{};
            for (;;) {
                auto [ec, bytes] = co_await peer.async_read_some(boost::asio::buffer(buffer), use_nothrow_awaitable);
                if (ec) break;
            }
            ++closed_;
        }

        template<typename Stream>
        awaitable<void> trickle(Stream& peer) {
            auto bytes = trickle_bytes();
            boost::asio::steady_timer timer(context_);
            for (size_t i = 0; !trickle_stopped_; ++i) {
                // the prefix, then its last byte repeated for ever
                char byte = bytes[std::min(i, bytes.size() - 1)];
                auto [ec, written] = co_await boost::asio::async_write(peer, boost::asio::buffer(&byte, 1),
                                                                       use_nothrow_awaitable);
                if (ec) co_return;
                ++trickled_;
                timer.expires_after(trickle_period_);
                auto [wait_ec] = co_await timer.async_wait(use_nothrow_awaitable);
                if (wait_ec) co_return;
            }
            // silent from now on: the reads go on until the client closes
            co_await stop_.async_wait(use_nothrow_awaitable);
        }

        // What a trickling peer writes, the last byte repeated once the rest is written: over
        // TLS before the handshake, a handshake record announcing 16 KB; after the response
        // headers, the body; after a 101, an unmasked text frame of 64 KB; otherwise a response
        // whose head never ends
        std::string trickle_bytes() const {
            if (endpoint_.kind == transport::tls && start_ == hostile_start::after_accept) {
                return std::string("\x16\x03\x03\x40\x00\x02", 6);
            }
            if (start_ == hostile_start::after_response_headers) return "a";
            if (start_ == hostile_start::after_101) return std::string("\x81\x7e\xff\xff", 4) + "a";
            return "HTTP/1.1 200 OK\r\nX-Trickle: a";
        }

        // The value of a header of a request head (none: empty)
        static std::string header_of(const std::string& head, std::string_view name) {
            std::string lower = head;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
            auto at = lower.find("\r\n" + std::string(name) + ":");
            if (at == std::string::npos) return {};
            auto begin = head.find_first_not_of(' ', at + 3 + name.size());
            auto end = head.find("\r\n", begin);
            return head.substr(begin, end - begin);
        }

        hostile_behaviour behaviour_;
        hostile_start start_;
        std::chrono::milliseconds trickle_period_;
        transport_endpoint endpoint_;
        std::shared_ptr<asio::certificate_manager> certificates_;
        std::shared_ptr<boost::asio::ssl::context> tls_context_;
        std::unique_ptr<temporary_directory> directory_;
        boost::asio::io_context context_;
        // never expires: the no-read peers wait on it until the peer is destroyed
        boost::asio::steady_timer stop_{context_, boost::asio::steady_timer::time_point::max()};
        std::variant<std::monostate, tcp_acceptor, unix_acceptor> acceptor_;
        // on the thread of the peer only
        std::vector<std::function<void()>> closers_;
        std::atomic<size_t> accepted_{0};
        std::atomic<size_t> started_{0};
        std::atomic<size_t> closed_{0};
        std::atomic<size_t> trickled_{0};
        std::atomic<bool> trickle_stopped_{false};
        std::thread thread_;
    };

    // A peer that never accepts: a listener whose queue is full, so that the next connect to it
    // stays pending until the client gives up. Over TLS the same TCP listener (the connect
    // pends before any handshake). The queue is filled with connections of its own (a backlog
    // of 1: macOS takes a backlog of 0 as the largest one), until one of them is seen pending.
    // Where the system refuses a connect to a full queue instead of leaving it pending (Unix
    // sockets on Linux and macOS), pending() is false and reason() says what happened
    class pending_connect_peer {
    public:
        explicit pending_connect_peer(transport kind) {
            endpoint_.kind = kind;
            int family = kind == transport::unix ? AF_UNIX : AF_INET;
            listener_ = ::socket(family, SOCK_STREAM, 0);
            REQUIRE(listener_ >= 0);
            sockaddr_storage address{};
            socklen_t length = 0;
            if (kind == transport::unix) {
                directory_ = std::make_unique<temporary_directory>();
                endpoint_.unix_path = (directory_->path() / "p.sock").string();
                auto& unix_address = reinterpret_cast<sockaddr_un&>(address);
                unix_address.sun_family = AF_UNIX;
                REQUIRE(endpoint_.unix_path.size() < sizeof(unix_address.sun_path));
                std::memcpy(unix_address.sun_path, endpoint_.unix_path.c_str(), endpoint_.unix_path.size() + 1);
                length = sizeof(unix_address);
            } else {
                auto& inet_address = reinterpret_cast<sockaddr_in&>(address);
                inet_address.sin_family = AF_INET;
                inet_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                length = sizeof(inet_address);
            }
            REQUIRE(::bind(listener_, reinterpret_cast<sockaddr*>(&address), length) == 0);
            REQUIRE(::listen(listener_, 1) == 0);
            REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
            if (kind != transport::unix) endpoint_.port = ntohs(reinterpret_cast<sockaddr_in&>(address).sin_port);
            fill(reinterpret_cast<sockaddr*>(&address), length);
        }

        ~pending_connect_peer() {
            for (int fd : fillers_) ::close(fd);
            ::close(listener_);
        }

        pending_connect_peer(const pending_connect_peer&) = delete;
        pending_connect_peer& operator=(const pending_connect_peer&) = delete;

        const transport_endpoint& endpoint() const { return endpoint_; }

        // Whether a connect to it now stays pending, and why not
        bool pending() const { return reason_.empty(); }
        const std::string& reason() const { return reason_; }

    private:
        // Connect until a connect stays pending for 200 ms, keeping every connection
        void fill(const sockaddr* address, socklen_t length) {
            for (int i = 0; i < 64; ++i) {
                int fd = ::socket(address->sa_family, SOCK_STREAM, 0);
                REQUIRE(fd >= 0);
                fillers_.push_back(fd);
                REQUIRE(::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK) == 0);
                if (::connect(fd, address, length) == 0) continue;
                if (errno != EINPROGRESS) {
                    reason_ = std::string("the system refuses a connect to a full queue (") + std::strerror(errno) +
                              ") instead of leaving it pending";
                    return;
                }
                pollfd poll{fd, POLLOUT, 0};
                if (::poll(&poll, 1, 200) == 0) return;
                int error = 0;
                socklen_t size = sizeof(error);
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size);
                if (error) {
                    reason_ = std::string("a connect to a full queue failed (") + std::strerror(error) + ")";
                    return;
                }
            }
            reason_ = "64 connections were queued, none left pending";
        }

        transport_endpoint endpoint_;
        std::unique_ptr<temporary_directory> directory_;
        int listener_ = -1;
        std::vector<int> fillers_;
        std::string reason_;
    };

    // What a hostile client saw of the server: what it wrote, whether it closed the connection
    // (any read error: a FIN, a reset, a TLS close_notify), when the first byte of an answer and
    // the close came, measured from the client's start (see hostile_client), and whether the
    // client's own writes still went through (a server that stopped reading may not close)
    struct hostile_outcome {
        std::string data;
        bool closed = false;
        std::chrono::steady_clock::duration answered{};
        std::chrono::steady_clock::duration elapsed{};
        size_t trickled = 0;

        bool answered_with(std::string_view status_line) const { return data.starts_with(status_line); }
    };

    // A hostile client, the server-side counterpart of hostile_peer: it connects to a server
    // through a transport_endpoint (over TLS verifying it against the matrix CA, the handshake
    // done before it starts), and misbehaves from its first byte, so that a server wait on it
    // must end at the server's own deadline. Each behaviour ends when the server closes the
    // connection or at `limit`, never later: a server that never acts fails its case, without
    // hanging it. Its times count from the client's start: the end of the connect (handshake
    // included) for a silent client, its first byte otherwise
    class hostile_client {
    public:
        // The start of a head that never ends, one byte of its last header value per period
        static constexpr std::string_view endless_head = "GET / HTTP/1.1\r\nHost: x\r\nX-Trickle: ";

        // A `receive_buffer` (bytes, over TCP and TLS) bounds what the server may write before a
        // client that reads nothing stalls it (see raw_connection)
        explicit hostile_client(const transport_endpoint& to, std::chrono::milliseconds period = 200ms,
                                int receive_buffer = 0)
            : connection_(to, true, 10s, receive_buffer), period_(period),
              connected_(std::chrono::steady_clock::now()) {}

        hostile_client(const hostile_client&) = delete;
        hostile_client& operator=(const hostile_client&) = delete;

        transport kind() const { return connection_.kind(); }
        raw_connection& connection() { return connection_; }

        // Send nothing, reading what the server sends until it closes
        hostile_outcome stay_silent(std::chrono::milliseconds limit) {
            hostile_outcome outcome;
            read_until(outcome, connected_, connected_ + limit);
            return outcome;
        }

        // Send `prefix` (by default a head that never ends), then one more byte (a) per period,
        // reading between the bytes, until the server closes
        hostile_outcome trickle_head(std::chrono::milliseconds limit, std::string_view prefix = endless_head) {
            return trickle(prefix, limit);
        }

        // Send a whole head announcing a body (`head`, its blank line included), then the body
        // one byte (x) per period, reading between the bytes, until the server closes
        hostile_outcome trickle_body(std::string_view head, std::chrono::milliseconds limit) {
            return trickle(head, limit, 'x');
        }

        // Send `request` whole, and read nothing: the server's response fills the buffers of the
        // connection. Its start is then; read_until_closed() reads what comes afterwards
        bool send_without_reading(std::string_view request) {
            start_ = std::chrono::steady_clock::now();
            return connection_.write(request);
        }

        // Read until the server closes, or `limit` from the start: when the close is seen, which a
        // client that stopped reading only sees once it reads what was sent before it
        hostile_outcome read_until_closed(std::chrono::milliseconds limit) {
            hostile_outcome outcome;
            read_until(outcome, start_, std::chrono::steady_clock::now() + limit);
            return outcome;
        }

        // A slow client that keeps reading: read at most `bytes` per period, appending to
        // `outcome`, until the server closes, `limit` from now, or `enough` holds for what it
        // read. Called again, it goes on where it left; once it returns it reads nothing more:
        // last_read() is when its last read took a byte
        template<typename Enough>
        hostile_outcome& read_at_rate(hostile_outcome& outcome, size_t bytes, std::chrono::milliseconds limit,
                                      Enough enough) {
            auto until = clock::now() + limit;
            for (auto period = clock::now(); !connection_.eof() && !enough(outcome.data) && period < until;) {
                auto end = std::min(period + period_, until);
                for (size_t taken = 0; taken < bytes && !connection_.eof() && clock::now() < end;) {
                    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - clock::now());
                    auto read = connection_.read_some(outcome.data, std::max(left, 1ms), bytes - taken);
                    if (read == 0) continue;
                    taken += read;
                    last_read_ = clock::now();
                    if (outcome.answered == clock::duration{}) outcome.answered = last_read_ - start_;
                }
                std::this_thread::sleep_until(end);
                period = end;
            }
            if (connection_.eof()) note_closed(outcome, start_);
            return outcome;
        }

        std::chrono::steady_clock::time_point last_read() const { return last_read_; }

        // Send an upgrade `request` and read its answer, within `limit`, up to the end of its
        // head (with what came after it). The start is then: a tunnel is open, which the
        // client keeps silent with read_until_closed(), or sends to with flood() or
        // trickle_data()
        hostile_outcome upgrade(std::string_view request, std::chrono::milliseconds limit = 5s) {
            hostile_outcome outcome;
            if (connection_.write(request)) connection_.read_until(outcome.data, "\r\n\r\n", limit);
            start_ = clock::now();
            return outcome;
        }

        // Send `chunk` over and over, reading nothing, until a write fails (the server closed
        // the connection) or `limit` from now: a client that does not read what it is sent
        // back. Its times count from the start; last_write() is when a write last ended
        hostile_outcome flood(std::string_view chunk, std::chrono::milliseconds limit) {
            hostile_outcome outcome;
            auto until = clock::now() + limit;
            for (;;) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - clock::now());
                if (left <= 0ms) return outcome;
                if (!connection_.write(chunk, left)) break;
                last_write_ = clock::now();
            }
            // (not a write cut at the limit)
            if (clock::now() < until) note_closed(outcome, start_);
            return outcome;
        }

        std::chrono::steady_clock::time_point last_write() const { return last_write_; }

        // Send `bytes` one per period, reading what comes between them and for one period after
        // the last one, until the server closes
        hostile_outcome trickle_data(std::string_view bytes) {
            hostile_outcome outcome;
            for (char byte : bytes) {
                if (read_until(outcome, start_, clock::now() + period_)) return outcome;
                if (!connection_.write(std::string_view(&byte, 1), 1s)) {
                    note_closed(outcome, start_);
                    return outcome;
                }
                ++outcome.trickled;
            }
            read_until(outcome, start_, clock::now() + period_);
            return outcome;
        }

    private:
        using clock = std::chrono::steady_clock;

        hostile_outcome trickle(std::string_view prefix, std::chrono::milliseconds limit, char filler = 'a') {
            hostile_outcome outcome;
            start_ = clock::now();
            auto until = start_ + limit;
            bool sent = connection_.write(prefix);
            while (sent && !connection_.eof() && clock::now() < until) {
                if (read_until(outcome, start_, std::min(clock::now() + period_, until))) break;
                sent = connection_.write(std::string_view(&filler, 1), 1s);
                if (sent) ++outcome.trickled;
            }
            // A write that failed: the server closed (or reset) the connection, read what it sent
            if (!sent && !connection_.eof()) read_until(outcome, start_, clock::now() + 500ms);
            if (!sent) note_closed(outcome, start_);
            return outcome;
        }

        // Read until the server closes or `until`: whether it closed
        bool read_until(hostile_outcome& outcome, clock::time_point start, clock::time_point until) {
            while (!connection_.eof()) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - clock::now());
                if (left <= 0ms) break;
                if (connection_.read_some(outcome.data, left) > 0 && outcome.answered == clock::duration{}) {
                    outcome.answered = clock::now() - start;
                }
            }
            if (connection_.eof()) note_closed(outcome, start);
            return outcome.closed;
        }

        static void note_closed(hostile_outcome& outcome, clock::time_point start) {
            if (outcome.closed) return;
            outcome.closed = true;
            outcome.elapsed = clock::now() - start;
        }

        raw_connection connection_;
        std::chrono::milliseconds period_;
        clock::time_point connected_;
        clock::time_point start_ = connected_;
        clock::time_point last_read_ = connected_;
        clock::time_point last_write_ = connected_;
    };

}

#endif

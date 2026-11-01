// Server cost of WebSocket connections that get a message now and then, as devices getting a
// command every few seconds: the server thread sends a small binary message to each of the
// connections every <interval> milliseconds, spread evenly over the interval, and the clients
// (one thread, answering the server's pings) read them. It prints the CPU time of the server
// thread per message sent, which counts what each message costs besides its write (e.g. timer
// operations of its write deadline), where a throughput benchmark sees none.
//
//   benchmark_ws_sporadic <connections> <interval ms> <seconds>
//
// The measure starts once every connection got its first message. Only the API of
// thinger-http 0.3.0 is used, so it builds against older versions too. Linux: it raises its own
// open-file limit, and reads the server thread's CPU clock.

#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/websocket_connection.hpp>
#include <boost/asio.hpp>

#include <pthread.h>
#include <sys/resource.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace thinger;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

    constexpr auto TICK = std::chrono::milliseconds(10);
    const std::string MESSAGE(32, 'c');

    double cpu_seconds(clockid_t clock) {
        timespec ts{};
        clock_gettime(clock, &ts);
        return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
    }

    void handshake(tcp::socket& sock) {
        std::string request = "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        net::write(sock, net::buffer(request));
        net::streambuf response;
        // the response alone: the server sends nothing before the sender starts
        net::read_until(sock, response, "\r\n\r\n");
    }

    // A client reading server frames (unmasked), answering pings with a masked pong
    struct client {
        explicit client(net::io_context& ioc) : sock(ioc) {}
        tcp::socket sock;
        std::vector<char> buffer = std::vector<char>(4096);
        std::string pending;
        std::string pong;
        uint64_t received = 0;

        void read() {
            sock.async_read_some(net::buffer(buffer), [this](boost::system::error_code ec, size_t bytes) {
                if (ec) return;
                pending.append(buffer.data(), bytes);
                parse();
                read();
            });
        }

        void parse() {
            size_t at = 0;
            while (pending.size() - at >= 2) {
                auto opcode = uint8_t(pending[at]) & 0x0F;
                size_t size = uint8_t(pending[at + 1]) & 0x7F;
                size_t header = 2;
                if (size == 126) {
                    if (pending.size() - at < 4) break;
                    size = (size_t(uint8_t(pending[at + 2])) << 8) | uint8_t(pending[at + 3]);
                    header = 4;
                }
                if (pending.size() - at < header + size) break;
                if (opcode == 0x9) send_pong(pending.substr(at + header, size));
                else if (opcode == 0x2) ++received;
                at += header + size;
            }
            pending.erase(0, at);
        }

        void send_pong(const std::string& payload) {
            // a ping carries at most 125 bytes; the key is fixed: the server only unmasks
            const char key[4] = {0x12, 0x34, 0x56, 0x78};
            pong.assign({char(0x8A), char(0x80 | payload.size())});
            pong.append(key, 4);
            for (size_t i = 0; i < payload.size(); ++i) pong.push_back(char(payload[i] ^ key[i % 4]));
            boost::system::error_code ec;
            net::write(sock, net::buffer(pong), ec);
        }
    };

    // Sends MESSAGE to each connection once per interval, on the server thread: TICK by TICK,
    // to the next share of the connections
    struct sender {
        sender(net::io_context& ioc, std::vector<std::shared_ptr<http::websocket_connection>>& sockets,
               std::chrono::milliseconds interval)
            : timer(ioc), sockets(sockets), per_tick(double(TICK.count()) / double(interval.count())) {}

        net::steady_timer timer;
        std::vector<std::shared_ptr<http::websocket_connection>>& sockets;
        double per_tick;
        double due = 0;
        size_t next = 0;
        std::atomic<uint64_t> sent{0};
        std::atomic<bool> running{true};

        void start() {
            timer.expires_after(TICK);
            timer.async_wait([this](boost::system::error_code ec) {
                if (ec || !running) return;
                due += per_tick * double(sockets.size());
                for (; due >= 1; due -= 1) {
                    sockets[next]->send_binary(MESSAGE);
                    next = (next + 1) % sockets.size();
                    sent.fetch_add(1, std::memory_order_relaxed);
                }
                start();
            });
        }
    };

}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <connections> <interval ms> <seconds>\n", argv[0]);
        return 1;
    }
    size_t connections = std::strtoul(argv[1], nullptr, 10);
    auto interval = std::chrono::milliseconds(std::strtoul(argv[2], nullptr, 10));
    int seconds = std::atoi(argv[3]);

    // two descriptors per connection, both ends in this process
    rlimit limit{};
    getrlimit(RLIMIT_NOFILE, &limit);
    limit.rlim_cur = limit.rlim_max;
    setrlimit(RLIMIT_NOFILE, &limit);

    http::server server;
    std::vector<std::shared_ptr<http::websocket_connection>> sockets;
    std::atomic<size_t> accepted{0};
    server.get("/ws", [&](http::request&, http::response& res) {
        res.upgrade_websocket({}, [&](std::shared_ptr<http::websocket_connection> ws) {
            // on the server thread, as the sender
            sockets.push_back(std::move(ws));
            accepted = sockets.size();
        });
    });
    if (server.listen("127.0.0.1", 0)) return 1;
    auto url = server.listening_on().front();
    auto port = static_cast<uint16_t>(std::stoul(url.substr(url.rfind(':') + 1)));
    std::thread server_thread([&] { server.wait(); });
    clockid_t server_clock;
    if (pthread_getcpuclockid(server_thread.native_handle(), &server_clock) != 0) return 1;

    net::io_context client_ioc;
    std::vector<std::unique_ptr<client>> clients;
    for (size_t i = 0; i < connections; ++i) {
        auto c = std::make_unique<client>(client_ioc);
        c->sock.connect({net::ip::make_address("127.0.0.1"), port});
        handshake(c->sock);
        c->read();
        clients.push_back(std::move(c));
    }
    while (accepted < connections) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto work = net::make_work_guard(client_ioc);
    std::thread client_thread([&] { client_ioc.run(); });

    sender send(server.get_io_context(), sockets, interval);
    net::post(server.get_io_context(), [&] { send.start(); });

    // warm up: every connection got a message, then measure
    std::this_thread::sleep_for(interval + std::chrono::seconds(1));
    auto start_sent = send.sent.load();
    auto start_cpu = cpu_seconds(server_clock);
    auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    auto cpu = cpu_seconds(server_clock) - start_cpu;
    auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    auto sent = send.sent.load() - start_sent;

    net::post(server.get_io_context(), [&] {
        send.running = false;
        send.timer.cancel();
    });
    work.reset();
    client_ioc.stop();
    client_thread.join();
    uint64_t received = 0;
    for (auto& c : clients) received += c->received;
    server.stop();
    server_thread.join();

    std::printf("connections=%zu interval_ms=%lld seconds=%.1f messages=%llu messages/s=%.0f "
                "server_cpu_s=%.3f server_cpu_us/message=%.2f received=%llu\n",
                connections, static_cast<long long>(interval.count()), elapsed,
                static_cast<unsigned long long>(sent), double(sent) / elapsed, cpu,
                sent ? cpu * 1e6 / double(sent) : 0.0, static_cast<unsigned long long>(received));
    return 0;
}

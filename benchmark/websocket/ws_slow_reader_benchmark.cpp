// A server pushing 64 KB WebSocket messages faster than its client reads them: the bytes
// queued by the server, its memory and the messages lost over time, by producer mode.
//
//   benchmark_ws_slow_reader <mode> <seconds> <reader MB/s> <producer MB/s>
//
// Modes:
//   drop_newest  send_binary() without waiting, default limits (100 messages, drop newest)
//   await        co_await send_binary_async(): one message queued at a time
//   watermark    send_binary() + co_await wait_writable(), high 1 MB / low 256 KB, no
//                message limit
//
// Prints a sample every 250 ms: time, bytes and messages queued, process RSS, messages
// produced and received.

#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace thinger;
using namespace std::chrono_literals;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

    constexpr size_t message_size = 64 * 1024;

    struct stats {
        std::atomic<uint64_t> produced{0};
        std::atomic<uint64_t> received{0};
        std::atomic<bool> running{true};
    };

    size_t rss_bytes() {
        std::ifstream statm("/proc/self/statm");
        size_t pages = 0, resident = 0;
        statm >> pages >> resident;
        return resident * size_t(sysconf(_SC_PAGESIZE));
    }

    // Produce at `rate` bytes/s at most
    awaitable<void> producer(std::shared_ptr<http::websocket_connection> ws, std::string mode, double rate,
                             std::shared_ptr<stats> counters) {
        net::steady_timer timer(co_await net::this_coro::executor);
        auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(double(message_size) / rate));
        auto next = std::chrono::steady_clock::now();
        std::string message(message_size, 'x');
        while (counters->running) {
            if (mode == "await") {
                if (!co_await ws->send_binary_async(message)) break;
            } else {
                ws->send_binary(message);
                if (mode == "watermark" && !co_await ws->wait_writable()) break;
            }
            ++counters->produced;
            next += interval;
            timer.expires_at(next);
            co_await timer.async_wait(use_nothrow_awaitable);
        }
    }

    // Print the state every 250 ms, on the thread of the websocket
    awaitable<void> sampler(std::shared_ptr<http::websocket_connection> ws, std::shared_ptr<stats> counters) {
        net::steady_timer timer(co_await net::this_coro::executor);
        auto start = std::chrono::steady_clock::now();
        while (counters->running) {
            auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::printf("%6.2f queued_bytes=%zu queued_messages=%zu rss_mb=%.1f produced=%llu received=%llu\n",
                        elapsed, ws->queued_bytes(), ws->queued_messages(), double(rss_bytes()) / 1e6,
                        (unsigned long long) counters->produced.load(), (unsigned long long) counters->received.load());
            std::fflush(stdout);
            timer.expires_after(250ms);
            co_await timer.async_wait(use_nothrow_awaitable);
        }
    }

    void configure(const std::shared_ptr<http::websocket_connection>& ws, const std::string& mode) {
        if (mode == "watermark") {
            ws->set_write_limits({.max_messages = 0, .high_watermark = 1024 * 1024, .low_watermark = 256 * 1024});
        }
    }

    // Read frames at `rate` bytes/s at most
    void slow_reader(uint16_t port, double rate, std::shared_ptr<stats> counters) {
        net::io_context ioc;
        tcp::socket sock(ioc);
        sock.connect({net::ip::make_address("127.0.0.1"), port});
        std::string request = "GET /push HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        net::write(sock, net::buffer(request));
        net::streambuf buffer;
        auto header_size = net::read_until(sock, buffer, "\r\n\r\n");
        buffer.consume(header_size);

        // every message is a binary frame of message_size bytes (64 KB: a 10 bytes header)
        const size_t frame_size = message_size + 10;
        size_t pending = buffer.size();
        std::vector<char> chunk(16 * 1024);
        auto start = std::chrono::steady_clock::now();
        uint64_t total = pending;
        boost::system::error_code ec;
        while (counters->running) {
            auto bytes = sock.read_some(net::buffer(chunk), ec);
            if (ec) break;
            total += bytes;
            pending += bytes;
            while (pending >= frame_size) {
                pending -= frame_size;
                ++counters->received;
            }
            auto due = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(double(total) / rate));
            std::this_thread::sleep_until(due);
        }
        sock.close(ec);
    }

}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <drop_newest|await|watermark> <seconds> <reader MB/s> <producer MB/s>\n", argv[0]);
        return 1;
    }
    std::string mode = argv[1];
    int seconds = std::atoi(argv[2]);
    double reader_rate = std::atof(argv[3]) * 1e6;
    double producer_rate = std::atof(argv[4]) * 1e6;
    auto counters = std::make_shared<stats>();

    http::server server;
    server.get("/push", [&](http::request&, http::response& res) {
        res.upgrade_websocket([&](std::shared_ptr<http::websocket_connection> ws) -> awaitable<void> {
            configure(ws, mode);
            auto executor = co_await net::this_coro::executor;
            co_spawn(executor, sampler(ws, counters), detached);
            co_spawn(executor, producer(ws, mode, producer_rate, counters), detached);
        });
    });
    if (!server.listen("127.0.0.1", 0)) return 1;
    uint16_t port = server.local_port();
    std::thread server_thread([&] { server.wait(); });
    std::thread reader([&] { slow_reader(port, reader_rate, counters); });

    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    counters->running = false;
    reader.join();
    server.stop();
    server_thread.join();

    auto produced = counters->produced.load();
    auto received = counters->received.load();
    std::printf("mode=%s produced=%llu received=%llu not_received=%llu\n", mode.c_str(),
                (unsigned long long) produced, (unsigned long long) received,
                (unsigned long long) (produced - std::min(produced, received)));
    return 0;
}

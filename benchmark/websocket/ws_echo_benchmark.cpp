// WebSocket echo throughput of the server websocket_connection (on_message + send_text/
// send_binary, the default overflow policy): clients on their own threads send windows of
// messages and read the echoes back.
//
//   benchmark_ws_echo <message size> <connections> <seconds> <window>
//
// The window stays under websocket_connection::MAX_OUTPUT_MESSAGES, so nothing is dropped.
// Only the API of thinger-http 0.3.0 is used, so it builds against older versions too.

#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace thinger;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

    // A masked binary frame, as a client sends it (the key is fixed: the server only unmasks)
    std::string client_frame(size_t size) {
        std::string frame;
        frame.push_back(char(0x82));
        if (size <= 125) {
            frame.push_back(char(0x80 | size));
        } else if (size <= 65535) {
            frame.push_back(char(0x80 | 126));
            frame.push_back(char(size >> 8));
            frame.push_back(char(size & 0xFF));
        } else {
            frame.push_back(char(0x80 | 127));
            for (int i = 7; i >= 0; --i) frame.push_back(char((uint64_t(size) >> (i * 8)) & 0xFF));
        }
        const char key[4] = {0x12, 0x34, 0x56, 0x78};
        frame.append(key, 4);
        for (size_t i = 0; i < size; ++i) frame.push_back(char('x' ^ key[i % 4]));
        return frame;
    }

    // Read one server frame (unmasked) and return its payload size
    size_t read_frame(tcp::socket& sock, std::vector<char>& payload) {
        uint8_t header[10];
        net::read(sock, net::buffer(header, 2));
        uint64_t size = header[1] & 0x7F;
        if (size == 126) {
            net::read(sock, net::buffer(header + 2, 2));
            size = (uint64_t(header[2]) << 8) | header[3];
        } else if (size == 127) {
            net::read(sock, net::buffer(header + 2, 8));
            size = 0;
            for (int i = 0; i < 8; ++i) size = (size << 8) | header[2 + i];
        }
        payload.resize(size);
        net::read(sock, net::buffer(payload));
        return size;
    }

    void handshake(tcp::socket& sock) {
        std::string request = "GET /echo HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        net::write(sock, net::buffer(request));
        net::streambuf response;
        net::read_until(sock, response, "\r\n\r\n");
    }

    void run_client(uint16_t port, size_t size, size_t window, std::atomic<bool>& running,
                    std::atomic<uint64_t>& messages) {
        net::io_context ioc;
        tcp::socket sock(ioc);
        sock.connect({net::ip::make_address("127.0.0.1"), port});
        sock.set_option(tcp::no_delay(true));
        handshake(sock);

        std::string batch;
        auto frame = client_frame(size);
        for (size_t i = 0; i < window; ++i) batch += frame;
        std::vector<char> payload;
        while (running) {
            net::write(sock, net::buffer(batch));
            for (size_t i = 0; i < window; ++i) {
                if (read_frame(sock, payload) != size) std::abort();
            }
            messages += window;
        }
        boost::system::error_code ec;
        sock.shutdown(tcp::socket::shutdown_both, ec);
    }

}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <message size> <connections> <seconds> <window>\n", argv[0]);
        return 1;
    }
    size_t size = std::strtoul(argv[1], nullptr, 10);
    size_t connections = std::strtoul(argv[2], nullptr, 10);
    int seconds = std::atoi(argv[3]);
    size_t window = std::strtoul(argv[4], nullptr, 10);

    http::server server;
    server.get("/echo", [](http::request&, http::response& res) {
        res.upgrade_websocket([](std::shared_ptr<http::websocket_connection> ws) {
            ws->on_message([ws](std::string message, bool binary) {
                if (binary) ws->send_binary(std::move(message));
                else ws->send_text(std::move(message));
            });
        });
    });
    if (!server.listen("127.0.0.1", 0)) return 1;
    uint16_t port = server.local_port();
    std::thread server_thread([&] { server.wait(); });

    std::atomic<bool> running{true};
    std::atomic<uint64_t> messages{0};
    std::vector<std::thread> clients;
    for (size_t i = 0; i < connections; ++i) {
        clients.emplace_back(run_client, port, size, window, std::ref(running), std::ref(messages));
    }

    // warm up, then measure
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto start_messages = messages.load();
    auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    auto measured = messages.load() - start_messages;
    running = false;
    for (auto& client : clients) client.join();
    server.stop();
    server_thread.join();

    double rate = double(measured) / elapsed;
    // bytes echoed: received and sent back by the server
    std::printf("size=%zu connections=%zu window=%zu messages/s=%.0f MB/s=%.1f\n", size, connections, window,
                rate, rate * double(size) / 1e6);
    return 0;
}

#ifndef THINGER_TESTS_STREAM_TEST_HELPERS_HPP
#define THINGER_TESTS_STREAM_TEST_HELPERS_HPP

#include <thinger/asio/sockets/socket.hpp>
#include <thinger/util/types.hpp>

#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>

// Helpers for tests of sockets used as byte streams: a raw WebSocket client writing and reading
// frames by hand, and a socket defined by the application (an in-memory stream)
namespace stream_test {

    using namespace std::chrono_literals;
    namespace net = boost::asio;
    using tcp = net::ip::tcp;

    constexpr uint8_t OP_CONTINUATION = 0x0;
    constexpr uint8_t OP_TEXT = 0x1;
    constexpr uint8_t OP_BINARY = 0x2;
    constexpr uint8_t OP_CLOSE = 0x8;
    constexpr uint8_t OP_PING = 0x9;
    constexpr uint8_t OP_PONG = 0xA;

    struct ws_frame {
        bool fin = false;
        uint8_t opcode = 0;
        std::string payload;

        // Status code of a close frame
        uint16_t close_code() const {
            return payload.size() >= 2 ? uint16_t((uint8_t(payload[0]) << 8) | uint8_t(payload[1])) : 0;
        }
    };

    // Frame as a client sends it (masked), or as a server does (mask = false)
    inline std::string make_frame(uint8_t opcode, std::string_view payload, bool fin = true, bool mask = true) {
        std::string frame;
        frame.push_back(char((fin ? 0x80 : 0x00) | opcode));
        uint8_t mask_bit = mask ? 0x80 : 0x00;
        if (payload.size() <= 125) {
            frame.push_back(char(mask_bit | payload.size()));
        } else if (payload.size() <= 65535) {
            frame.push_back(char(mask_bit | 126));
            frame.push_back(char((payload.size() >> 8) & 0xFF));
            frame.push_back(char(payload.size() & 0xFF));
        } else {
            frame.push_back(char(mask_bit | 127));
            for (int i = 7; i >= 0; --i) frame.push_back(char((uint64_t(payload.size()) >> (i * 8)) & 0xFF));
        }
        if (!mask) return frame.append(payload);
        static thread_local std::mt19937 engine{1234};
        uint8_t key[4];
        for (auto& byte : key) byte = uint8_t(engine());
        frame.append(reinterpret_cast<const char*>(key), 4);
        for (size_t i = 0; i < payload.size(); ++i) frame.push_back(char(payload[i] ^ key[i % 4]));
        return frame;
    }

    inline std::string close_payload(uint16_t code, std::string_view reason = "") {
        std::string payload{char(code >> 8), char(code & 0xFF)};
        return payload.append(reason);
    }

    // Raw WebSocket client over TCP: every read waits up to a timeout
    class raw_ws_client {
    public:
        explicit raw_ws_client(uint16_t port) : sock_(ioc_) {
            sock_.connect({net::ip::make_address("127.0.0.1"), port});
            sock_.set_option(tcp::no_delay(true));
        }

        // Send the handshake request (and `extra` bytes in the same write) and read the
        // response headers
        std::string handshake(const std::string& path, const std::string& headers = "", const std::string& extra = "") {
            std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" +
                headers + "\r\n";
            send_raw(request + extra);
            auto deadline = std::chrono::steady_clock::now() + 5s;
            while (buffer_.find("\r\n\r\n") == std::string::npos) {
                if (!read_more(deadline)) break;
            }
            auto end = buffer_.find("\r\n\r\n");
            if (end == std::string::npos) return std::exchange(buffer_, {});
            auto response = buffer_.substr(0, end + 4);
            buffer_.erase(0, end + 4);
            return response;
        }

        void send_raw(const std::string& data) {
            boost::system::error_code ec;
            net::write(sock_, net::buffer(data), ec);
        }

        void send_frame(uint8_t opcode, std::string_view payload, bool fin = true) {
            send_raw(make_frame(opcode, payload, fin));
        }

        // Next frame, if one arrives before the timeout
        std::optional<ws_frame> read_frame(std::chrono::milliseconds timeout = 5s) {
            auto deadline = std::chrono::steady_clock::now() + timeout;
            for (;;) {
                if (auto frame = parse()) return frame;
                if (!read_more(deadline)) return std::nullopt;
            }
        }

        // Payload of the data frames received until it holds `size` bytes, answering pings
        std::string read_data(size_t size, std::chrono::milliseconds timeout = 10s) {
            std::string data;
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (data.size() < size) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (left.count() <= 0) break;
                auto frame = read_frame(left);
                if (!frame) break;
                if (frame->opcode == OP_PING) {
                    send_frame(OP_PONG, frame->payload);
                } else if (frame->opcode == OP_CLOSE) {
                    break;
                } else if (frame->opcode < 0x8) {
                    data += frame->payload;
                }
            }
            return data;
        }

        // Whether the server closes the connection before the timeout (anything else received
        // is dropped)
        bool closed_within(std::chrono::milliseconds timeout) {
            buffer_.clear();
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!eof_) {
                if (!read_more(deadline) && !eof_) return false;
                buffer_.clear();
            }
            return true;
        }

        bool eof() const { return eof_; }

        void close() {
            boost::system::error_code ec;
            sock_.shutdown(tcp::socket::shutdown_both, ec);
            sock_.close(ec);
        }

        // Close the connection with a reset (RST), as a peer that is lost
        void reset() {
            boost::system::error_code ec;
            sock_.set_option(net::socket_base::linger(true, 0), ec);
            sock_.close(ec);
        }

    private:
        std::optional<ws_frame> parse() {
            if (buffer_.size() < 2) return std::nullopt;
            auto* data = reinterpret_cast<const uint8_t*>(buffer_.data());
            size_t length = data[1] & 0x7F;
            bool masked = data[1] & 0x80;
            size_t header = 2 + (length == 126 ? 2 : length == 127 ? 8 : 0) + (masked ? 4 : 0);
            if (buffer_.size() < header) return std::nullopt;
            uint64_t size = length;
            if (length == 126) size = (uint64_t(data[2]) << 8) | data[3];
            if (length == 127) {
                size = 0;
                for (int i = 0; i < 8; ++i) size = (size << 8) | data[2 + i];
            }
            if (buffer_.size() < header + size) return std::nullopt;
            ws_frame frame;
            frame.fin = data[0] & 0x80;
            frame.opcode = data[0] & 0x0F;
            frame.payload = buffer_.substr(header, size);
            if (masked) {
                for (size_t i = 0; i < size; ++i) frame.payload[i] ^= char(data[header - 4 + (i % 4)]);
            }
            buffer_.erase(0, header + size);
            return frame;
        }

        bool read_more(std::chrono::steady_clock::time_point deadline) {
            if (eof_) return false;
            auto timeout = deadline - std::chrono::steady_clock::now();
            if (timeout <= std::chrono::steady_clock::duration::zero()) return false;
            bool done = false;
            boost::system::error_code error;
            size_t bytes = 0;
            sock_.async_read_some(net::buffer(chunk_), [&](boost::system::error_code ec, size_t n) {
                error = ec;
                bytes = n;
                done = true;
            });
            ioc_.restart();
            ioc_.run_for(timeout);
            if (!done) {
                sock_.cancel();
                ioc_.restart();
                ioc_.run();
            }
            buffer_.append(chunk_.data(), bytes);
            if (error && error != net::error::operation_aborted) eof_ = true;
            return !error;
        }

        net::io_context ioc_;
        tcp::socket sock_;
        std::array<char, 65536> chunk_{};
        std::string buffer_;
        bool eof_ = false;
    };

    // Socket defined by the application: a virtual stream whose other end is somewhere else
    // (another thread, another connection), like a stream multiplexed inside another protocol.
    // Writes go to `on_write` (on the io_context of the socket); the other end hands data and
    // the end of the stream over with push() and push_eof(), from any thread.
    class memory_socket : public thinger::asio::socket, public std::enable_shared_from_this<memory_socket> {
    public:
        memory_socket(net::io_context& io_context, std::function<void(std::string)> on_write,
                      std::function<void()> on_close = {})
            : socket("memory", io_context), signal_(io_context), on_write_(std::move(on_write)),
              on_close_(std::move(on_close)) {}

        ~memory_socket() override { close(); }

        // the default overloads of read() and write(), hidden by the ones implemented here
        using socket::read;
        using socket::write;

        // From any thread: data written by the other end
        void push(std::string data) {
            net::post(io_context_, [self = shared_from_this(), data = std::move(data)] {
                self->inbox_ += data;
                self->signal_.cancel();
            });
        }

        // From any thread: the other end closed the stream
        void push_eof() {
            net::post(io_context_, [self = shared_from_this()] {
                self->eof_ = true;
                self->signal_.cancel();
            });
        }

        thinger::awaitable<thinger::io_result> read_some(uint8_t buffer[], size_t max_size) override {
            for (;;) {
                if (!open_) co_return thinger::io_result{net::error::bad_descriptor, 0};
                if (!inbox_.empty() || max_size == 0) {
                    size_t bytes = std::min(max_size, inbox_.size());
                    std::memcpy(buffer, inbox_.data(), bytes);
                    inbox_.erase(0, bytes);
                    co_return thinger::io_result{boost::system::error_code{}, bytes};
                }
                if (eof_) co_return thinger::io_result{net::error::eof, 0};

                // Wait for data, the end of the stream, close() or cancel()
                auto generation = cancel_generation_;
                signal_.expires_at(std::chrono::steady_clock::time_point::max());
                co_await signal_.async_wait(thinger::use_nothrow_awaitable);
                auto state = co_await net::this_coro::cancellation_state;
                if (cancel_generation_ != generation || state.cancelled() != net::cancellation_type::none) {
                    co_return thinger::io_result{net::error::operation_aborted, 0};
                }
            }
        }

        thinger::awaitable<thinger::io_result> write(const uint8_t buffer[], size_t size) override {
            if (!open_) co_return thinger::io_result{net::error::bad_descriptor, 0};
            if (on_write_) on_write_(std::string(reinterpret_cast<const char*>(buffer), size));
            co_return thinger::io_result{boost::system::error_code{}, size};
        }

        void close() override {
            if (!open_) return;
            open_ = false;
            signal_.cancel();
            if (on_close_) on_close_();
        }

        void cancel() override {
            ++cancel_generation_;
            signal_.cancel();
        }

        bool is_open() const override { return open_; }

    private:
        net::steady_timer signal_;
        std::function<void(std::string)> on_write_;
        std::function<void()> on_close_;
        std::string inbox_;
        bool eof_ = false;
        bool open_ = true;
        uint64_t cancel_generation_ = 0;
    };

    // Wait up to the timeout for a flag set by another thread
    inline bool wait_for(const std::atomic<bool>& flag, std::chrono::milliseconds timeout = 5s) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
        return flag;
    }

    // Deterministic data of `size` bytes
    inline std::string pattern(size_t size, unsigned seed = 0) {
        std::string data(size, '\0');
        std::mt19937 engine{seed};
        for (auto& byte : data) byte = char(engine());
        return data;
    }

}

#endif

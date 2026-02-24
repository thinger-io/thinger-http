#ifndef THINGER_ASIO_WEBSOCKET_HPP
#define THINGER_ASIO_WEBSOCKET_HPP
#pragma once

#include "socket.hpp"
#include <queue>
#include <string_view>

namespace thinger::asio {

class websocket : public socket {

public:

    static constexpr auto CONNECTION_TIMEOUT_SECONDS = std::chrono::seconds{60};
    static constexpr int MASK_SIZE_BYTES = 4;
    static std::atomic<unsigned long> connections;

    websocket(std::shared_ptr<socket> socket, bool binary = true, bool server = true);
    virtual ~websocket();

    // Socket control
    awaitable<boost::system::error_code> connect(
        const std::string &host,
        const std::string &port,
        std::chrono::seconds timeout) override;
    void close() override;
    awaitable<void> close_graceful();
    void cancel() override;
    bool requires_handshake() const override;
    awaitable<boost::system::error_code> handshake(const std::string& host = "") override;

    // Read operations
    awaitable<io_result> read_some(uint8_t buffer[], size_t max_size) override;
    awaitable<io_result> read(uint8_t buffer[], size_t size) override;
    awaitable<io_result> read(boost::asio::streambuf &buffer, size_t size) override;
    awaitable<io_result> read_until(boost::asio::streambuf &buffer, std::string_view delim) override;

    // Write operations
    awaitable<io_result> write(const uint8_t buffer[], size_t size) override;
    awaitable<io_result> write(std::string_view str) override;
    awaitable<io_result> write(const std::vector<boost::asio::const_buffer> &buffers) override;

    // Wait
    awaitable<boost::system::error_code> wait(boost::asio::socket_base::wait_type type) override;

    // WebSocket-specific operations
    awaitable<void> send_ping(uint8_t buffer[] = nullptr, size_t size = 0);
    awaitable<void> send_pong(uint8_t buffer[] = nullptr, size_t size = 0);

    // State getters
    bool is_open() const override;
    bool is_secure() const override;
    size_t available() const override;
    std::string get_remote_ip() const override;
    std::string get_local_port() const override;
    std::string get_remote_port() const override;
    std::size_t remaining_in_frame() const;
    bool is_message_complete() const;
    bool is_binary() const { return message_opcode_ == 0x2; }
    void set_binary(bool binary) { binary_ = binary; }

    // Timeout management
    void start_timeout();

private:
    // Internal helpers
    void unmask(uint8_t buffer[], size_t size);
    awaitable<size_t> read_frame(uint8_t buffer[], size_t max_size, boost::system::error_code& ec);
    awaitable<io_result> send_message(uint8_t opcode, const uint8_t buffer[], size_t size);
    awaitable<void> send_close(uint8_t buffer[] = nullptr, size_t size = 0);

    std::shared_ptr<socket> socket_;
    boost::asio::steady_timer timer_;
    bool binary_;
    bool server_role_;

    // Message state
    bool new_message_ = true;
    uint8_t message_opcode_ = 0;
    std::size_t frame_remaining_ = 0;

    // Frame state
    uint8_t opcode_ = 0;
    bool fin_ = false;
    bool masked_ = false;

    // Buffers
    uint8_t buffer_[140];
    uint8_t output_[14];
    uint8_t mask_[MASK_SIZE_BYTES];

    // Connection state
    bool close_received_ = false;
    bool close_sent_ = false;
    bool data_received_ = true;
    bool pending_ping_ = false;

    // Write synchronization (for write ordering)
    std::mutex write_mutex_;
};

}

#endif
#include "websocket.hpp"
#include "../../util/logger.hpp"
#include "tcp_socket.hpp"

#include <random>

namespace thinger::asio {

using random_bytes_engine = std::independent_bits_engine<std::default_random_engine, CHAR_BIT, unsigned char>;

std::atomic<unsigned long> websocket::connections{0};

websocket::websocket(std::shared_ptr<socket> sock, bool binary, bool server)
    : socket("websocket", sock->get_io_context())
    , socket_(sock)
    , timer_(sock->get_io_context())
    , binary_(binary)
    , server_role_(server) {
    ++connections;
    LOG_DEBUG("websocket created");
}

websocket::~websocket() {
    --connections;
    LOG_DEBUG("releasing websocket");
    timer_.cancel();
}

void websocket::unmask(uint8_t buffer[], std::size_t size) {
    LOG_DEBUG("unmasking payload. size: {}", size);
    for (size_t i = 0; i < size; ++i) {
        buffer[i] ^= mask_[i % MASK_SIZE_BYTES];
    }
}

void websocket::start_timeout() {
    timer_.expires_after(CONNECTION_TIMEOUT_SECONDS);
    timer_.async_wait([this](const boost::system::error_code& e) {
        if (e) {
            if (e != boost::asio::error::operation_aborted) {
                LOG_ERROR("error on timeout: {}", e.message());
            }
            return;
        }
        if (data_received_) {
            data_received_ = false;
            return start_timeout();
        } else {
            if (!pending_ping_) {
                pending_ping_ = true;
                co_spawn(io_context_, [this]() -> awaitable<void> {
                    try {
                        co_await send_ping();
                        start_timeout();
                    } catch (...) {
                        close();
                    }
                }, detached);
            } else {
                LOG_DEBUG("websocket ping timeout... closing connection!");
                close();
            }
        }
    });
}

awaitable<void> websocket::send_close(uint8_t buffer[], size_t size) {
    LOG_DEBUG("sending close frame");
    co_await send_message(0x88, buffer, size);
    close_sent_ = true;
    LOG_DEBUG("close frame sent");

    if (!close_received_) {
        // Race: read the close ack vs timeout
        timer_.cancel();
        timer_.expires_after(std::chrono::seconds{5});

        auto read_close_ack = [this]() -> awaitable<void> {
            try {
                uint8_t buf[125];
                while (!close_received_ && socket_->is_open()) {
                    co_await read_frame(buf, sizeof(buf));
                }
            } catch (...) {
                // Expected: read_frame throws after processing close frame
            }
        };

        auto wait_timeout = [this]() -> awaitable<void> {
            auto [ec] = co_await timer_.async_wait(use_nothrow_awaitable);
        };

        co_await (read_close_ack() || wait_timeout());

        if (!close_received_) {
            LOG_WARNING("timeout while waiting close acknowledgement");
        }
    }
    close();
}

awaitable<void> websocket::send_ping(uint8_t buffer[], size_t size) {
    LOG_DEBUG("sending ping frame");
    co_await send_message(0x09, buffer, size);
    LOG_DEBUG("ping frame sent");
}

awaitable<void> websocket::send_pong(uint8_t buffer[], size_t size) {
    LOG_DEBUG("sending pong frame");
    co_await send_message(0x0A, buffer, size);
    LOG_DEBUG("pong frame sent");
}

awaitable<size_t> websocket::read_frame(uint8_t buffer[], size_t max_size) {
    // If there's remaining data in current frame, read it
    if (frame_remaining_ > 0) {
        auto read_size = std::min(frame_remaining_, max_size);
        auto bytes = co_await socket_->read(buffer, read_size);

        if (masked_) unmask(buffer, bytes);
        frame_remaining_ -= bytes;

        co_return bytes;
    }

    // Read frame header (2 bytes minimum)
    co_await socket_->read(buffer_, 2);
    data_received_ = true;

    uint8_t data_type = buffer_[0];
    uint8_t fin = data_type & 0b10000000;
    uint8_t rsv = data_type & 0b01110000;

    if (rsv) {
        LOG_ERROR("invalid RSV parameters");
        throw boost::system::system_error(boost::asio::error::invalid_argument);
    }

    uint8_t opcode = data_type & 0x0F;
    uint8_t data_size = buffer_[1] & ~(1 << 7);
    uint8_t masked = buffer_[1] & 0b10000000;

    LOG_DEBUG("decoded frame header. fin: {}, opcode: 0x{:02X} mask: {} data_size: {}", fin, opcode, masked, data_size);

    if (!masked && server_role_) {
        LOG_ERROR("client is not masking the information");
        throw boost::system::system_error(boost::asio::error::invalid_argument);
    }

    masked_ = masked;
    fin_ = fin;
    opcode_ = opcode;

    // Handle opcodes
    if (new_message_) {
        switch (opcode) {
            case 0x0:
                LOG_ERROR("received continuation message as the first message!");
                throw boost::system::system_error(boost::asio::error::invalid_argument);
            case 0x1: // text
            case 0x2: // binary
                message_opcode_ = opcode;
                break;
            case 0x8: // close
            case 0x9: // ping
            case 0xA: // pong
                if (!fin) {
                    LOG_ERROR("control frame messages cannot be fragmented");
                    throw boost::system::system_error(boost::asio::error::invalid_argument);
                }
                break;
            default:
                LOG_ERROR("received unknown websocket opcode: {}", (int)opcode);
                throw boost::system::system_error(boost::asio::error::invalid_argument);
        }
    } else {
        // Continuation frame expected
        if (opcode != 0x0 && opcode < 0x8) {
            LOG_ERROR("unexpected fragment type. expecting a continuation frame");
            throw boost::system::system_error(boost::asio::error::invalid_argument);
        }
    }

    // Determine payload length
    uint64_t payload_size = data_size;
    if (data_size == 126) {
        co_await socket_->read(buffer_, 2);
        payload_size = (buffer_[0] << 8) | buffer_[1];
    } else if (data_size == 127) {
        co_await socket_->read(buffer_, 8);
        payload_size = 0;
        for (int i = 0; i < 8; ++i) {
            payload_size = (payload_size << 8) | buffer_[i];
        }
    }

    frame_remaining_ = payload_size;

    // Read mask if present
    if (masked_) {
        co_await socket_->read(mask_, MASK_SIZE_BYTES);
    }

    // Handle control frames
    if (opcode >= 0x8) {
        // Read control frame payload
        uint8_t control_buffer[125];
        size_t control_size = std::min(static_cast<size_t>(payload_size), size_t(125));
        if (control_size > 0) {
            co_await socket_->read(control_buffer, control_size);
            if (masked_) unmask(control_buffer, control_size);
        }
        frame_remaining_ = 0;

        switch (opcode) {
            case 0x8: // close
                LOG_DEBUG("received close frame");
                close_received_ = true;
                if (!close_sent_) {
                    co_await send_close();
                }
                throw boost::system::system_error(boost::asio::error::connection_aborted);
            case 0x9: // ping
                LOG_DEBUG("received ping frame");
                co_await send_pong(control_buffer, control_size);
                co_return co_await read_frame(buffer, max_size);
            case 0xA: // pong
                LOG_DEBUG("received pong frame");
                pending_ping_ = false;
                data_received_ = false;
                co_return co_await read_frame(buffer, max_size);
        }
    }

    // Read data frame payload
    if (frame_remaining_ == 0) {
        if (fin_) new_message_ = true;
        co_return 0;
    }

    auto read_size = std::min(frame_remaining_, max_size);
    auto bytes = co_await socket_->read(buffer, read_size);

    if (masked_) unmask(buffer, bytes);
    frame_remaining_ -= bytes;

    if (frame_remaining_ == 0 && fin_) {
        new_message_ = true;
    } else if (frame_remaining_ == 0) {
        new_message_ = false;
    }

    co_return bytes;
}

awaitable<size_t> websocket::send_message(uint8_t opcode, const uint8_t buffer[], size_t size) {
    std::lock_guard<std::mutex> lock(write_mutex_);

    uint8_t header_size = 2;
    output_[0] = 0x80 | opcode;

    if (size <= 125) {
        output_[1] = size;
    } else if (size <= 65535) {
        output_[1] = 126;
        output_[2] = (size >> 8) & 0xff;
        output_[3] = size & 0xff;
        header_size += 2;
    } else {
        output_[1] = 127;
        for (int i = 0; i < 8; ++i) {
            output_[2 + i] = (size >> ((7 - i) * 8)) & 0xff;
        }
        header_size += 8;
    }

    // Create output buffers
    std::vector<boost::asio::const_buffer> output_buffers;
    output_buffers.push_back(boost::asio::buffer(output_, header_size));

    // Handle masking for client role
    std::vector<uint8_t> masked_data;
    if (!server_role_) {
        output_[1] |= 0b10000000;

        static random_bytes_engine rbe;
        uint8_t mask[MASK_SIZE_BYTES];
        for (int i = 0; i < MASK_SIZE_BYTES; ++i) {
            mask[i] = rbe();
            output_[header_size + i] = mask[i];
        }
        header_size += MASK_SIZE_BYTES;

        // Mask the data
        masked_data.resize(size);
        for (size_t i = 0; i < size; ++i) {
            masked_data[i] = buffer[i] ^ mask[i % MASK_SIZE_BYTES];
        }

        output_buffers.clear();
        output_buffers.push_back(boost::asio::buffer(output_, header_size));
        output_buffers.push_back(boost::asio::buffer(masked_data));
    } else {
        output_buffers.push_back(boost::asio::buffer(buffer, size));
    }

    LOG_DEBUG("sending websocket data. header: {}, payload: {}", header_size, size);

    auto bytes = co_await socket_->write(output_buffers);
    co_return bytes - header_size;
}

awaitable<size_t> websocket::read_some(uint8_t buffer[], size_t max_size) {
    co_return co_await read_frame(buffer, max_size);
}

awaitable<size_t> websocket::read(uint8_t buffer[], size_t size) {
    co_return co_await read_frame(buffer, size);
}

awaitable<size_t> websocket::read(boost::asio::streambuf& buffer, size_t size) {
    throw boost::system::system_error(boost::asio::error::operation_not_supported);
}

awaitable<size_t> websocket::read_until(boost::asio::streambuf& buffer, std::string_view delim) {
    throw boost::system::system_error(boost::asio::error::operation_not_supported);
}

awaitable<size_t> websocket::write(const uint8_t buffer[], size_t size) {
    co_return co_await send_message(binary_ ? 0x02 : 0x01, buffer, size);
}

awaitable<size_t> websocket::write(std::string_view str) {
    co_return co_await send_message(binary_ ? 0x02 : 0x01,
        reinterpret_cast<const uint8_t*>(str.data()), str.size());
}

awaitable<size_t> websocket::write(const std::vector<boost::asio::const_buffer>& buffers) {
    throw boost::system::system_error(boost::asio::error::operation_not_supported);
}

awaitable<void> websocket::wait(boost::asio::socket_base::wait_type type) {
    co_await socket_->wait(type);
}

awaitable<void> websocket::connect(
    const std::string& host,
    const std::string& port,
    std::chrono::seconds timeout) {
    co_await socket_->connect(host, port, timeout);
}

void websocket::close() {
    timer_.cancel();
    socket_->close();
}

awaitable<void> websocket::close_graceful() {
    if (!close_sent_ && socket_->is_open()) {
        co_await send_close();
    } else if (socket_->is_open()) {
        close();
    }
}

void websocket::cancel() {
    socket_->cancel();
}

bool websocket::requires_handshake() const {
    return socket_->requires_handshake();
}

awaitable<void> websocket::handshake(const std::string& host) {
    co_await socket_->handshake(host);
}

bool websocket::is_open() const {
    return socket_->is_open();
}

bool websocket::is_secure() const {
    return socket_->is_secure();
}

size_t websocket::available() const {
    return socket_->available();
}

std::string websocket::get_remote_ip() const {
    return socket_->get_remote_ip();
}

std::string websocket::get_local_port() const {
    return socket_->get_local_port();
}

std::string websocket::get_remote_port() const {
    return socket_->get_remote_port();
}

std::size_t websocket::remaining_in_frame() const {
    return frame_remaining_;
}

bool websocket::is_message_complete() const {
    return new_message_;
}

}

#include "websocket.hpp"
#include "../io_context_of.hpp"
#include "../../util/logger.hpp"

#include <algorithm>
#include <cstring>
#include <random>
#ifdef THINGER_HTTP_SSL_ENABLED
#include <openssl/rand.h>
#endif

namespace thinger::asio {

namespace {

// Bytes of the input buffer; larger payloads are read straight into the reader's buffer
constexpr size_t INPUT_BUFFER_SIZE = 4096;
constexpr size_t DIRECT_READ_SIZE = 1024;

constexpr uint8_t OPCODE_CONTINUATION = 0x0;
constexpr uint8_t OPCODE_TEXT = 0x1;
constexpr uint8_t OPCODE_BINARY = 0x2;
constexpr uint8_t OPCODE_CLOSE = 0x8;
constexpr uint8_t OPCODE_PING = 0x9;
constexpr uint8_t OPCODE_PONG = 0xA;

constexpr size_t MAX_CONTROL_PAYLOAD = 125;

// Close codes that may be sent back (RFC 6455, 7.4, and the IANA registry up to 1014, as Node
// ws and Go accept)
bool valid_close_code(uint16_t code) {
    if (code >= 3000 && code <= 4999) return true;
    return code >= 1000 && code <= 1014 && code != 1004 && code != 1005 && code != 1006;
}

// Mask keys must be unpredictable (RFC 6455, 5.3): taken from the CSPRNG of OpenSSL (a few
// bytes per frame sent by a client), or from std::random_device without it
void random_mask(uint8_t mask[websocket::MASK_SIZE_BYTES]) {
#ifdef THINGER_HTTP_SSL_ENABLED
    if (RAND_bytes(mask, websocket::MASK_SIZE_BYTES) == 1) return;
#endif
    thread_local std::random_device device;
    uint32_t value = device();
    std::memcpy(mask, &value, websocket::MASK_SIZE_BYTES);
}

// The code to send in a close frame: 1000 for one that may not be sent
uint16_t sendable_close_code(uint16_t code) {
    if (valid_close_code(code)) return code;
    LOG_WARNING("websocket close code {} may not be sent, closing with 1000", code);
    return websocket::CLOSE_NORMAL;
}

boost::system::error_code protocol_error() {
    return boost::system::errc::make_error_code(boost::system::errc::protocol_error);
}

}

std::atomic<unsigned long> websocket::connections{0};

websocket::websocket(std::shared_ptr<socket> sock, bool binary, bool server, boost::asio::any_io_executor executor)
    : socket("websocket", sock->get_io_context())
    , socket_(std::move(sock))
    , executor_(executor ? std::move(executor) : boost::asio::any_io_executor(io_context_.get_executor()))
    , binary_(binary)
    , server_role_(server)
    , in_(INPUT_BUFFER_SIZE)
    , close_timer_(std::make_shared<boost::asio::steady_timer>(executor_))
    , keepalive_timer_(std::make_shared<boost::asio::steady_timer>(executor_))
    , last_received_(std::chrono::steady_clock::now()) {
    ++connections;
    LOG_DEBUG("websocket created");
}

websocket::~websocket() {
    --connections;
    LOG_DEBUG("releasing websocket");
    // A websocket released open on its executor says goodbye to its peer. Anywhere else no
    // I/O is started, and nothing shared with the executor is touched: the socket closes the
    // connection once released
    if (!closed_ && !writing_ && running_in_this_thread(executor_)) {
        close(CLOSE_GOING_AWAY);
    }
}

void websocket::set_read_ahead(std::string data) {
    if (data.size() > in_.size()) in_.resize(data.size());
    std::memcpy(in_.data(), data.data(), data.size());
    in_begin_ = 0;
    in_end_ = data.size();
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

bool websocket::parse_header(const uint8_t* data, size_t size, frame_header& header) {
    header.size = 2;
    if (size < 2) return false;
    uint8_t length = data[1] & 0x7F;
    header.masked = data[1] & 0x80;
    header.size += (length == 126 ? 2 : length == 127 ? 8 : 0) + (header.masked ? MASK_SIZE_BYTES : 0);
    if (size < header.size) return false;

    header.fin = data[0] & 0x80;
    header.rsv = data[0] & 0x70;
    header.opcode = data[0] & 0x0F;
    size_t offset = 2;
    if (length == 126) {
        header.payload_size = (uint64_t(data[2]) << 8) | data[3];
        offset += 2;
    } else if (length == 127) {
        header.payload_size = 0;
        for (int i = 0; i < 8; ++i) header.payload_size = (header.payload_size << 8) | data[2 + i];
        offset += 8;
    } else {
        header.payload_size = length;
    }
    if (header.masked) std::memcpy(header.mask, data + offset, MASK_SIZE_BYTES);
    return true;
}

size_t websocket::build_header(uint8_t opcode, uint64_t payload_size, uint8_t header[14],
                               uint8_t mask[MASK_SIZE_BYTES]) const {
    size_t size = 2;
    header[0] = 0x80 | opcode;
    if (payload_size <= 125) {
        header[1] = static_cast<uint8_t>(payload_size);
    } else if (payload_size <= 65535) {
        header[1] = 126;
        header[2] = (payload_size >> 8) & 0xFF;
        header[3] = payload_size & 0xFF;
        size += 2;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; ++i) header[2 + i] = (payload_size >> ((7 - i) * 8)) & 0xFF;
        size += 8;
    }
    // Clients mask what they send (RFC 6455, 5.3)
    if (!server_role_) {
        header[1] |= 0x80;
        random_mask(mask);
        std::memcpy(header + size, mask, MASK_SIZE_BYTES);
        size += MASK_SIZE_BYTES;
    }
    return size;
}

void websocket::apply_mask(uint8_t* data, size_t size, const uint8_t mask[MASK_SIZE_BYTES], size_t offset) {
    // The key from `offset`, repeated over a word
    uint8_t key[8];
    for (size_t i = 0; i < sizeof(key); ++i) key[i] = mask[(offset + i) % MASK_SIZE_BYTES];
    uint64_t word_key;
    std::memcpy(&word_key, key, sizeof(word_key));
    size_t i = 0;
    for (; i + sizeof(word_key) <= size; i += sizeof(word_key)) {
        uint64_t word;
        std::memcpy(&word, data + i, sizeof(word));
        word ^= word_key;
        std::memcpy(data + i, &word, sizeof(word));
    }
    for (; i < size; ++i) data[i] ^= key[i % sizeof(key)];
}

void websocket::unmask(uint8_t buffer[], size_t size) {
    apply_mask(buffer, size, mask_, mask_offset_);
    mask_offset_ = (mask_offset_ + size) % MASK_SIZE_BYTES;
}

// ---------------------------------------------------------------------------
// Read side
// ---------------------------------------------------------------------------

awaitable<boost::system::error_code> websocket::fill(size_t size) {
    while (buffered() < size) {
        if (buffered() == 0) {
            in_begin_ = in_end_ = 0;
        } else if (in_.size() - in_begin_ < size) {
            std::memmove(in_.data(), in_.data() + in_begin_, buffered());
            in_end_ -= in_begin_;
            in_begin_ = 0;
        }
        auto [ec, bytes] = co_await socket_->read_some(in_.data() + in_end_, in_.size() - in_end_);
        // closed meanwhile: the socket is not this websocket's anymore
        if (closed_) co_return boost::asio::error::operation_aborted;
        if (ec) co_return ec;
        if (bytes == 0) co_return boost::asio::error::eof;   // a socket breaking its guarantees
        in_end_ += bytes;
        last_received_ = std::chrono::steady_clock::now();
    }
    co_return boost::system::error_code{};
}

awaitable<io_result> websocket::read_payload(uint8_t buffer[], size_t max_size) {
    auto wanted = static_cast<size_t>(std::min<uint64_t>(max_size, frame_remaining_));
    size_t bytes;
    if (buffered() == 0 && wanted >= DIRECT_READ_SIZE) {
        // Large reads go straight to the reader's buffer, never past this frame
        auto [ec, read] = co_await socket_->read_some(buffer, wanted);
        if (closed_) co_return io_result{boost::asio::error::operation_aborted, 0};
        if (ec) co_return io_result{ec, 0};
        if (read == 0) co_return io_result{boost::asio::error::eof, 0};   // a socket breaking its guarantees
        last_received_ = std::chrono::steady_clock::now();
        bytes = read;
    } else {
        if (auto ec = co_await fill(1)) co_return io_result{ec, 0};
        bytes = std::min(wanted, buffered());
        std::memcpy(buffer, in_.data() + in_begin_, bytes);
        in_begin_ += bytes;
    }
    if (frame_masked_) unmask(buffer, bytes);
    frame_remaining_ -= bytes;
    if (frame_remaining_ == 0 && frame_fin_) new_message_ = true;
    co_return io_result{boost::system::error_code{}, bytes};
}

boost::system::error_code websocket::fail(const char* reason) {
    LOG_WARNING("websocket protocol error: {}", reason);
    close(CLOSE_PROTOCOL_ERROR);
    return protocol_error();
}

awaitable<boost::system::error_code> websocket::handle_control(const frame_header& header) {
    uint8_t payload[MAX_CONTROL_PAYLOAD];
    size_t size = header.payload_size;
    std::memcpy(payload, in_.data() + in_begin_ + header.size, size);
    if (header.masked) apply_mask(payload, size, header.mask, 0);
    in_begin_ += header.size + size;

    switch (header.opcode) {
        case OPCODE_PING: {
            LOG_DEBUG("received ping frame");
            if (close_sent_) co_return boost::system::error_code{};
            if (weak_from_this().expired()) {
                // Not owned by a std::shared_ptr: answered before reading on
                co_await send_frame(OPCODE_PONG, {boost::asio::buffer(payload, size)}, true);
                if (closed_) co_return boost::asio::error::operation_aborted;
                co_return boost::system::error_code{};
            }
            // Answered as soon as no frame is being written, while reading goes on (a pong may
            // answer only the last ping)
            pending_pong_.emplace(reinterpret_cast<const char*>(payload), size);
            queue_control();
            co_return boost::system::error_code{};
        }
        case OPCODE_PONG:
            LOG_DEBUG("received pong frame");
            co_return boost::system::error_code{};
        case OPCODE_CLOSE: {
            LOG_DEBUG("received close frame");
            if (size == 1) co_return fail("invalid close frame payload");
            close_received_ = true;
            uint16_t code = size >= 2 ? uint16_t((payload[0] << 8) | payload[1]) : CLOSE_NORMAL;
            // Answered (unless this end closed first), and the connection closed: the server
            // closes it first, and a client does not need to wait for it
            close(valid_close_code(code) ? code : CLOSE_PROTOCOL_ERROR, false);
            co_return boost::asio::error::eof;
        }
        default:
            co_return fail("unknown control frame opcode");
    }
}

awaitable<io_result> websocket::read_frame_data(uint8_t buffer[], size_t max_size, bool skip_empty) {
    for (;;) {
        if (close_received_) co_return io_result{boost::asio::error::eof, 0};
        if (closed_) co_return io_result{boost::asio::error::bad_descriptor, 0};

        // Payload of the data frame being received
        if (frame_remaining_ > 0) {
            if (max_size == 0) co_return io_result{boost::system::error_code{}, 0};
            co_return co_await read_payload(buffer, max_size);
        }

        // Next frame header
        frame_header header;
        while (!parse_header(in_.data() + in_begin_, buffered(), header)) {
            if (auto ec = co_await fill(header.size)) co_return io_result{ec, 0};
        }

        if (header.rsv) co_return io_result{fail("reserved bits set without an extension"), 0};
        if (server_role_ && !header.masked) co_return io_result{fail("unmasked frame from a client"), 0};
        if (!server_role_ && header.masked) co_return io_result{fail("masked frame from a server"), 0};
        if (header.payload_size >> 63) co_return io_result{fail("invalid payload length"), 0};

        // Control frames, which may come between the frames of a message
        if (header.opcode >= 0x8) {
            if (!header.fin) co_return io_result{fail("fragmented control frame"), 0};
            if (header.payload_size > MAX_CONTROL_PAYLOAD) co_return io_result{fail("control frame too large"), 0};
            if (auto ec = co_await fill(header.size + header.payload_size)) co_return io_result{ec, 0};
            if (auto ec = co_await handle_control(header)) co_return io_result{ec, 0};
            continue;
        }

        // Data frames: a text or binary frame starts a message, continuation frames go on
        switch (header.opcode) {
            case OPCODE_CONTINUATION:
                if (new_message_) co_return io_result{fail("continuation frame without a message"), 0};
                break;
            case OPCODE_TEXT:
            case OPCODE_BINARY:
                if (!new_message_) co_return io_result{fail("new message before the previous one ended"), 0};
                message_opcode_ = header.opcode;
                break;
            default:
                co_return io_result{fail("unknown data frame opcode"), 0};
        }

        in_begin_ += header.size;
        frame_remaining_ = header.payload_size;
        frame_fin_ = header.fin;
        frame_masked_ = header.masked;
        std::memcpy(mask_, header.mask, MASK_SIZE_BYTES);
        mask_offset_ = 0;
        new_message_ = false;

        if (frame_remaining_ == 0) {
            if (frame_fin_) new_message_ = true;
            if (!skip_empty) co_return io_result{boost::system::error_code{}, 0};
        }
    }
}

awaitable<io_result> websocket::read_some(uint8_t buffer[], size_t max_size) {
    if (max_size == 0) co_return io_result{closed_ ? boost::asio::error::bad_descriptor : boost::system::error_code{}, 0};
    reading_ = true;
    auto result = co_await read_frame_data(buffer, max_size, true);
    reading_ = false;
    co_return result;
}

awaitable<io_result> websocket::read_message_some(uint8_t buffer[], size_t max_size) {
    reading_ = true;
    auto result = co_await read_frame_data(buffer, max_size, false);
    reading_ = false;
    co_return result;
}

// ---------------------------------------------------------------------------
// Write side
// ---------------------------------------------------------------------------

awaitable<bool> websocket::acquire_write(bool control) {
    if (closed_) co_return false;
    if (!writing_) {
        writing_ = true;
        co_return true;
    }
    // Wait for the turn: control frames go before the data waiting, after other control frames
    auto waiter = std::make_shared<write_waiter>(executor_);
    waiter->timer.expires_at(std::chrono::steady_clock::time_point::max());
    if (control) {
        auto position = std::find_if(write_waiters_.begin(), write_waiters_.end(),
                                     [](const auto& other) { return !other->control; });
        waiter->control = true;
        write_waiters_.insert(position, waiter);
    } else {
        write_waiters_.push_back(waiter);
    }
    co_await waiter->timer.async_wait(use_nothrow_awaitable);
    if (waiter->granted) co_return true;
    // closed, cancelled, or the wait itself cancelled
    auto it = std::find(write_waiters_.begin(), write_waiters_.end(), waiter);
    if (it != write_waiters_.end()) write_waiters_.erase(it);
    co_return false;
}

void websocket::release_write() {
    if (closed_) {
        // the closing handshake waited for the write in progress
        writing_ = false;
        if (linger_pending_) start_linger();
        return;
    }
    if (pending_pong_ || pending_ping_) {
        // the turn goes to the control frames queued: writing_ stays set
        if (auto self = weak_from_this().lock()) {
            co_spawn(executor_, [self]() -> awaitable<void> { co_await self->flush_control(); }, detached);
            return;
        }
        pending_pong_.reset();
        pending_ping_ = false;
    }
    if (!write_waiters_.empty()) {
        // the turn passes to the next writer: writing_ stays set
        auto next = std::move(write_waiters_.front());
        write_waiters_.pop_front();
        next->granted = true;
        next->timer.cancel();
        return;
    }
    writing_ = false;
}

void websocket::wake_writers() {
    auto waiters = std::move(write_waiters_);
    write_waiters_.clear();
    for (auto& waiter : waiters) waiter->timer.cancel();
}

void websocket::queue_control() {
    if (writing_) return;   // sent when the turn is released
    writing_ = true;
    release_write();
}

awaitable<void> websocket::flush_control() {
    while (!closed_ && (pending_pong_ || pending_ping_)) {
        boost::system::error_code ec;
        if (pending_pong_) {
            auto payload = std::move(*pending_pong_);
            pending_pong_.reset();
            ec = co_await write_frame(OPCODE_PONG, {boost::asio::buffer(payload)});
        } else {
            pending_ping_ = false;
            ping_in_flight_ = true;
            ec = co_await write_frame(OPCODE_PING, {});
            ping_in_flight_ = false;
            // a ping that cannot be sent leaves nothing to keep alive (cancel() aside)
            if (ec && ec != boost::asio::error::operation_aborted && !closed_) {
                LOG_DEBUG("websocket keepalive ping failed: {}", ec.message());
                close(CLOSE_GOING_AWAY);
            }
        }
        if (ec) break;
    }
    release_write();
}

awaitable<boost::system::error_code> websocket::write_frame(uint8_t opcode,
                                                            const std::vector<boost::asio::const_buffer>& payload) {
    // The rest of an interrupted frame goes first
    if (!tail_.empty()) {
        auto [ec, bytes] = co_await socket_->write(tail_.data(), tail_.size());
        tail_.erase(tail_.begin(), tail_.begin() + static_cast<std::ptrdiff_t>(std::min(bytes, tail_.size())));
        if (ec) {
            if (ec != boost::asio::error::operation_aborted) write_error_ = ec;
            co_return ec;
        }
    }
    if (opcode == OPCODE_CLOSE) close_sent_ = true;

    size_t size = boost::asio::buffer_size(payload);
    uint8_t header[14];
    uint8_t mask[MASK_SIZE_BYTES];
    size_t header_size = build_header(opcode, size, header, mask);

    std::vector<boost::asio::const_buffer> output;
    output.reserve(payload.size() + 1);
    output.emplace_back(header, header_size);
    std::vector<uint8_t> masked;
    if (server_role_) {
        output.insert(output.end(), payload.begin(), payload.end());
    } else if (size > 0) {
        masked.resize(size);
        boost::asio::buffer_copy(boost::asio::buffer(masked), payload);
        apply_mask(masked.data(), size, mask, 0);
        output.emplace_back(masked.data(), masked.size());
    }

    auto [ec, bytes] = co_await socket_->write(output);
    if (ec) {
        if (ec != boost::asio::error::operation_aborted) {
            write_error_ = ec;
        } else if (bytes > 0) {
            // Interrupted in the middle of the frame: its rest is sent before anything else
            size_t total = header_size + size;
            tail_.resize(total - bytes);
            auto rest = boost::asio::buffer(tail_);
            size_t skip = bytes;
            for (const auto& buffer : output) {
                if (skip >= buffer.size()) {
                    skip -= buffer.size();
                    continue;
                }
                rest += boost::asio::buffer_copy(rest, buffer + skip);
                skip = 0;
            }
        }
    }
    co_return ec;
}

awaitable<io_result> websocket::send_frame(uint8_t opcode, const std::vector<boost::asio::const_buffer>& payload,
                                           bool control) {
    if (!co_await acquire_write(control)) {
        co_return io_result{closed_ ? boost::asio::error::bad_descriptor : boost::asio::error::operation_aborted, 0};
    }

    boost::system::error_code error;
    if (closed_) {
        error = boost::asio::error::bad_descriptor;
    } else if (write_error_) {
        error = write_error_;
    } else if (close_sent_ || close_received_) {
        // nothing is sent after a close frame
        error = boost::asio::error::broken_pipe;
    }
    if (error) {
        release_write();
        co_return io_result{error, 0};
    }

    auto ec = co_await write_frame(opcode, payload);
    release_write();
    co_return io_result{ec, ec ? 0 : boost::asio::buffer_size(payload)};
}

awaitable<boost::system::error_code> websocket::send_close(uint16_t code) {
    LOG_DEBUG("sending close frame: {}", code);
    uint8_t payload[2] = {uint8_t(code >> 8), uint8_t(code & 0xFF)};
    auto [ec, bytes] = co_await send_frame(OPCODE_CLOSE, {boost::asio::buffer(payload)}, true);
    co_return ec;
}

awaitable<void> websocket::send_ping(uint8_t buffer[], size_t size) {
    co_await send_frame(OPCODE_PING, {boost::asio::buffer(buffer, std::min(size, MAX_CONTROL_PAYLOAD))}, true);
}

awaitable<void> websocket::send_pong(uint8_t buffer[], size_t size) {
    co_await send_frame(OPCODE_PONG, {boost::asio::buffer(buffer, std::min(size, MAX_CONTROL_PAYLOAD))}, true);
}

awaitable<io_result> websocket::write(const uint8_t buffer[], size_t size) {
    co_return co_await write(std::vector<boost::asio::const_buffer>{boost::asio::buffer(buffer, size)});
}

awaitable<io_result> websocket::write(std::string_view str) {
    co_return co_await write(std::vector<boost::asio::const_buffer>{boost::asio::buffer(str.data(), str.size())});
}

awaitable<io_result> websocket::write(const std::vector<boost::asio::const_buffer>& buffers) {
    // As a stream, writing nothing sends nothing (not an empty message)
    if (boost::asio::buffer_size(buffers) == 0) {
        co_return io_result{closed_ ? boost::asio::error::bad_descriptor : boost::system::error_code{}, 0};
    }
    co_return co_await send_frame(binary_ ? OPCODE_BINARY : OPCODE_TEXT, buffers, false);
}

// ---------------------------------------------------------------------------
// Closing
// ---------------------------------------------------------------------------

void websocket::close() {
    close(CLOSE_NORMAL);
}

void websocket::close(uint16_t code, bool expect_reply) {
    if (closed_) return;
    code = sendable_close_code(code);
    closed_ = true;
    ++keepalive_generation_;
    keepalive_timer_->cancel();
    close_timer_->cancel();
    wake_writers();
    pending_pong_.reset();
    pending_ping_ = false;

    // The closing handshake runs on the executor: called anywhere else (e.g. by a synchronous
    // client between its calls, while nothing runs its io_context) the connection is closed now
    if (close_sent_ || write_error_ || !socket_->is_open() || !running_in_this_thread(executor_)) {
        socket_->close();
        return;
    }
    close_sent_ = true;
    close_code_ = code;
    linger_expect_reply_ = expect_reply;

    // Pending operations end now. A write in progress leaves the rest of its frame, sent
    // before the close frame once it returns the turn
    socket_->cancel();
    if (writing_) {
        linger_pending_ = true;
    } else {
        start_linger();
    }
}

void websocket::start_linger() {
    linger_pending_ = false;
    std::vector<uint8_t> output = std::move(tail_);
    tail_.clear();
    uint8_t header[14];
    uint8_t mask[MASK_SIZE_BYTES];
    size_t header_size = build_header(OPCODE_CLOSE, 2, header, mask);
    output.insert(output.end(), header, header + header_size);
    uint8_t payload[2] = {uint8_t(close_code_ >> 8), uint8_t(close_code_ & 0xFF)};
    if (!server_role_) apply_mask(payload, sizeof(payload), mask, 0);
    output.insert(output.end(), payload, payload + sizeof(payload));

    // The closing handshake goes on without this websocket, on its socket, with what was
    // received but not read yet
    std::string input(reinterpret_cast<const char*>(in_.data() + in_begin_), buffered());
    co_spawn(executor_, linger(socket_, std::move(output), std::move(input), frame_remaining_, linger_expect_reply_),
             detached);
}

awaitable<void> websocket::linger(std::shared_ptr<socket> socket, std::vector<uint8_t> output,
                                  std::string input, uint64_t skip, bool expect_reply) {
    // Close the socket after the timeout, whatever is pending on it: on the executor of the
    // websocket, as the operations it ends
    auto timer = std::make_shared<boost::asio::steady_timer>(co_await boost::asio::this_coro::executor, CLOSE_TIMEOUT);
    timer->async_wait([socket](const boost::system::error_code& ec) {
        if (!ec) socket->close();
    });

    auto [write_ec, written] = co_await socket->write(output.data(), output.size());
    if (!write_ec && expect_reply) {
        // Wait for the close frame of the peer, skipping anything else
        uint8_t chunk[1024];
        for (;;) {
            auto skipped = std::min<uint64_t>(skip, input.size());
            input.erase(0, skipped);
            skip -= skipped;
            if (skip == 0) {
                frame_header header;
                if (parse_header(reinterpret_cast<const uint8_t*>(input.data()), input.size(), header)) {
                    if (header.opcode == OPCODE_CLOSE) break;
                    input.erase(0, header.size);
                    skip = header.payload_size;
                    continue;
                }
            }
            auto [ec, bytes] = co_await socket->read_some(chunk, sizeof(chunk));
            if (ec || bytes == 0) break;
            input.append(reinterpret_cast<const char*>(chunk), bytes);
        }
    }
    timer->cancel();
    socket->close();
}

awaitable<void> websocket::close_graceful(uint16_t code) {
    if (closed_) co_return;
    code = sendable_close_code(code);
    if (!close_sent_ && !close_received_) {
        if (co_await send_close(code)) {
            close(code);
            co_return;
        }
    }
    if (closed_) co_return;
    if (!close_received_) {
        close_timer_->expires_after(CLOSE_TIMEOUT);
        if (reading_) {
            // The reader gets the close frame of the peer, and closes (which ends this wait)
            co_await close_timer_->async_wait(use_nothrow_awaitable);
        } else {
            // Read until the close frame of the peer, discarding anything else
            auto discard = [this]() -> awaitable<void> {
                uint8_t buffer[512];
                for (;;) {
                    auto [ec, bytes] = co_await read_message_some(buffer, sizeof(buffer));
                    if (ec) co_return;
                }
            };
            co_await (discard() || close_timer_->async_wait(use_nothrow_awaitable));
        }
    }
    close(code);
}

void websocket::cancel() {
    socket_->cancel();
    wake_writers();
}

// ---------------------------------------------------------------------------
// Keepalive
// ---------------------------------------------------------------------------

void websocket::set_keepalive(std::chrono::milliseconds ping_interval, std::chrono::milliseconds idle_timeout) {
    ping_interval_ = ping_interval;
    idle_timeout_ = idle_timeout;
    auto generation = ++keepalive_generation_;
    keepalive_timer_->cancel();
    if (closed_ || (ping_interval.count() <= 0 && idle_timeout.count() <= 0)) return;

    std::weak_ptr<websocket> weak = weak_from_this();
    if (weak.expired()) {
        LOG_ERROR("websocket keepalive requires the websocket to be owned by a std::shared_ptr");
        return;
    }
    keepalive_timer_ = std::make_shared<boost::asio::steady_timer>(executor_);
    last_ping_ = {};
    last_received_ = std::chrono::steady_clock::now();

    co_spawn(executor_, [weak, timer = keepalive_timer_, generation]() -> awaitable<void> {
        using clock = std::chrono::steady_clock;
        for (;;) {
            clock::time_point next = clock::time_point::max();
            {
                // Not kept alive while waiting
                auto self = weak.lock();
                if (!self || self->closed_ || self->keepalive_generation_ != generation) co_return;
                auto now = clock::now();
                auto received = self->last_received_;
                if (self->idle_timeout_.count() > 0) {
                    if (now - received >= self->idle_timeout_) {
                        LOG_DEBUG("websocket idle for too long, closing");
                        self->close(CLOSE_GOING_AWAY);
                        co_return;
                    }
                    next = received + self->idle_timeout_;
                }
                if (self->ping_interval_.count() > 0) {
                    auto since = std::max(received, self->last_ping_);
                    if (now - since >= self->ping_interval_) {
                        // Queued, not waited for: a ping stuck behind a write that does not
                        // end does not hold the idle timeout. One ping at a time
                        self->last_ping_ = now;
                        if (!self->pending_ping_ && !self->ping_in_flight_) {
                            self->pending_ping_ = true;
                            self->queue_control();
                        }
                        since = now;
                    }
                    next = std::min(next, since + self->ping_interval_);
                }
            }
            timer->expires_at(next);
            co_await timer->async_wait(use_nothrow_awaitable);
        }
    }, detached);
}

void websocket::start_timeout() {
    set_keepalive(CONNECTION_TIMEOUT_SECONDS, 2 * CONNECTION_TIMEOUT_SECONDS);
}

// ---------------------------------------------------------------------------
// Delegated to the socket
// ---------------------------------------------------------------------------

awaitable<boost::system::error_code> websocket::wait(boost::asio::socket_base::wait_type type) {
    if (type == boost::asio::socket_base::wait_read && buffered() > 0) {
        co_return boost::system::error_code{};
    }
    co_return co_await socket_->wait(type);
}

awaitable<boost::system::error_code> websocket::connect(
    const std::string& host,
    const std::string& port,
    std::chrono::seconds timeout) {
    co_return co_await socket_->connect(host, port, timeout);
}

bool websocket::requires_handshake() const {
    return socket_->requires_handshake();
}

awaitable<boost::system::error_code> websocket::handshake(const std::string& host) {
    co_return co_await socket_->handshake(host);
}

bool websocket::is_open() const {
    return !closed_ && socket_->is_open();
}

bool websocket::is_secure() const {
    return socket_->is_secure();
}

size_t websocket::available() const {
    // received, framing included
    if (closed_) return 0;
    return buffered() + socket_->available();
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

uint64_t websocket::remaining_in_frame() const {
    return frame_remaining_;
}

bool websocket::is_message_complete() const {
    return new_message_;
}

}

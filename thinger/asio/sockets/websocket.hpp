#ifndef THINGER_ASIO_WEBSOCKET_HPP
#define THINGER_ASIO_WEBSOCKET_HPP
#pragma once

#include "socket.hpp"
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace thinger::asio {

// WebSocket (RFC 6455) over another socket, once the HTTP handshake is done (see
// http::response::upgrade_websocket_stream() and the client websocket()).
//
// As a socket it is a byte stream, like TCP, TLS or Unix sockets, so a protocol running on it
// (or a socket_pipe) cannot tell it runs over WebSocket:
// - Reads return the payload of the data messages received, text or binary, as one continuous
//   stream: messages, and the frames of a fragmented message, are not delimited. read_some()
//   returns data from one frame at most, and never 0 bytes (empty frames are skipped).
// - Each write sends one message (unfragmented), binary by default (see set_binary()). Writes
//   may be called while another one is pending, from several coroutines: they are sent one
//   after another, in call order. Writing 0 bytes sends nothing. A write interrupted in the
//   middle of its frame (cancel(), or the cancellation of the operation) leaves the rest of
//   the frame to be sent before anything else, so the framing is never broken.
// - Pings are answered, pongs and unexpected control frames handled, while reading: some
//   read must be pending for them to be answered (as with a protocol reading its socket). The
//   pong is sent as soon as no frame is being written, without holding the reads.
// - A close frame received is answered, the connection closed, and reads end with
//   boost::asio::error::eof (as a TCP socket closed by its peer). Writes then fail.
// - close(), on the io_context of the socket, ends the pending operations and closes the
//   socket for its user right away; the closing handshake goes on by itself: the rest of a
//   frame being written, a close frame (code 1000), and the connection closed once the peer
//   answers or after CLOSE_TIMEOUT. Called while nothing runs the io_context (e.g. by a
//   synchronous client), it closes the connection at once instead. close(code) does the same
//   with another status code. close_graceful() does the handshake, waiting. A websocket
//   destroyed open on its io_context closes with 1001.
// - An optional keepalive pings the peer (one ping at a time) and closes the connection
//   when it is idle (see set_keepalive()).
// - Text is not validated as UTF-8 here, neither in text messages nor in close reasons:
//   the stream may cut a character anywhere (http::websocket_connection validates whole
//   messages).
//
// The message interface (read_message_some(), is_message_complete(), is_binary()) is for
// message-oriented users, such as http::websocket_connection and http::websocket_client.
//
// It must be owned by a std::shared_ptr for the keepalive and to answer pings without
// holding the reads. Not thread-safe, as any socket (see socket.hpp).
class websocket : public socket, public std::enable_shared_from_this<websocket> {

public:

    static constexpr auto CONNECTION_TIMEOUT_SECONDS = std::chrono::seconds{60};
    static constexpr auto CLOSE_TIMEOUT = std::chrono::seconds{5};
    static constexpr int MASK_SIZE_BYTES = 4;
    static std::atomic<unsigned long> connections;

    // Close status codes (RFC 6455, 7.4.1)
    static constexpr uint16_t CLOSE_NORMAL = 1000;
    static constexpr uint16_t CLOSE_GOING_AWAY = 1001;
    static constexpr uint16_t CLOSE_PROTOCOL_ERROR = 1002;
    static constexpr uint16_t CLOSE_INVALID_PAYLOAD = 1007;
    static constexpr uint16_t CLOSE_MESSAGE_TOO_BIG = 1009;
    // IANA registry: the server sheds a client (e.g. one that does not keep up); it may retry
    static constexpr uint16_t CLOSE_TRY_AGAIN_LATER = 1013;

    // `binary`: frames written are binary (otherwise text); `server`: the server role (frames
    // received must be masked, frames sent are not)
    websocket(std::shared_ptr<socket> socket, bool binary = true, bool server = true);
    ~websocket() override;

    // Socket control
    awaitable<boost::system::error_code> connect(
        const std::string &host,
        const std::string &port,
        std::chrono::seconds timeout) override;
    void close() override;
    // Close the connection with `code`: the closing handshake on its own when called on the
    // io_context, waiting for the close frame of the peer if `expect_reply`. On the thread of
    // the io_context, as any operation. A code that may not be sent (RFC 6455, 7.4: e.g. 1005,
    // 1006 or 1015) is sent as 1000
    void close(uint16_t code, bool expect_reply = true);
    // Send a close frame (waiting for a pending write), wait for the close frame of the peer
    // (up to CLOSE_TIMEOUT), and close. A code that may not be sent is sent as 1000
    awaitable<void> close_graceful(uint16_t code = CLOSE_NORMAL);
    void cancel() override;
    bool requires_handshake() const override;
    awaitable<boost::system::error_code> handshake(const std::string& host = "") override;

    // Read operations: read(), read(streambuf) and read_until() are built on read_some()
    awaitable<io_result> read_some(uint8_t buffer[], size_t max_size) override;

    // Write operations: one message each
    awaitable<io_result> write(const uint8_t buffer[], size_t size) override;
    awaitable<io_result> write(std::string_view str) override;
    awaitable<io_result> write(const std::vector<boost::asio::const_buffer> &buffers) override;

    // Wait
    awaitable<boost::system::error_code> wait(boost::asio::socket_base::wait_type type) override;

    // Message interface: up to max_size bytes of the frame being received, which may be
    // empty (0 bytes for an empty frame, without error). The message is complete when
    // is_message_complete() and remaining_in_frame() is 0; is_binary() tells its type.
    awaitable<io_result> read_message_some(uint8_t buffer[], size_t max_size);
    uint64_t remaining_in_frame() const;
    bool is_message_complete() const;
    bool is_binary() const { return message_opcode_ == 0x2; }

    // Control frames, sent before the writes waiting (after the one in progress), waiting
    // until sent
    awaitable<void> send_ping(uint8_t buffer[] = nullptr, size_t size = 0);
    awaitable<void> send_pong(uint8_t buffer[] = nullptr, size_t size = 0);

    // State getters
    bool is_open() const override;
    bool is_secure() const override;
    size_t available() const override;
    std::string get_remote_ip() const override;
    std::string get_local_port() const override;
    std::string get_remote_port() const override;

    // Type of the messages written: binary (true) or text
    void set_binary(bool binary) { binary_ = binary; }
    bool get_binary() const { return binary_; }

    // Subprotocol negotiated in the handshake (Sec-WebSocket-Protocol), if any
    const std::string& get_protocol() const { return protocol_; }
    void set_protocol(std::string protocol) { protocol_ = std::move(protocol); }

    // Socket the WebSocket runs on
    std::shared_ptr<socket> get_socket() const { return socket_; }

    // Keepalive: ping the peer once nothing was received from it for `ping_interval`, and
    // close the connection once nothing was received for `idle_timeout` (pongs included).
    // 0 disables each one; calling it again replaces the previous settings, and counts the
    // idle time from then. Requires the websocket to be owned by a std::shared_ptr.
    void set_keepalive(std::chrono::milliseconds ping_interval, std::chrono::milliseconds idle_timeout);
    std::chrono::milliseconds get_ping_interval() const { return ping_interval_; }
    std::chrono::milliseconds get_idle_timeout() const { return idle_timeout_; }

    // Keepalive of the message interface: ping after CONNECTION_TIMEOUT_SECONDS without
    // receiving anything, close after twice that
    void start_timeout();

    // Bytes already read from the socket, read before anything else from it (e.g. frames
    // received along with the handshake)
    void set_read_ahead(std::string data);

private:
    struct frame_header {
        bool fin = false;
        uint8_t rsv = 0;
        uint8_t opcode = 0;
        bool masked = false;
        uint64_t payload_size = 0;
        uint8_t mask[MASK_SIZE_BYTES]{};
        size_t size = 0;   // bytes of the header
    };

    // Parse a frame header from `data`: false if more bytes are needed
    static bool parse_header(const uint8_t* data, size_t size, frame_header& header);
    // Header of a frame sent (with its mask key in the client role); its size
    size_t build_header(uint8_t opcode, uint64_t payload_size, uint8_t header[14], uint8_t mask[MASK_SIZE_BYTES]) const;

    // Read side
    size_t buffered() const { return in_end_ - in_begin_; }
    // Wait until the input buffer holds `size` bytes
    awaitable<boost::system::error_code> fill(size_t size);
    // Read up to max_size bytes of the current data frame payload (at least one)
    awaitable<io_result> read_payload(uint8_t buffer[], size_t max_size);
    // Handle the control frame whose header and payload are buffered
    awaitable<boost::system::error_code> handle_control(const frame_header& header);
    awaitable<io_result> read_frame_data(uint8_t buffer[], size_t max_size, bool skip_empty);
    void unmask(uint8_t buffer[], size_t size);
    // XOR `data` with the mask key, starting at byte `offset` of the key
    static void apply_mask(uint8_t* data, size_t size, const uint8_t mask[MASK_SIZE_BYTES], size_t offset);
    // Fail the connection on a protocol error: close frame (1002, best effort) and close
    boost::system::error_code fail(const char* reason);

    // Write side: one frame at a time (the writer holding the turn), in order: the rest of an
    // interrupted frame first, then the pong and ping queued, then the writes waiting
    awaitable<bool> acquire_write(bool control);
    // Pass the turn on: to the closing handshake once closed, to the control frames queued,
    // or to the next writer waiting
    void release_write();
    void wake_writers();
    awaitable<io_result> send_frame(uint8_t opcode, const std::vector<boost::asio::const_buffer>& payload,
                                    bool control);
    // Write a frame, holding the turn: the rest of an interrupted frame first, and what this
    // one leaves if interrupted
    awaitable<boost::system::error_code> write_frame(uint8_t opcode, const std::vector<boost::asio::const_buffer>& payload);
    awaitable<boost::system::error_code> send_close(uint16_t code);
    // Queue a pong (answering the last ping) or a ping, sent without waiting for it
    void queue_control();
    // Send the control frames queued, holding the turn
    awaitable<void> flush_control();

    void start_linger();
    // The rest of a frame and a close frame, the bytes still buffered and the payload
    // remaining of the frame being received: what the closing handshake needs once this
    // websocket is closed
    static awaitable<void> linger(std::shared_ptr<socket> socket, std::vector<uint8_t> output,
                                  std::string input, uint64_t skip, bool expect_reply);

    std::shared_ptr<socket> socket_;
    bool binary_;
    bool server_role_;
    std::string protocol_;

    // Input buffer: bytes read from the socket not processed yet
    std::vector<uint8_t> in_;
    size_t in_begin_ = 0;
    size_t in_end_ = 0;

    // Frame being received
    bool new_message_ = true;
    uint8_t message_opcode_ = 0;
    uint64_t frame_remaining_ = 0;
    bool frame_fin_ = false;
    bool frame_masked_ = false;
    uint8_t mask_[MASK_SIZE_BYTES]{};
    size_t mask_offset_ = 0;
    bool reading_ = false;

    // Writes waiting for their turn
    struct write_waiter {
        explicit write_waiter(boost::asio::io_context& io_context) : timer(io_context) {}
        boost::asio::steady_timer timer;
        bool control = false;
        bool granted = false;
    };
    bool writing_ = false;
    std::deque<std::shared_ptr<write_waiter>> write_waiters_;
    // Rest of a frame whose write was interrupted, sent before anything else
    std::vector<uint8_t> tail_;
    // Error that broke the connection while writing
    boost::system::error_code write_error_;
    // Control frames queued: the payload of the last ping received, and a keepalive ping
    std::optional<std::string> pending_pong_;
    bool pending_ping_ = false;
    bool ping_in_flight_ = false;

    // Connection state
    bool closed_ = false;
    bool close_received_ = false;
    bool close_sent_ = false;
    // Closing handshake to start once the write in progress ends
    bool linger_pending_ = false;
    bool linger_expect_reply_ = true;
    uint16_t close_code_ = CLOSE_NORMAL;
    std::shared_ptr<boost::asio::steady_timer> close_timer_;

    // Keepalive
    std::shared_ptr<boost::asio::steady_timer> keepalive_timer_;
    uint64_t keepalive_generation_ = 0;
    std::chrono::milliseconds ping_interval_{0};
    std::chrono::milliseconds idle_timeout_{0};
    std::chrono::steady_clock::time_point last_received_;
    std::chrono::steady_clock::time_point last_ping_;
};

}

#endif

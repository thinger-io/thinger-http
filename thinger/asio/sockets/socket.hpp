
#ifndef THINGER_ASIO_SOCKET_HPP
#define THINGER_ASIO_SOCKET_HPP

#include <mutex>
#include <map>
#include <memory>
#include <optional>
#include <string_view>

#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>

#include "../../util/types.hpp"
#include "../ssl/tls_session.hpp"

namespace thinger::asio {

// Byte stream socket: TCP, TLS, Unix, a WebSocket (see websocket.hpp), or a socket defined by
// the application (e.g. a virtual stream multiplexed inside another connection), all used the
// same way by the protocols and tunnels running on them (see socket_pipe.hpp).
//
// Like the asio objects they wrap, sockets are not thread-safe: use them (close() and cancel()
// included) on a thread running their io_context (see get_io_context()), dispatching there
// from any other thread. At most one read and one write may be pending at the same time
// (a websocket queues concurrent writes).
//
// A socket defined by the application must implement read_some(), write(buffer, size),
// close(), cancel() and is_open(); the other methods have defaults built on them (read(),
// read_until() and the other writes) or answering as a socket that does not support them
// (connect(), wait(), and the getters); `using socket::read; using socket::write;` keeps the
// default overloads visible on the derived type. Its operations must keep these guarantees:
// - read_some() waits until at least one byte is available, and returns it (never 0 bytes
//   without an error, unless max_size is 0); boost::asio::error::eof once the peer closed
//   the stream and everything it sent was read.
// - write() returns once all the bytes are written (accepted by the socket, as a TCP socket
//   copies them to the kernel), or with an error.
// - close() closes the socket right away, without waiting: is_open() is false afterwards, the
//   pending operations complete with an error (operation_aborted) and new ones fail. It may be
//   called several times, and from the destructor.
// - cancel() makes the pending operations complete with operation_aborted; the socket stays
//   open.
// - Operations complete on the io_context of the socket. Data arriving from another thread
//   (e.g. from the connection a virtual stream runs inside) is handed over by posting it to
//   that io_context.
// - Pending operations should support per-operation cancellation (as asio operations and
//   timers do), used by the awaitable operators (||). If they do not, close() must still
//   complete them.
class socket : private boost::asio::noncopyable {

public:
    // constructors and destructors
    socket(const std::string &context, boost::asio::io_context &io_context);
    virtual ~socket();

    // socket control
    // Connect to host:port (with the TLS handshake if required); operation_not_supported
    // by default. `timeout` bounds the whole call, taken as a deadline when it starts: the
    // name resolution, the connect and, over TLS, the client handshake share it, each phase
    // getting the time left. Once it passes, the socket is closed and the call returns
    // boost::asio::error::timed_out (operation_aborted if the caller cancelled it). Other
    // errors are returned as they are (a handshake error with the socket closed). The bound of
    // the resolution is best-effort: asio cannot interrupt getaddrinfo, so a resolver that
    // hangs ends the call only once it returns (with timed_out if the deadline passed); a host
    // given as an address is not resolved
    virtual awaitable<boost::system::error_code> connect(
        const std::string &host,
        const std::string &port,
        std::chrono::seconds timeout);
    virtual void close() = 0;
    virtual void cancel() = 0;
    virtual bool requires_handshake() const;
    virtual awaitable<boost::system::error_code> handshake(const std::string &host = "");

    // read operations
    // Up to max_size bytes, at least one (see above)
    virtual awaitable<io_result> read_some(uint8_t buffer[], size_t max_size) = 0;
    // As read_some(), for a caller that ends this read alone: a cancellation through `slot` ends
    // it (operation_aborted), not whatever else waits on the socket, as cancel() would, and that
    // of the caller does not reach it, which is for the caller to forward. The default runs
    // read_some() on a coroutine of its own (which costs one per call); TCP, TLS and Unix bind
    // the slot to the read itself
    virtual awaitable<io_result> read_some_cancellable(uint8_t buffer[], size_t max_size,
                                                       boost::asio::cancellation_slot slot);
    // Exactly `size` bytes (fewer only with an error)
    virtual awaitable<io_result> read(uint8_t buffer[], size_t size);
    // Exactly `size` bytes appended to `buffer` (fewer only with an error)
    virtual awaitable<io_result> read(boost::asio::streambuf &buffer, size_t size);
    // Read into `buffer` until it contains `delim`, as boost::asio::async_read_until: the
    // bytes up to the end of the delimiter (the buffer may hold more); not_found if the
    // buffer gets full before
    virtual awaitable<io_result> read_until(boost::asio::streambuf &buffer, std::string_view delim);

    // write operations: all the bytes, or an error
    virtual awaitable<io_result> write(const uint8_t buffer[], size_t size) = 0;
    virtual awaitable<io_result> write(std::string_view str);
    virtual awaitable<io_result> write(const std::vector<boost::asio::const_buffer> &buffers);

    // Write what the socket accepts right now, without waiting: the bytes written, with
    // would_block if none, or operation_not_supported if the socket cannot write without
    // waiting (then nothing is written). Not to be mixed with a pending write.
    virtual size_t write_now(const std::vector<boost::asio::const_buffer> &buffers, boost::system::error_code &ec);

    // As write(buffers), in slices of WRITE_SLICE_SIZE (a socket buffer), `progress` called after
    // each one: a write deadline it moves tells a slow peer that keeps reading from one that
    // stopped, as nginx's send_timeout does. `buffers` is left with what was not written
    static constexpr size_t WRITE_SLICE_SIZE = 64 * 1024;
    awaitable<io_result> write_slices(std::vector<boost::asio::const_buffer> &buffers,
                                      const sync_callback<void()> &progress);
    // Drop the first `size` bytes of `buffers` (and the empty buffers they leave first)
    static void consume(std::vector<boost::asio::const_buffer> &buffers, size_t size);

    // wait (operation_not_supported by default: then an HTTP server on the socket does not
    // watch for a client leaving while a response is pending)
    virtual awaitable<boost::system::error_code> wait(boost::asio::socket_base::wait_type type);

    // Once the socket is readable (see wait()): why, without consuming anything. No error if
    // the peer sent data (it stays available to the next read), eof if it closed its sending
    // side (a FIN; over TLS a close_notify, or a FIN without one), the error of the connection
    // otherwise (a reset). operation_not_supported by default, as wait(). It may wait for the
    // rest of a TLS record
    virtual awaitable<boost::system::error_code> peer_closed();

    // some getters to check the state
    virtual bool is_open() const = 0;
    // false by default
    virtual bool is_secure() const;
    // Bytes that can be read without waiting (0 by default)
    virtual size_t available() const;
    // Bytes written that the peer has not taken yet, as the system counts them (over TCP not
    // acknowledged, over a Unix socket not read); 0 by default (unknown). It only falls as the
    // peer takes them, so a stalled write tells a slow reader from one that stopped reading
    virtual size_t send_backlog() const;
    // Without waiting: whether a read would complete now, with data, the end of the stream or
    // an error (e.g. on an idle connection, which should have nothing to read, the peer
    // closing it). False by default (unknown)
    virtual bool readable_now() const;
    // Addresses of the connection (empty by default)
    virtual std::string get_remote_ip() const;
    virtual std::string get_local_port() const;
    virtual std::string get_remote_port() const;
    // TLS details of the connection once its handshake is done: none for a plain socket (the
    // default), or before the handshake or once closed. A socket running on another one (a
    // websocket) answers for it, so the caller never casts
    virtual std::optional<tls_session> tls() const;
    // Whether a request for the host name `host` (its Host, port removed) is misdirected on
    // this connection: its TLS handshake chose another client certificate mode or certificate
    // than `host` gets (see tls_handshake_record::misdirected()). False without TLS (the default)
    virtual bool misdirected(std::string_view /*host*/) const { return false; }

    // other methods
    boost::asio::io_context &get_io_context() const;

protected:
    using native_handle_type = boost::asio::ip::tcp::socket::native_handle_type;

    // write_now() of a plain stream socket: a single non-blocking gathered send (the
    // buffers past the first 64 are left, as if the socket accepted no more)
    static size_t send_now(native_handle_type descriptor, const std::vector<boost::asio::const_buffer>& buffers,
                           boost::system::error_code& ec);

    // readable_now() of a plain stream socket: polled with no timeout (true if the poll fails)
    static bool poll_readable(native_handle_type descriptor);

    // send_backlog() of a plain stream socket: SIOCOUTQ on Linux, SO_NWRITE on macOS (0 if
    // neither tells, or on an error)
    static size_t output_queue(native_handle_type descriptor);

    // peer_closed() of a plain stream socket: a non-blocking peek of one byte, which leaves it
    // in the socket. No error with data (or nothing to read yet), eof at the end of the stream,
    // else the error of the socket
    static boost::system::error_code peek_closed(native_handle_type descriptor);

    boost::asio::io_context &io_context_;
    static std::atomic<unsigned long> connections;
};

namespace detail {

    // async_read_some() that a cancellation through `slot` ends alone, completing as
    // use_nothrow_awaitable does. A handler of its own, not bind_cancellation_slot(slot,
    // use_nothrow_awaitable): an echo of 128 KiB takes 4% more cycles with the binder (the same
    // instructions), even with a slot not connected; with this one, connected as it is, none.
    // For TCP and Unix: over TLS the binder is not slower (see ssl_socket::read_some_cancellable())
    template <typename Handler>
    struct on_slot_handler {
        Handler handler;
        boost::asio::cancellation_slot slot;

        using cancellation_slot_type = boost::asio::cancellation_slot;
        cancellation_slot_type get_cancellation_slot() const noexcept { return slot; }

        using executor_type = boost::asio::associated_executor_t<Handler>;
        executor_type get_executor() const noexcept { return boost::asio::get_associated_executor(handler); }

        void operator()(boost::system::error_code ec, size_t bytes) { std::move(handler)(io_result{ec, bytes}); }
    };

    template <typename Stream>
    awaitable<io_result> read_some_on_slot(Stream& stream, uint8_t* buffer, size_t max_size,
                                           boost::asio::cancellation_slot slot) {
        return boost::asio::async_initiate<const boost::asio::use_awaitable_t<>, void(io_result)>(
            [&stream, buffer, max_size, slot](auto handler) {
                stream.async_read_some(boost::asio::buffer(buffer, max_size),
                                       on_slot_handler<decltype(handler)>{std::move(handler), slot});
            },
            boost::asio::use_awaitable);
    }

}

}

#endif

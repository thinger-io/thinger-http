
#ifndef THINGER_ASIO_SOCKET_HPP
#define THINGER_ASIO_SOCKET_HPP

#include <mutex>
#include <map>
#include <string_view>

#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>

#include "../../util/types.hpp"

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
    // by default
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

    // wait (operation_not_supported by default: then an HTTP server on the socket does not
    // watch for a client leaving while a response is pending)
    virtual awaitable<boost::system::error_code> wait(boost::asio::socket_base::wait_type type);

    // Once the socket is readable (see wait()): whether the peer closed the connection
    // (closed it, half-closed it or reset it) instead of sending data. Data is not consumed:
    // it stays available to the next read. It may wait for the rest of a TLS record.
    virtual awaitable<bool> peer_closed();

    // some getters to check the state
    virtual bool is_open() const = 0;
    // false by default
    virtual bool is_secure() const;
    // Bytes that can be read without waiting (0 by default)
    virtual size_t available() const;
    // Addresses of the connection (empty by default)
    virtual std::string get_remote_ip() const;
    virtual std::string get_local_port() const;
    virtual std::string get_remote_port() const;

    // other methods
    boost::asio::io_context &get_io_context() const;

protected:
    using native_handle_type = boost::asio::ip::tcp::socket::native_handle_type;

    // write_now() of a plain stream socket: a single non-blocking gathered send (the
    // buffers past the first 64 are left, as if the socket accepted no more)
    static size_t send_now(native_handle_type descriptor, const std::vector<boost::asio::const_buffer>& buffers,
                           boost::system::error_code& ec);

    boost::asio::io_context &io_context_;
    static std::atomic<unsigned long> connections;
};

}

#endif

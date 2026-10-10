
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

class socket : private boost::asio::noncopyable {

public:
    // constructors and destructors
    socket(const std::string &context, boost::asio::io_context &io_context);
    virtual ~socket();

    // socket control
    virtual awaitable<boost::system::error_code> connect(
        const std::string &host,
        const std::string &port,
        std::chrono::seconds timeout) = 0;
    virtual void close() = 0;
    virtual void cancel() = 0;
    virtual bool requires_handshake() const;
    virtual awaitable<boost::system::error_code> handshake(const std::string &host = "");

    // read operations
    virtual awaitable<io_result> read_some(uint8_t buffer[], size_t max_size) = 0;
    virtual awaitable<io_result> read(uint8_t buffer[], size_t size) = 0;
    virtual awaitable<io_result> read(boost::asio::streambuf &buffer, size_t size) = 0;
    virtual awaitable<io_result> read_until(boost::asio::streambuf &buffer, std::string_view delim) = 0;

    // write operations
    virtual awaitable<io_result> write(const uint8_t buffer[], size_t size) = 0;
    virtual awaitable<io_result> write(std::string_view str) = 0;
    virtual awaitable<io_result> write(const std::vector<boost::asio::const_buffer> &buffers) = 0;

    // Write what the socket accepts right now, without waiting: the bytes written, with
    // would_block if none, or operation_not_supported if the socket cannot write without
    // waiting (then nothing is written). Not to be mixed with a pending write.
    virtual size_t write_now(const std::vector<boost::asio::const_buffer> &buffers, boost::system::error_code &ec);

    // wait
    virtual awaitable<boost::system::error_code> wait(boost::asio::socket_base::wait_type type) = 0;

    // Once the socket is readable (see wait()): whether the peer closed the connection
    // (closed it, half-closed it or reset it) instead of sending data. Data is not consumed:
    // it stays available to the next read. It may wait for the rest of a TLS record.
    virtual awaitable<bool> peer_closed();

    // some getters to check the state
    virtual bool is_open() const = 0;
    virtual bool is_secure() const = 0;
    virtual size_t available() const = 0;
    virtual std::string get_remote_ip() const = 0;
    virtual std::string get_local_port() const = 0;
    virtual std::string get_remote_port() const = 0;

    // other methods
    boost::asio::io_context &get_io_context() const;

protected:
    using native_handle_type = boost::asio::ip::tcp::socket::native_handle_type;

    // write_now() of a plain stream socket: a single non-blocking gathered send (the
    // buffers past the first 64 are left, as if the socket accepted no more)
    static size_t send_now(native_handle_type descriptor, const std::vector<boost::asio::const_buffer>& buffers,
                           boost::system::error_code& ec);

    std::string context_;
    boost::asio::io_context &io_context_;
    static std::atomic<unsigned long> connections;
    static std::map<std::string, unsigned long> context_count;
    static std::mutex mutex_;
};

}

#endif

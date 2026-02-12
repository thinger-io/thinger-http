
#ifndef THINGER_ASIO_SOCKET_HPP
#define THINGER_ASIO_SOCKET_HPP

#include <mutex>
#include <map>
#include <string_view>

#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>

#include "../../util/types.hpp"

namespace thinger::asio {

class socket : private boost::asio::noncopyable {

public:
    // constructors and destructors
    socket(const std::string &context, boost::asio::io_context &io_context);
    virtual ~socket();

    // socket control
    virtual awaitable<void> connect(
        const std::string &host,
        const std::string &port,
        std::chrono::seconds timeout) = 0;
    virtual void close() = 0;
    virtual void cancel() = 0;
    virtual bool requires_handshake() const;
    virtual awaitable<void> handshake(const std::string &host = "");

    // read operations
    virtual awaitable<size_t> read_some(uint8_t buffer[], size_t max_size) = 0;
    virtual awaitable<size_t> read(uint8_t buffer[], size_t size) = 0;
    virtual awaitable<size_t> read(boost::asio::streambuf &buffer, size_t size) = 0;
    virtual awaitable<size_t> read_until(boost::asio::streambuf &buffer, std::string_view delim) = 0;

    // write operations
    virtual awaitable<size_t> write(const uint8_t buffer[], size_t size) = 0;
    virtual awaitable<size_t> write(std::string_view str) = 0;
    virtual awaitable<size_t> write(const std::vector<boost::asio::const_buffer> &buffers) = 0;

    // wait
    virtual awaitable<void> wait(boost::asio::socket_base::wait_type type) = 0;

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
    std::string context_;
    boost::asio::io_context &io_context_;
    static std::atomic<unsigned long> connections;
    static std::map<std::string, unsigned long> context_count;
    static std::mutex mutex_;
};

}

#endif

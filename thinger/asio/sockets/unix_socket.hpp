#ifndef THINGER_ASIO_UNIX_SOCKET_HPP
#define THINGER_ASIO_UNIX_SOCKET_HPP

#include "socket.hpp"
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/steady_timer.hpp>
#include <string_view>

namespace thinger::asio {

class unix_socket : public socket {

public:
    // constructors and destructors
    unix_socket(const std::string &context, boost::asio::io_context &io_context);
    unix_socket(const std::string &context, boost::asio::local::stream_protocol::socket&& socket);
    ~unix_socket() override;

    // socket control
    awaitable<void> connect(
        const std::string &host,
        const std::string &port,
        std::chrono::seconds timeout) override;
    awaitable<void> connect(const std::string &path, std::chrono::seconds timeout);
    void close() override;
    void cancel() override;

    // read operations
    awaitable<size_t> read_some(uint8_t buffer[], size_t max_size) override;
    awaitable<size_t> read(uint8_t buffer[], size_t size) override;
    awaitable<size_t> read(boost::asio::streambuf &buffer, size_t size) override;
    awaitable<size_t> read_until(boost::asio::streambuf &buffer, std::string_view delim) override;

    // write operations
    awaitable<size_t> write(const uint8_t buffer[], size_t size) override;
    awaitable<size_t> write(std::string_view str) override;
    awaitable<size_t> write(const std::vector<boost::asio::const_buffer> &buffers) override;

    // wait
    awaitable<void> wait(boost::asio::socket_base::wait_type type) override;

    // some getters to check the state
    bool is_open() const override;
    bool is_secure() const override;
    size_t available() const override;
    std::string get_remote_ip() const override;
    std::string get_local_port() const override;
    std::string get_remote_port() const override;

private:
    boost::asio::local::stream_protocol::socket socket_;
};

}

#endif
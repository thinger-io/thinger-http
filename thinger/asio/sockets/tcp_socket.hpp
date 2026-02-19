#ifndef THINGER_ASIO_TCP_SOCKET_HPP
#define THINGER_ASIO_TCP_SOCKET_HPP

#include <memory>
#include <functional>
#include <thread>
#include <mutex>
#include <iostream>
#include <string_view>

#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/steady_timer.hpp>

#include "../../util/logger.hpp"
#include "socket.hpp"

namespace thinger::asio {

class tcp_socket : public socket {

public:
    // constructors and destructors
    tcp_socket(const std::string &context, boost::asio::io_context &io_context);
    tcp_socket(const std::string &context, const std::shared_ptr<tcp_socket>& tcp_socket);
    tcp_socket(const std::string &context, boost::asio::ip::tcp::socket&& socket);
    ~tcp_socket() override;

    // socket control
    awaitable<boost::system::error_code> connect(
        const std::string &host,
        const std::string &port,
        std::chrono::seconds timeout) override;
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
    awaitable<boost::system::error_code> wait(boost::asio::socket_base::wait_type type) override;

    // some getters to check the state
    bool is_open() const override;
    bool is_secure() const override;
    size_t available() const override;
    std::string get_remote_ip() const override;
    std::string get_local_port() const override;
    std::string get_remote_port() const override;

    // other methods
    void enable_tcp_no_delay();
    void disable_tcp_no_delay();
    virtual boost::asio::ip::tcp::socket &get_socket();

protected:
    boost::asio::ip::tcp::socket socket_;
};

}

#endif

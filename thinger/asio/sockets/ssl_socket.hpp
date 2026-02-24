#ifndef THINGER_ASIO_SSL_SOCKET_HPP
#define THINGER_ASIO_SSL_SOCKET_HPP

#include "tcp_socket.hpp"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

namespace thinger::asio {

class ssl_socket : public tcp_socket {
public:
    // constructors and destructors
    ssl_socket(const std::string& context, boost::asio::io_context& io_context,
               const std::shared_ptr<boost::asio::ssl::context>& ssl_context);
    ssl_socket(const std::string& context, const std::shared_ptr<tcp_socket>& socket,
               const std::shared_ptr<boost::asio::ssl::context>& ssl_context);
    ~ssl_socket() override;

    // socket control
    void close() override;
    bool requires_handshake() const override;
    awaitable<boost::system::error_code> handshake(const std::string& host = "") override;

    // read operations
    awaitable<io_result> read_some(uint8_t buffer[], size_t max_size) override;
    awaitable<io_result> read(uint8_t buffer[], size_t size) override;
    awaitable<io_result> read(boost::asio::streambuf& buffer, size_t size) override;
    awaitable<io_result> read_until(boost::asio::streambuf& buffer, std::string_view delim) override;

    // write operations
    awaitable<io_result> write(const uint8_t buffer[], size_t size) override;
    awaitable<io_result> write(std::string_view str) override;
    awaitable<io_result> write(const std::vector<boost::asio::const_buffer>& buffers) override;

    // some getters to check the state
    bool is_secure() const override;

private:
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket&> ssl_stream_;
    std::shared_ptr<boost::asio::ssl::context> ssl_context_;
};

}

#endif

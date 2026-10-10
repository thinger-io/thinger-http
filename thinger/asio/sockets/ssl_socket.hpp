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

    // wait
    awaitable<boost::system::error_code> wait(boost::asio::socket_base::wait_type type) override;
    awaitable<bool> peer_closed() override;

    // write operations
    awaitable<io_result> write(const uint8_t buffer[], size_t size) override;
    awaitable<io_result> write(std::string_view str) override;
    awaitable<io_result> write(const std::vector<boost::asio::const_buffer>& buffers) override;
    // Writes go through the TLS stream: never written without waiting
    size_t write_now(const std::vector<boost::asio::const_buffer>& buffers, boost::system::error_code& ec) override {
        return socket::write_now(buffers, ec);
    }

    // some getters to check the state
    bool is_secure() const override;

private:
    // Move up to `size` bytes of read_ahead_ to `buffer`; how many were moved
    size_t take_read_ahead(uint8_t buffer[], size_t size);
    // Move read_ahead_ to `buffer`
    void take_read_ahead(boost::asio::streambuf& buffer, size_t max_size);

    boost::asio::ssl::stream<boost::asio::ip::tcp::socket&> ssl_stream_;
    // Data already decrypted by peer_closed(), served before reading from the stream
    std::string read_ahead_;
    std::shared_ptr<boost::asio::ssl::context> ssl_context_;
};

}

#endif

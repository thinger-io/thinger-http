#ifndef THINGER_HTTP_CLIENT_CONNECTION_HPP
#define THINGER_HTTP_CLIENT_CONNECTION_HPP

#include <memory>
#include <mutex>
#include <boost/noncopyable.hpp>

#include "../common/http_request.hpp"
#include "../common/http_response.hpp"
#include "response_factory.hpp"
#include "stream_types.hpp"
#include "../../asio/sockets/tcp_socket.hpp"
#include "../../asio/sockets/unix_socket.hpp"
#include "../../util/types.hpp"

namespace thinger::http {

class client_connection : public std::enable_shared_from_this<client_connection>, public boost::noncopyable {

    static constexpr unsigned MAX_BUFFER_SIZE = 4096;
    static constexpr unsigned MAX_RETRIES = 3;
    static constexpr auto DEFAULT_TIMEOUT = std::chrono::seconds{60};
    static constexpr auto CONNECT_TIMEOUT = std::chrono::seconds{10};

public:
    static std::atomic<unsigned long> connections;

    // Constructors
    explicit client_connection(std::shared_ptr<thinger::asio::socket> socket,
                               std::chrono::seconds timeout = DEFAULT_TIMEOUT);

    client_connection(std::shared_ptr<thinger::asio::unix_socket> socket,
                      const std::string& path,
                      std::chrono::seconds timeout = DEFAULT_TIMEOUT);

    virtual ~client_connection();

    // Main operation - send request and get response
    awaitable<std::shared_ptr<http_response>> send_request(std::shared_ptr<http_request> request);

    // Streaming operation - send request and stream response through callback
    awaitable<stream_result> send_request_streaming(
        std::shared_ptr<http_request> request,
        stream_callback callback);

    // Connection management
    void close();
    std::shared_ptr<thinger::asio::socket> release_socket();
    std::shared_ptr<thinger::asio::socket> get_socket() const { return socket_; }
    bool is_open() const { return socket_ && socket_->is_open(); }

private:
    // Internal helpers
    awaitable<void> ensure_connected(const http_request& request);
    awaitable<std::shared_ptr<http_response>> read_response(bool head_request);

    std::shared_ptr<thinger::asio::socket> socket_;
    std::string socket_path_;
    std::chrono::seconds timeout_;
    uint8_t buffer_[MAX_BUFFER_SIZE];
    response_factory response_parser_;
    std::mutex connection_mutex_;
};

}

#endif

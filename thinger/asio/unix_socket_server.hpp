#ifndef THINGER_ASIO_UNIX_SOCKET_SERVER_HPP
#define THINGER_ASIO_UNIX_SOCKET_SERVER_HPP

#include "basic_socket_server.hpp"
#include "sockets/unix_socket.hpp"
#include <boost/asio/local/stream_protocol.hpp>

namespace thinger::asio {

class unix_socket_server : public basic_socket_server<boost::asio::local::stream_protocol> {
public:
    // Constructor with io_context providers
    unix_socket_server(std::string unix_path,
                      io_context_provider acceptor_context_provider,
                      io_context_provider connection_context_provider,
                      std::set<std::string> allowed_remotes = {},
                      std::set<std::string> forbidden_remotes = {});

    // Legacy constructor for backward compatibility (uses workers): connections are served
    // on the workers, and accepted on a thread of the server
    unix_socket_server(std::string unix_path,
                      std::set<std::string> allowed_remotes = {},
                      std::set<std::string> forbidden_remotes = {});

    ~unix_socket_server() override;

    // Override stop to also remove the socket file
    bool stop() override;

    // Override from base
    std::string get_service_name() const override;
    uint16_t local_port() const override { return 0; }

protected:
    std::optional<endpoint_type> listening_endpoint(boost::asio::io_context& io_context) override;
    void on_listening() override;
    connection_server make_connection_server() const override;

private:
    std::string unix_path_;
};

} // namespace thinger::asio

#endif // THINGER_ASIO_UNIX_SOCKET_SERVER_HPP

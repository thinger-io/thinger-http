#ifndef THINGER_ASIO_UNIX_SOCKET_SERVER_HPP
#define THINGER_ASIO_UNIX_SOCKET_SERVER_HPP

#include "socket_server_base.hpp"
#include "sockets/unix_socket.hpp"
#include <boost/asio/local/stream_protocol.hpp>

namespace thinger::asio {

class unix_socket_server : public socket_server_base {
public:
    // Constructor with io_context providers
    unix_socket_server(std::string unix_path,
                      io_context_provider acceptor_context_provider,
                      io_context_provider connection_context_provider,
                      std::set<std::string> allowed_remotes = {}, 
                      std::set<std::string> forbidden_remotes = {});
    
    // Legacy constructor for backward compatibility (uses workers)
    unix_socket_server(std::string unix_path,
                      std::set<std::string> allowed_remotes = {}, 
                      std::set<std::string> forbidden_remotes = {});

    ~unix_socket_server();

    // Override stop to properly close acceptor
    bool stop() override;

    // Override from base
    std::string get_service_name() const override;
    uint16_t local_port() const override { return 0; }

protected:
    bool create_acceptor() override;
    void accept_connection() override;

private:
    std::unique_ptr<boost::asio::local::stream_protocol::acceptor> acceptor_;
    std::string unix_path_;
};

} // namespace thinger::asio

#endif // THINGER_ASIO_UNIX_SOCKET_SERVER_HPP
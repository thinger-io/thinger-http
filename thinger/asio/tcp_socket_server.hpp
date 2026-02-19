#ifndef THINGER_ASIO_TCP_SOCKET_SERVER_HPP
#define THINGER_ASIO_TCP_SOCKET_SERVER_HPP

#include "socket_server_base.hpp"
#include "sockets/tcp_socket.hpp"
#include "sockets/ssl_socket.hpp"
#include <boost/asio/ssl.hpp>

namespace thinger::asio {

class tcp_socket_server : public socket_server_base {
public:
    // Constructor with io_context providers
    tcp_socket_server(std::string host, 
                     std::string port,
                     io_context_provider acceptor_context_provider,
                     io_context_provider connection_context_provider,
                     std::set<std::string> allowed_remotes = {}, 
                     std::set<std::string> forbidden_remotes = {});
    
    // Legacy constructor for backward compatibility (uses workers)
    tcp_socket_server(std::string host, 
                     std::string port, 
                     std::set<std::string> allowed_remotes = {}, 
                     std::set<std::string> forbidden_remotes = {});

    ~tcp_socket_server();

    // Override stop to properly close acceptor
    bool stop() override;

    // TCP specific configuration
    void set_tcp_no_delay(bool tcp_no_delay);
    
    // SSL configuration
    void enable_ssl(bool ssl = true, bool client_certificate = false);
    void set_ssl_context(std::shared_ptr<boost::asio::ssl::context> context);
    
    // SNI callback type: int (*callback)(SSL *ssl, int *ad, void *arg)
    using sni_callback_type = int (*)(SSL*, int*, void*);
    void set_sni_callback(sni_callback_type callback);

    // Override from base
    std::string get_service_name() const override;
    uint16_t local_port() const override;

protected:
    bool create_acceptor() override;
    void accept_connection() override;

private:
    void close_acceptor();
    
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    std::string host_;
    std::string port_;
    bool tcp_no_delay_ = true;
    
    // SSL configuration
    bool ssl_enabled_ = false;
    bool client_certificate_ = false;
    std::shared_ptr<boost::asio::ssl::context> ssl_context_;
};

} // namespace thinger::asio

#endif // THINGER_ASIO_TCP_SOCKET_SERVER_HPP
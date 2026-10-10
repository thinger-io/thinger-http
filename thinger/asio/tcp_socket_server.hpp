#ifndef THINGER_ASIO_TCP_SOCKET_SERVER_HPP
#define THINGER_ASIO_TCP_SOCKET_SERVER_HPP

#include "socket_server_base.hpp"
#include "sockets/tcp_socket.hpp"
#include "sockets/ssl_socket.hpp"
#include <boost/asio/ssl.hpp>

namespace thinger::asio {

class worker_thread;

class tcp_socket_server : public socket_server_base {
public:
    // Constructor with io_context providers
    tcp_socket_server(std::string host, 
                     std::string port,
                     io_context_provider acceptor_context_provider,
                     io_context_provider connection_context_provider,
                     std::set<std::string> allowed_remotes = {}, 
                     std::set<std::string> forbidden_remotes = {});
    
    // Legacy constructor for backward compatibility (uses workers): connections are served
    // on the workers, and accepted on a thread of the server
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
    bool listen(boost::asio::io_context& io_context);
    void close_acceptor();
    void stop_acceptor_thread();
    void retry_accept();

    // With a thread of its own: wait for connections, accept the pending ones, and hand each
    // one over to the worker serving it
    void wait_connections();
    void accept_pending();
    void hand_over(int descriptor);

    // Thread accepting the connections of a server on the workers: accepting does not wait
    // behind the connections a worker serves, and the workers serve them in turn. Declared
    // before the acceptor, which uses its io_context
    bool own_acceptor_thread_ = false;
    std::unique_ptr<worker_thread> acceptor_thread_;

    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    boost::asio::ip::tcp protocol_ = boost::asio::ip::tcp::v4();
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
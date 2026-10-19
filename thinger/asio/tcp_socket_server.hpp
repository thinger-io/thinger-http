#ifndef THINGER_ASIO_TCP_SOCKET_SERVER_HPP
#define THINGER_ASIO_TCP_SOCKET_SERVER_HPP

#include "basic_socket_server.hpp"
#include "sockets/tcp_socket.hpp"
#include "sockets/ssl_socket.hpp"
#include "ssl/client_verify.hpp"
#include <boost/asio/ssl.hpp>
#include <chrono>

namespace thinger::asio {

namespace detail {
// Connections of a server whose TLS handshake is pending (see tcp_socket_server::stop())
class pending_handshakes;
}

class tcp_socket_server : public basic_socket_server<boost::asio::ip::tcp> {
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

    ~tcp_socket_server() override;

    // Default of set_handshake_timeout(): nginx's ssl_handshake_timeout
    static constexpr std::chrono::milliseconds DEFAULT_HANDSHAKE_TIMEOUT{60000};

    // TCP specific configuration
    void set_tcp_no_delay(bool tcp_no_delay);
    
    // SSL configuration. `verify`: the certificates asked of the clients (none by default),
    // set on each connection, so other listeners sharing the context keep theirs. False
    // (logged) if its trusted CAs cannot be read: then the server does not serve connections
    // (start() listens, and accepts none), rather than serve them without the validation asked
    // for
    bool enable_ssl(bool ssl = true, client_verify_options verify = {});
    // Deprecated: `client_certificate` (unused before 0.8) asks for an optional certificate,
    // validated by the application (client_verify::optional, deferred)
    [[deprecated("use enable_ssl(bool, client_verify_options)")]]
    void enable_ssl(bool ssl, bool client_certificate);
    void set_ssl_context(std::shared_ptr<boost::asio::ssl::context> context);

    // How long a TLS client has to complete its handshake once accepted, from the connection
    // accepted: closed otherwise, without reaching the handler. 60 s by default, as nginx; 0
    // disables it. Applies to the connections accepted afterwards
    void set_handshake_timeout(std::chrono::milliseconds timeout);
    std::chrono::milliseconds get_handshake_timeout() const { return handshake_timeout_; }

    // Also closes the connections whose TLS handshake is not done yet: they never reach the
    // handler
    bool stop() override;
    
    // SNI callback type: int (*callback)(SSL *ssl, int *ad, void *arg)
    using sni_callback_type = int (*)(SSL*, int*, void*);
    void set_sni_callback(sni_callback_type callback);

    // Override from base
    std::string get_service_name() const override;
    uint16_t local_port() const override;

protected:
    std::optional<endpoint_type> listening_endpoint(boost::asio::io_context& io_context) override;
    void on_listening() override;
    connection_server make_connection_server() const override;
    bool is_connection_allowed(native_handle_type descriptor) const override;

private:
    std::string host_;
    std::string port_;
    bool tcp_no_delay_ = true;
    
    // SSL configuration
    bool ssl_enabled_ = false;
    std::shared_ptr<boost::asio::ssl::context> ssl_context_;
    // Client certificates asked for: none if their trusted CAs could not be read
    std::shared_ptr<const client_verify_policy> client_verify_ = client_verify_policy::create({});
    std::chrono::milliseconds handshake_timeout_ = DEFAULT_HANDSHAKE_TIMEOUT;

    // Connections accepted while listening whose handshake is pending, closed on stop()
    mutable std::shared_ptr<detail::pending_handshakes> handshakes_;
};

} // namespace thinger::asio

#endif // THINGER_ASIO_TCP_SOCKET_SERVER_HPP
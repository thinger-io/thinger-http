#include "tcp_socket_server.hpp"
#include "workers.hpp"
#include "../util/logger.hpp"
#include "../util/types.hpp"
#include <boost/asio/ssl.hpp>
#include <sys/socket.h>

namespace thinger::asio {

// Constructor with io_context providers
tcp_socket_server::tcp_socket_server(std::string host, 
                                   std::string port,
                                   io_context_provider acceptor_context_provider,
                                   io_context_provider connection_context_provider,
                                   std::set<std::string> allowed_remotes, 
                                   std::set<std::string> forbidden_remotes)
    : basic_socket_server(std::move(acceptor_context_provider),
                        std::move(connection_context_provider),
                        std::move(allowed_remotes),
                        std::move(forbidden_remotes))
    , host_(std::move(host))
    , port_(std::move(port))
{
}

// Legacy constructor for backward compatibility
tcp_socket_server::tcp_socket_server(std::string host, 
                                   std::string port, 
                                   std::set<std::string> allowed_remotes, 
                                   std::set<std::string> forbidden_remotes)
    : tcp_socket_server(host, port,
                       []() -> boost::asio::io_context& { return get_workers().get_thread_io_context(); },
                       []() -> boost::asio::io_context& { return get_workers().get_next_io_context(); },
                       std::move(allowed_remotes), 
                       std::move(forbidden_remotes))
{
    use_acceptor_thread();
}

tcp_socket_server::~tcp_socket_server() {
    // Before the acceptor thread could use what is destroyed here
    stop();
}

void tcp_socket_server::set_tcp_no_delay(bool tcp_no_delay) {
    tcp_no_delay_ = tcp_no_delay;
}

void tcp_socket_server::enable_ssl(bool ssl, bool client_certificate) {
    ssl_enabled_ = ssl;
    client_certificate_ = client_certificate;
}

void tcp_socket_server::set_ssl_context(std::shared_ptr<boost::asio::ssl::context> context) {
    ssl_context_ = std::move(context);
}

void tcp_socket_server::set_sni_callback(sni_callback_type callback) {
    if (ssl_context_) {
        SSL_CTX_set_tlsext_servername_callback(ssl_context_->native_handle(), callback);
    }
}

std::string tcp_socket_server::get_service_name() const {
    return (ssl_enabled_ ? "ssl_server@" : "tcp_server@") + host_ + ":" + port_;
}

uint16_t tcp_socket_server::local_port() const {
    return acceptor() ? acceptor()->local_endpoint().port() : 0;
}

std::optional<boost::asio::ip::tcp::endpoint> tcp_socket_server::listening_endpoint(boost::asio::io_context& io_context) {
    try {
        boost::asio::ip::tcp::resolver resolver(io_context);
        auto results = resolver.resolve(host_, port_);
        if (results.begin() == results.end()) {
            LOG_ERROR("no endpoints found for {}:{}", host_, port_);
            return std::nullopt;
        }
        return results.begin()->endpoint();
    } catch (const boost::system::system_error& e) {
        LOG_ERROR("failed to resolve {}:{} - {}", host_, port_, e.code().message());
        return std::nullopt;
    }
}

void tcp_socket_server::on_listening() {
    LOG_INFO("TCP server is now listening on {}:{}", host_, port_);
}

namespace {

// What an accepted connection needs to be set up, copied: the server may be stopped and gone
// by then
struct connection_setup {
    std::shared_ptr<boost::asio::ssl::context> ssl_context; // none without TLS
    bool tcp_no_delay;
    std::function<void(std::shared_ptr<socket>)> handler;
};

std::shared_ptr<tcp_socket> make_socket(boost::asio::io_context& io_context, const connection_setup& setup) {
    if (setup.ssl_context) return std::make_shared<ssl_socket>("ssl_socket_server", io_context, setup.ssl_context);
    return std::make_shared<tcp_socket>("tcp_socket_server", io_context);
}

// Set an accepted connection up and hand it to the handler
void serve_connection(std::shared_ptr<tcp_socket> sock, const connection_setup& setup) {
    LOG_INFO("received connection from: ip: {}, port: {}, secure: {}",
            sock->get_remote_ip(), sock->get_local_port(), sock->is_secure());

    if (setup.tcp_no_delay) {
        sock->enable_tcp_no_delay();
    }

    if (sock->requires_handshake()) {
        // Use co_spawn to run the coroutine-based handshake
        co_spawn(sock->get_io_context(),
            [sock, handler = setup.handler]() -> awaitable<void> {
                auto ec = co_await sock->handshake();
                if (ec) {
                    LOG_ERROR("error while handling SSL handshake: {}, remote ip: {}",
                             ec.message(), sock->get_remote_ip());
                    co_return;
                }
                if (handler) handler(sock);
            },
            detached);
    } else {
        if (setup.handler) setup.handler(std::move(sock));
    }
}

} // namespace

tcp_socket_server::connection_server tcp_socket_server::make_connection_server() const {
    if (ssl_enabled_ && !ssl_context_) {
        LOG_ERROR("SSL enabled but no SSL context configured");
        return {};
    }
    return [setup = connection_setup{ssl_enabled_ ? ssl_context_ : nullptr, tcp_no_delay_, handler_}]
        (boost::asio::ip::tcp::socket peer) {
            auto sock = make_socket(static_cast<boost::asio::io_context&>(peer.get_executor().context()), setup);
            sock->get_socket() = std::move(peer);
            serve_connection(std::move(sock), setup);
        };
}

bool tcp_socket_server::is_connection_allowed(native_handle_type descriptor) const {
    // The remote address is only looked up to filter it
    if (!filters_remotes()) return true;

    boost::asio::ip::tcp::endpoint remote;
    auto size = static_cast<socklen_t>(remote.capacity());
    std::string remote_ip = "0.0.0.0";
    if (::getpeername(descriptor, remote.data(), &size) == 0) {
        remote.resize(size);
        remote_ip = remote.address().to_string();
    }
    if (is_remote_allowed(remote_ip)) return true;

    LOG_WARNING("rejecting connection from: ip: {}, port: {}, secure: {}",
               remote_ip, local_port(), ssl_enabled_);
    return false;
}

} // namespace thinger::asio

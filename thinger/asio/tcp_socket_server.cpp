#include "tcp_socket_server.hpp"
#include "workers.hpp"
#include "../util/logger.hpp"
#include "../util/types.hpp"
#include <boost/asio/ssl.hpp>
#include <sys/socket.h>
#include <mutex>
#include <unordered_map>

namespace thinger::asio {

namespace detail {

// What the connections whose handshake is pending share with the server: stop() closes them,
// on their io_contexts, and those done afterwards are not served. Each one is removed once its
// handshake ends, so it holds no more than the connections open
class pending_handshakes {
public:
    // False if the server stopped
    bool add(const std::shared_ptr<tcp_socket>& sock) {
        std::scoped_lock lock(mutex_);
        if (stopped_) return false;
        sockets_.emplace(sock.get(), sock);
        return true;
    }

    // Its handshake ended: whether to serve it (false if the server stopped)
    bool remove(const tcp_socket* sock) {
        std::scoped_lock lock(mutex_);
        sockets_.erase(sock);
        return !stopped_;
    }

    void stop() {
        std::unordered_map<const tcp_socket*, std::weak_ptr<tcp_socket>> sockets;
        {
            std::scoped_lock lock(mutex_);
            stopped_ = true;
            sockets.swap(sockets_);
        }
        for (auto& [key, weak] : sockets) {
            auto sock = weak.lock();
            if (!sock) continue;
            boost::asio::post(sock->get_io_context(), [weak] {
                if (auto pending = weak.lock()) pending->close();
            });
        }
    }

private:
    std::mutex mutex_;
    bool stopped_ = false;
    std::unordered_map<const tcp_socket*, std::weak_ptr<tcp_socket>> sockets_;
};

} // namespace detail

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

bool tcp_socket_server::enable_ssl(bool ssl, client_verify_options verify) {
    ssl_enabled_ = ssl;
    client_verify_ = client_verify_policy::create(verify);
    return client_verify_ != nullptr;
}

void tcp_socket_server::enable_ssl(bool ssl, bool client_certificate) {
    enable_ssl(ssl, client_verify_options{client_certificate ? client_verify::optional : client_verify::none});
}

void tcp_socket_server::set_handshake_timeout(std::chrono::milliseconds timeout) {
    handshake_timeout_ = timeout;
}

bool tcp_socket_server::stop() {
    auto result = basic_socket_server::stop();
    if (handshakes_) handshakes_->stop();
    return result;
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
    if (!acceptor()) return 0;
    boost::system::error_code ec;
    auto endpoint = acceptor()->local_endpoint(ec);
    return ec ? 0 : endpoint.port();
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
    LOG_DEBUG("TCP server is now listening on {}:{}", host_, local_port());
}

namespace {

// What an accepted connection needs to be set up, copied: the server may be stopped and gone
// by then
struct connection_setup {
    std::shared_ptr<boost::asio::ssl::context> ssl_context; // none without TLS
    std::shared_ptr<const client_verify_policy> client_verify;
    std::chrono::milliseconds handshake_timeout;
    std::shared_ptr<detail::pending_handshakes> handshakes;
    bool tcp_no_delay;
    sync_callback<void(std::shared_ptr<socket>)> handler;
};

std::shared_ptr<tcp_socket> make_socket(boost::asio::io_context& io_context, const connection_setup& setup) {
    if (!setup.ssl_context) return std::make_shared<tcp_socket>("tcp_socket_server", io_context);
    auto sock = std::make_shared<ssl_socket>("ssl_socket_server", io_context, setup.ssl_context);
    sock->set_client_verify(setup.client_verify);
    return sock;
}

// The handshake of a TLS connection, closed if it does not end within `timeout` (0: no limit)
awaitable<boost::system::error_code> handshake_within(std::shared_ptr<tcp_socket> sock,
                                                      std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0) co_return co_await sock->handshake();
    // On the io_context of the socket, as the handshake. The handler does not own the socket
    auto timer = std::make_shared<boost::asio::steady_timer>(sock->get_io_context(), timeout);
    auto expired = std::make_shared<bool>(false);
    timer->async_wait([weak = std::weak_ptr<tcp_socket>(sock), expired](const boost::system::error_code& ec) {
        if (ec) return;
        *expired = true;
        if (auto pending = weak.lock()) pending->close();
    });
    auto ec = co_await sock->handshake();
    timer->cancel();
    co_return *expired ? boost::system::error_code(boost::asio::error::timed_out) : ec;
}

// The handshake of a TLS connection, then the handler, unless the server stopped meanwhile
awaitable<void> serve_tls(std::shared_ptr<tcp_socket> sock, connection_setup setup) {
    if (!setup.handshakes->add(sock)) co_return;
    auto ec = co_await handshake_within(sock, setup.handshake_timeout);
    bool serve = setup.handshakes->remove(sock.get());
    // Not errors of the server: any client (or scanner) may cause them
    if (ec == boost::asio::error::timed_out) {
        LOG_DEBUG("TLS handshake not done in time, remote ip: {}", sock->get_remote_ip());
        co_return;
    }
    if (ec) {
        LOG_WARNING("error while handling SSL handshake: {}, remote ip: {}", ec.message(), sock->get_remote_ip());
        co_return;
    }
    if (!serve) {
        sock->close();
        co_return;
    }
    if (setup.handler) setup.handler(sock);
}

// Set an accepted connection up and hand it to the handler
void serve_connection(std::shared_ptr<tcp_socket> sock, const connection_setup& setup) {
    LOG_DEBUG("received connection from: ip: {}, port: {}, secure: {}",
            sock->get_remote_ip(), sock->get_local_port(), sock->is_secure());

    // Fails if the connection is already gone: drop it
    if (setup.tcp_no_delay && !sock->enable_tcp_no_delay()) {
        return;
    }

    if (sock->requires_handshake()) {
        // Use co_spawn to run the coroutine-based handshake. What the handler throws is
        // logged, as when it is called right away (see basic_socket_server)
        auto& io_context = sock->get_io_context();
        co_spawn(io_context, serve_tls(std::move(sock), setup),
            [](std::exception_ptr error) {
                if (!error) return;
                try {
                    std::rethrow_exception(error);
                } catch (const std::exception& e) {
                    LOG_ERROR("cannot serve a TLS connection: {}", e.what());
                } catch (...) {
                    LOG_ERROR("cannot serve a TLS connection");
                }
            });
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
    if (ssl_enabled_ && !client_verify_) {
        LOG_ERROR("SSL enabled but the client certificates cannot be validated: the trusted CAs are not valid");
        return {};
    }
    handshakes_ = std::make_shared<detail::pending_handshakes>();
    connection_setup setup{ssl_enabled_ ? ssl_context_ : nullptr, client_verify_,
                                               handshake_timeout_, handshakes_, tcp_no_delay_, handler_};
    return [setup = std::move(setup)]
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

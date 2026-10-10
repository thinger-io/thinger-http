#include "tcp_socket_server.hpp"
#include "workers.hpp"
#include "worker_thread.hpp"
#include "../util/logger.hpp"
#include "../util/types.hpp"
#include <boost/asio/ssl.hpp>
#include <cerrno>
#include <system_error>
#include <utility>
#include <sys/socket.h>
#include <unistd.h>

namespace thinger::asio {

// Constructor with io_context providers
tcp_socket_server::tcp_socket_server(std::string host, 
                                   std::string port,
                                   io_context_provider acceptor_context_provider,
                                   io_context_provider connection_context_provider,
                                   std::set<std::string> allowed_remotes, 
                                   std::set<std::string> forbidden_remotes)
    : socket_server_base(std::move(acceptor_context_provider),
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
    own_acceptor_thread_ = true;
}

tcp_socket_server::~tcp_socket_server() {
    stop_acceptor_thread();
    close_acceptor();
}

bool tcp_socket_server::stop() {
    // First call base class to set running_ = false
    socket_server_base::stop();
    
    // Now close the acceptor, once its own thread (if any) is not using it
    stop_acceptor_thread();
    close_acceptor();
    
    return true;
}

void tcp_socket_server::stop_acceptor_thread() {
    // The thread object is kept while the acceptor uses its io_context
    if (acceptor_thread_) acceptor_thread_->stop();
}

void tcp_socket_server::close_acceptor() {
    // Close the acceptor to cancel pending async operations, but do NOT
    // destroy it (reset) here. The async_accept handler may still be in
    // flight on the io_context thread and needs the acceptor alive until
    // the handler completes. The unique_ptr will clean up on destruction.
    if (acceptor_ && acceptor_->is_open()) {
        boost::system::error_code ec;
        acceptor_->close(ec);
        if (ec) {
            LOG_WARNING("Error closing TCP acceptor: {}", ec.message());
        }
    }
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
    return acceptor_ ? acceptor_->local_endpoint().port() : 0;
}

bool tcp_socket_server::create_acceptor() {
    if (own_acceptor_thread_) {
        acceptor_.reset(); // it uses the io_context of the previous thread, if any
        acceptor_thread_ = std::make_unique<worker_thread>("tcp acceptor " + host_ + ":" + port_);
        acceptor_thread_->start();
        if (listen(acceptor_thread_->get_io_context())) return true;
        stop_acceptor_thread();
        return false;
    }
    return listen(acceptor_context_provider_());
}

bool tcp_socket_server::listen(boost::asio::io_context& io_context) {
    int num_attempts = 0;
    
    // Resolve endpoint
    boost::asio::ip::tcp::endpoint endpoint;
    try {
        boost::asio::ip::tcp::resolver resolver(io_context);
        auto results = resolver.resolve(host_, port_);
        if (results.begin() == results.end()) {
            LOG_ERROR("no endpoints found for {}:{}", host_, port_);
            return false;
        }
        
        auto entry = *results.begin();
        endpoint = entry.endpoint();
    } catch (const boost::system::system_error& e) {
        LOG_ERROR("failed to resolve {}:{} - {}", host_, port_, e.code().message());
        return false;
    }
    
    bool success = false;
    do {
        LOG_DEBUG("starting TCP socket acceptor on {}:{}", host_, port_);
        if (num_attempts > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
        
        protocol_ = endpoint.protocol();
        acceptor_ = std::make_unique<boost::asio::ip::tcp::acceptor>(io_context);
        acceptor_->open(endpoint.protocol());
        acceptor_->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
        
        try {
            LOG_DEBUG("binding and listening to endpoint: {}:{}", 
                     endpoint.address().to_string(), endpoint.port());
            acceptor_->bind(endpoint);
            acceptor_->listen();
            success = true;
        } catch (boost::system::system_error& error) {
            LOG_ERROR("cannot start listening on {}:{}: {}", 
                     host_, port_, error.code().message());
            // Reset acceptor if binding failed to avoid inconsistent state
            acceptor_.reset();
            if (max_listening_attempts_ >= 0 && num_attempts >= max_listening_attempts_) {
                return false;
            }
        }
        num_attempts++;
    } while (!success && (max_listening_attempts_ < 0 || num_attempts < max_listening_attempts_));

    if (success) {
        LOG_INFO("TCP server is now listening on {}:{}", host_, port_);
    }
    
    return success;
}

namespace {

// What an accepted connection needs to be set up, copied: the server may be stopped and gone
// by then
struct connection_setup {
    std::shared_ptr<boost::asio::ssl::context> ssl_context; // none without TLS
    bool tcp_no_delay;
    std::function<void(std::shared_ptr<socket>)> handler;
};

// An accepted descriptor on its way to the io_context serving it, closed if it never gets there
class accepted_descriptor {
public:
    explicit accepted_descriptor(int descriptor) : descriptor_(descriptor) {}
    accepted_descriptor(accepted_descriptor&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    accepted_descriptor& operator=(accepted_descriptor&&) = delete;
    ~accepted_descriptor() { if (descriptor_ >= 0) ::close(descriptor_); }

    int get() const { return descriptor_; }
    int release() { return std::exchange(descriptor_, -1); }

private:
    int descriptor_;
};

std::shared_ptr<tcp_socket> make_socket(boost::asio::io_context& io_context, const connection_setup& setup) {
    if (setup.ssl_context) return std::make_shared<ssl_socket>("ssl_socket_server", io_context, setup.ssl_context);
    return std::make_shared<tcp_socket>("tcp_socket_server", io_context);
}

// Set an accepted connection up and hand it to the handler
void serve_connection(std::shared_ptr<tcp_socket> sock, connection_setup setup) {
    LOG_INFO("received connection from: ip: {}, port: {}, secure: {}",
            sock->get_remote_ip(), sock->get_local_port(), sock->is_secure());

    if (setup.tcp_no_delay) {
        sock->enable_tcp_no_delay();
    }

    if (sock->requires_handshake()) {
        // Use co_spawn to run the coroutine-based handshake
        co_spawn(sock->get_io_context(),
            [sock, handler = std::move(setup.handler)]() -> awaitable<void> {
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

void tcp_socket_server::accept_connection() {
    if (ssl_enabled_ && !ssl_context_) {
        LOG_ERROR("SSL enabled but no SSL context configured");
        return;
    }

    // On a thread of its own, every pending connection is accepted at once, without waiting
    if (acceptor_thread_) {
        boost::system::error_code ec;
        acceptor_->non_blocking(true, ec);
        if (ec) {
            LOG_ERROR("cannot accept connections: {}", ec.message());
            return;
        }
        boost::asio::post(acceptor_thread_->get_io_context(), [this] { accept_pending(); });
        return;
    }

    // Get next io_context from provider
    boost::asio::io_context& io_context = connection_context_provider_();
    connection_setup setup{ssl_enabled_ ? ssl_context_ : nullptr, tcp_no_delay_, handler_};
    auto sock = make_socket(io_context, setup);
    auto& socket = sock->get_socket();
    
    // Start accepting a connection
    acceptor_->async_accept(socket, [sock = std::move(sock), setup = std::move(setup), this](const boost::system::error_code& e) mutable {
        if (!e) {
            // Check if IP is allowed (the remote address is only looked up to filter it)
            if (filters_remotes()) {
                auto remote_ip = sock->get_remote_ip();
                if (!is_remote_allowed(remote_ip)) {
                    sock->close();
                    LOG_WARNING("rejecting connection from: ip: {}, port: {}, secure: {}",
                               remote_ip, sock->get_local_port(), sock->is_secure());
                    if (running_) accept_connection();
                    return;
                }
            }

            serve_connection(std::move(sock), std::move(setup));

            // Continue accepting connections
            if (running_) accept_connection();
        } else if (e != boost::asio::error::operation_aborted) {
            LOG_ERROR("cannot accept more connections: {}", e.message());
            retry_accept();
        } else {
            LOG_INFO("stop accepting connections");
        }
    });
}

void tcp_socket_server::wait_connections() {
    acceptor_->async_wait(boost::asio::socket_base::wait_read, [this](const boost::system::error_code& e) {
        if (!e) {
            accept_pending();
        } else if (e != boost::asio::error::operation_aborted) {
            LOG_ERROR("cannot accept more connections: {}", e.message());
            retry_accept();
        } else {
            LOG_INFO("stop accepting connections");
        }
    });
}

void tcp_socket_server::accept_pending() {
    // Only this thread uses the acceptor (it is closed once the thread stops), and it is the
    // only one running its io_context, so no other thread takes its readiness meanwhile
    while (running_) {
        int descriptor = ::accept(acceptor_->native_handle(), nullptr, nullptr);
        if (descriptor >= 0) {
#if defined(SO_NOSIGPIPE)
            int enabled = 1;
            ::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
            hand_over(descriptor);
            continue;
        }
        int error = errno;
        if (error == EINTR || error == ECONNABORTED || error == EPROTO) continue;
        if (error == EAGAIN || error == EWOULDBLOCK) {
            wait_connections();
        } else {
            LOG_ERROR("cannot accept more connections: {}", std::error_code(error, std::system_category()).message());
            retry_accept();
        }
        return;
    }
}

void tcp_socket_server::hand_over(int accepted) {
    accepted_descriptor descriptor(accepted);

    // Check if IP is allowed (the remote address is only looked up to filter it)
    if (filters_remotes()) {
        boost::asio::ip::tcp::endpoint remote;
        auto size = static_cast<socklen_t>(remote.capacity());
        std::string remote_ip = "0.0.0.0";
        if (::getpeername(descriptor.get(), remote.data(), &size) == 0) {
            remote.resize(size);
            remote_ip = remote.address().to_string();
        }
        if (!is_remote_allowed(remote_ip)) {
            LOG_WARNING("rejecting connection from: ip: {}, port: {}, secure: {}",
                       remote_ip, local_port(), ssl_enabled_);
            return;
        }
    }

    // The connection is registered and set up on the worker serving it, waking it up once
    boost::asio::io_context& io_context = connection_context_provider_();
    boost::asio::post(io_context,
        [&io_context, descriptor = std::move(descriptor), protocol = protocol_,
         setup = connection_setup{ssl_enabled_ ? ssl_context_ : nullptr, tcp_no_delay_, handler_}]() mutable {
            auto sock = make_socket(io_context, setup);
            boost::system::error_code ec;
            sock->get_socket().assign(protocol, descriptor.get(), ec);
            if (ec) {
                LOG_ERROR("cannot serve an accepted connection: {}", ec.message());
                return;
            }
            descriptor.release();
            serve_connection(std::move(sock), std::move(setup));
        });
}

void tcp_socket_server::retry_accept() {
    if (!running_) return;
    // Retry after a delay to avoid tight loop on persistent errors
    auto timer = std::make_shared<boost::asio::steady_timer>(acceptor_->get_executor(), std::chrono::seconds(1));
    timer->async_wait([this, timer](const boost::system::error_code& e) {
        if (e == boost::asio::error::operation_aborted) return;
        if (acceptor_thread_) accept_pending();
        else accept_connection();
    });
}

} // namespace thinger::asio
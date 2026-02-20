#include "tcp_socket_server.hpp"
#include "workers.hpp"
#include "../util/logger.hpp"
#include "../util/types.hpp"
#include <boost/asio/ssl.hpp>

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
}

tcp_socket_server::~tcp_socket_server() {
    close_acceptor();
}

bool tcp_socket_server::stop() {
    // First call base class to set running_ = false
    socket_server_base::stop();
    
    // Now close the acceptor
    close_acceptor();
    
    return true;
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
    int num_attempts = 0;
    
    // Get io_context from provider
    boost::asio::io_context& io_context = acceptor_context_provider_();
    
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

void tcp_socket_server::accept_connection() {
    // Get next io_context from provider
    boost::asio::io_context& io_context = connection_context_provider_();
    
    // Create socket based on SSL configuration
    std::shared_ptr<tcp_socket> sock;
    if (ssl_enabled_) {
        if (!ssl_context_) {
            LOG_ERROR("SSL enabled but no SSL context configured");
            return;
        }
        sock = std::make_shared<ssl_socket>("ssl_socket_server", io_context, ssl_context_);
    } else {
        sock = std::make_shared<tcp_socket>("tcp_socket_server", io_context);
    }

    auto& socket = sock->get_socket();
    
    // Start accepting a connection
    acceptor_->async_accept(socket, [sock = std::move(sock), this](const boost::system::error_code& e) mutable {
        if (!e) {
            // Get remote socket ip
            auto remote_ip = sock->get_remote_ip();

            // Check if IP is allowed
            if (!is_remote_allowed(remote_ip)) {
                sock->close();
                LOG_WARNING("rejecting connection from: ip: {}, port: {}, secure: {}", 
                           remote_ip, sock->get_local_port(), sock->is_secure());
                if (running_) accept_connection();
                return;
            }

            LOG_INFO("received connection from: ip: {}, port: {}, secure: {}", 
                    remote_ip, sock->get_local_port(), sock->is_secure());

            if (tcp_no_delay_) {
                sock->enable_tcp_no_delay();
            }

            if (sock->requires_handshake()) {
                // Use co_spawn to run the coroutine-based handshake
                co_spawn(sock->get_io_context(),
                    [this, sock]() -> awaitable<void> {
                        auto ec = co_await sock->handshake();
                        if (ec) {
                            LOG_ERROR("error while handling SSL handshake: {}, remote ip: {}",
                                     ec.message(), sock->get_remote_ip());
                            co_return;
                        }
                        if (handler_) handler_(sock);
                    },
                    detached);
            } else {
                if (handler_) handler_(std::move(sock));
            }

            // Continue accepting connections
            if (running_) accept_connection();
        } else {
            if (e != boost::asio::error::operation_aborted) {
                LOG_ERROR("cannot accept more connections: {}", e.message());
                if (running_) {
                    // Retry after a delay to avoid tight loop on persistent errors
                    auto timer = std::make_shared<boost::asio::steady_timer>(
                        acceptor_context_provider_(),
                        std::chrono::seconds(1)
                    );
                    timer->async_wait([this, timer](const boost::system::error_code& e) {
                        if (e != boost::asio::error::operation_aborted) {
                            accept_connection();
                        }
                    });
                }
            } else {
                LOG_INFO("stop accepting connections");
            }
        }
    });
}

} // namespace thinger::asio
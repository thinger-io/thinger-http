#include "unix_socket_server.hpp"
#include "workers.hpp"
#include "../util/logger.hpp"
#include <filesystem>

namespace thinger::asio {

// Constructor with io_context providers
unix_socket_server::unix_socket_server(std::string unix_path,
                                     io_context_provider acceptor_context_provider,
                                     io_context_provider connection_context_provider,
                                     std::set<std::string> allowed_remotes, 
                                     std::set<std::string> forbidden_remotes)
    : socket_server_base(std::move(acceptor_context_provider),
                        std::move(connection_context_provider),
                        std::move(allowed_remotes),
                        std::move(forbidden_remotes))
    , unix_path_(std::move(unix_path))
{
}

// Legacy constructor for backward compatibility
unix_socket_server::unix_socket_server(std::string unix_path,
                                     std::set<std::string> allowed_remotes, 
                                     std::set<std::string> forbidden_remotes)
    : unix_socket_server(unix_path,
                        []() -> boost::asio::io_context& { return get_workers().get_thread_io_context(); },
                        []() -> boost::asio::io_context& { return get_workers().get_next_io_context(); },
                        std::move(allowed_remotes), 
                        std::move(forbidden_remotes))
{
}

unix_socket_server::~unix_socket_server() {
    stop();
}

bool unix_socket_server::stop() {
    // First call base class to set running_ = false
    socket_server_base::stop();
    
    // Now close the acceptor safely
    if (acceptor_) {
        boost::system::error_code ec;
        acceptor_->close(ec);
        if (ec) {
            LOG_WARNING("Error closing Unix acceptor: {}", ec.message());
        }
        acceptor_.reset();
    }
    
    // Remove the socket file when server is stopped
    if (!unix_path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(unix_path_, ec);
        if (ec) {
            LOG_WARNING("Failed to remove Unix socket file {}: {}", unix_path_, ec.message());
        }
    }
    
    return true;
}

std::string unix_socket_server::get_service_name() const {
    return "unix_server@" + unix_path_;
}

bool unix_socket_server::create_acceptor() {
    // Get io_context from provider
    boost::asio::io_context& io_context = acceptor_context_provider_();
    
    // Remove existing socket file if it exists
    std::error_code ec;
    std::filesystem::remove(unix_path_, ec);
    
    // Create the endpoint
    boost::asio::local::stream_protocol::endpoint endpoint(unix_path_);
    
    int num_attempts = 0;
    bool success = false;
    
    do {
        LOG_DEBUG("starting Unix socket acceptor on {}", unix_path_);
        if (num_attempts > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
        
        try {
            acceptor_ = std::make_unique<boost::asio::local::stream_protocol::acceptor>(io_context);
            acceptor_->open(endpoint.protocol());
            acceptor_->bind(endpoint);
            acceptor_->listen();
            success = true;
        } catch (boost::system::system_error& error) {
            LOG_ERROR("cannot start listening on Unix socket {}: {}", 
                     unix_path_, error.code().message());
            if (max_listening_attempts_ >= 0 && num_attempts >= max_listening_attempts_) {
                return false;
            }
        }
        num_attempts++;
    } while (!success && (max_listening_attempts_ < 0 || num_attempts < max_listening_attempts_));

    if (success) {
        LOG_INFO("Unix socket server is now listening on {}", unix_path_);
        
        // Set permissions to allow access (you might want to make this configurable)
        std::filesystem::permissions(unix_path_,
                                   std::filesystem::perms::owner_all |
                                   std::filesystem::perms::group_read |
                                   std::filesystem::perms::group_write,
                                   std::filesystem::perm_options::replace);
    }
    
    return success;
}

void unix_socket_server::accept_connection() {
    // Use async_accept with move semantics for the socket
    acceptor_->async_accept([this](const boost::system::error_code& e, 
                           boost::asio::local::stream_protocol::socket peer) mutable {
        if (!e) {
            // Create unix_socket with the already-connected socket
            auto sock = std::make_shared<unix_socket>("unix_socket_server", std::move(peer));
            
            LOG_INFO("received connection on Unix socket: {}", unix_path_);

            // Call handler with the connected socket
            if (handler_) {
                handler_(std::move(sock));
            }

            // Continue accepting connections
            if (running_) accept_connection();
        } else {
            if (e != boost::asio::error::operation_aborted) {
                LOG_ERROR("cannot accept more Unix socket connections: {}", e.message());
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
                LOG_INFO("stop accepting Unix socket connections");
            }
        }
    });
}

} // namespace thinger::asio
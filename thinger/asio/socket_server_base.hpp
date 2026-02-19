#ifndef THINGER_ASIO_SOCKET_SERVER_BASE_HPP
#define THINGER_ASIO_SOCKET_SERVER_BASE_HPP

#include "sockets/socket.hpp"
#include "../util/logger.hpp"

#include <memory>
#include <set>
#include <functional>
#include <boost/noncopyable.hpp>
#include <boost/asio.hpp>

namespace thinger::asio {

// Type for providing io_context
using io_context_provider = std::function<boost::asio::io_context&()>;

class socket_server_base : private boost::noncopyable {
public:
    static constexpr int MAX_LISTENING_ATTEMPTS = -1;

    socket_server_base(io_context_provider acceptor_context_provider,
                      io_context_provider connection_context_provider,
                      std::set<std::string> allowed_remotes = {}, 
                      std::set<std::string> forbidden_remotes = {});
    
    virtual ~socket_server_base() = default;

    // Configuration
    void set_max_listening_attempts(int attempts);
    void set_handler(std::function<void(std::shared_ptr<socket>)> handler);
    void set_allowed_remotes(std::set<std::string> allowed);
    void set_forbidden_remotes(std::set<std::string> forbidden);

    // Control
    bool start();
    virtual bool stop();
    bool is_running() const { return running_; }
    virtual std::string get_service_name() const = 0;
    virtual uint16_t local_port() const = 0;

protected:
    // Pure virtual methods that derived classes must implement
    virtual bool create_acceptor() = 0;
    virtual void accept_connection() = 0;
    
    // Helper method for IP filtering
    bool is_remote_allowed(const std::string& remote_ip) const;

protected:
    std::function<void(std::shared_ptr<socket>)> handler_;
    std::set<std::string> allowed_remotes_;
    std::set<std::string> forbidden_remotes_;
    int max_listening_attempts_;
    bool running_ = false;
    
    // io_context providers
    io_context_provider acceptor_context_provider_;
    io_context_provider connection_context_provider_;
};

} // namespace thinger::asio

#endif // THINGER_ASIO_SOCKET_SERVER_BASE_HPP
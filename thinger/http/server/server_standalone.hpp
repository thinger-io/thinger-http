#ifndef THINGER_HTTP_SERVER_STANDALONE_HPP
#define THINGER_HTTP_SERVER_STANDALONE_HPP

#include "http_server_base.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>

namespace thinger::http {

/**
 * Standalone HTTP server that uses its own io_context.
 * Perfect for simple applications that don't need the full worker infrastructure.
 * Single-threaded server that runs in the thread that calls wait().
 */
class server : public http_server_base {
private:
    // Own io_context for handling async operations
    boost::asio::io_context io_context_;
    
    // Work guard to keep io_context running
    using work_guard_type = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
    std::unique_ptr<work_guard_type> work_guard_;
    
    std::atomic<bool> running_{false};
    
protected:
    // Create socket server with our io_context
    std::unique_ptr<asio::socket_server> create_socket_server(
        const std::string& host, const std::string& port) override;
    
    // Create Unix socket server with our io_context
    std::unique_ptr<asio::unix_socket_server> create_unix_socket_server(
        const std::string& unix_path) override;
    
public:
    // Constructor - single threaded server
    server();
    ~server() override;
    
    // Override listen to setup server
    bool listen(const std::string& host, uint16_t port) override;
    
    // Wait for server to stop
    void wait() override;
    
    // Stop the server
    bool stop() override;
    
    // Get direct access to io_context for advanced use cases
    boost::asio::io_context& get_io_context() { 
        return io_context_; 
    }
};

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_STANDALONE_HPP
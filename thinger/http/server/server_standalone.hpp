#ifndef THINGER_HTTP_SERVER_STANDALONE_HPP
#define THINGER_HTTP_SERVER_STANDALONE_HPP

#include "server_base.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>

namespace thinger::http {

/**
 * Standalone HTTP server that uses its own io_context.
 * Perfect for simple applications that don't need the full worker infrastructure.
 * Single-threaded server that runs in the thread that calls wait(): every listener accepts and
 * serves on it.
 */
class server : public server_base {
private:
    // Own io_context for handling async operations
    boost::asio::io_context io_context_;
    
    // Work guard to keep io_context running
    using work_guard_type = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
    std::unique_ptr<work_guard_type> work_guard_;
    
    std::atomic<bool> running_{false};
    // A thread runs the io_context (see wait())
    std::atomic<bool> serving_{false};

    // Stop listening and the io_context, releasing wait()
    void stop_running();
    
protected:
    // A listener accepting and serving on our io_context
    std::unique_ptr<asio::listener> create_listener(asio::listener_options options) override;
    // The first listener opened: the io_context may run (see wait())
    void listening() override;

    boost::asio::io_context* shutdown_context() override;
    bool in_server_thread() override;
    
public:
    // Constructor - single threaded server
    server();
    ~server() override;
    
    // Run the io_context in the calling thread until the server stops: at once if it does not
    // listen
    void wait() override;
    
    // Stop the server
    void stop() override;
    
    // Get direct access to io_context for advanced use cases
    boost::asio::io_context& get_io_context() { 
        return io_context_; 
    }
};

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_STANDALONE_HPP
#ifndef THINGER_HTTP_POOL_SERVER_HPP
#define THINGER_HTTP_POOL_SERVER_HPP

#include "http_server_base.hpp"
#include "../../asio/worker_client.hpp"
#include "../../asio/workers.hpp"

namespace thinger::http {

/**
 * HTTP server integrated with the worker thread pool.
 * Designed for high-performance server applications where
 * the server runs alongside other asynchronous services.
 * Uses the global worker thread pool for optimal resource sharing.
 */
class pool_server : public http_server_base, public asio::worker_client {
protected:
    // Create socket server that uses worker pool
    std::unique_ptr<asio::socket_server> create_socket_server(
        const std::string& host, const std::string& port) override;
    
    // Create Unix socket server that uses worker pool
    std::unique_ptr<asio::unix_socket_server> create_unix_socket_server(
        const std::string& unix_path) override;
    
public:
    pool_server();
    ~pool_server() override;

    // Expose http_server_base::start overloads (hidden by worker_client::start)
    using http_server_base::start;
    
    // Implementation of worker_client interface
    bool start() override { return true; } // Already started in listen()
    bool stop() override;
    void wait() override;
};

} // namespace thinger::http

#endif // THINGER_HTTP_POOL_SERVER_HPP
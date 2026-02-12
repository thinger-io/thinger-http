#include "pool_server.hpp"
#include "../../util/logger.hpp"
#include "../../asio/ssl/certificate_manager.hpp"

namespace thinger::http {

pool_server::pool_server() 
    : worker_client("http_pool_server") {
    LOG_DEBUG("Created HTTP pool server");
}

pool_server::~pool_server() {
    LOG_DEBUG("Destroying HTTP pool server");
    if (is_running()) {
        stop();
    }
}

std::unique_ptr<asio::socket_server> pool_server::create_socket_server(
    const std::string& host, const std::string& port) {
    
    // Use legacy constructor that automatically uses workers
    auto server = std::make_unique<asio::socket_server>(host, port);
    
    // Configure SSL if enabled
    if (ssl_enabled_) {
        server->enable_ssl(true);
        
        auto& cert_mgr = asio::certificate_manager::instance();
        auto default_ctx = cert_mgr.get_default_certificate();
        
        if (!default_ctx) {
            LOG_ERROR("No default SSL certificate configured");
            throw std::runtime_error("No default SSL certificate configured");
        }
        
        server->set_ssl_context(default_ctx);
        server->set_sni_callback(asio::certificate_manager::sni_callback);
    }
    
    return server;
}

std::unique_ptr<asio::unix_socket_server> pool_server::create_unix_socket_server(
    const std::string& unix_path) {
    
    // Use legacy constructor that automatically uses workers
    auto server = std::make_unique<asio::unix_socket_server>(unix_path);
    
    return server;
}

bool pool_server::stop() {
    // Stop the socket server
    bool result = http_server_base::stop();
    
    // Call base class implementation to handle notifications
    worker_client::stop();
    
    return result;
}

void pool_server::wait() {
    // Use worker_client's wait implementation
    worker_client::wait();
}

} // namespace thinger::http
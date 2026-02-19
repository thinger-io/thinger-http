#include "server_standalone.hpp"
#include "../../util/logger.hpp"
#include "../../asio/ssl/certificate_manager.hpp"
#include <future>

namespace thinger::http {

server::server() {
    LOG_DEBUG("Created standalone HTTP server (single-threaded)");
}

server::~server() {
    LOG_DEBUG("Destroying standalone HTTP server");
    if (running_) {
        stop();
    }
}


std::unique_ptr<asio::socket_server> server::create_socket_server(
    const std::string& host, const std::string& port) {
    
    // Both acceptor and connections use our io_context
    auto context_provider = [this]() -> boost::asio::io_context& {
        return io_context_;
    };
    
    auto server = std::make_unique<asio::socket_server>(
        host, port,
        context_provider,    // acceptor context
        context_provider     // connection context
    );
    
    // Configure SSL if enabled
    if (ssl_enabled_) {
        server->enable_ssl(true);
        
        auto& cert_mgr = asio::certificate_manager::instance();
        auto default_ctx = cert_mgr.get_default_certificate();
        
        if (!default_ctx) {
            LOG_ERROR("No default SSL certificate configured");
            return nullptr;
        }

        server->set_ssl_context(default_ctx);
        server->set_sni_callback(asio::certificate_manager::sni_callback);
    }

    return server;
}

std::unique_ptr<asio::unix_socket_server> server::create_unix_socket_server(
    const std::string& unix_path) {
    
    // Both acceptor and connections use our io_context
    auto context_provider = [this]() -> boost::asio::io_context& {
        return io_context_;
    };
    
    auto server = std::make_unique<asio::unix_socket_server>(
        unix_path,
        context_provider,    // acceptor context
        context_provider     // connection context
    );
    
    return server;
}

bool server::listen(const std::string& host, uint16_t port) {
    if (running_) {
        LOG_WARNING("Server already running");
        return false;
    }
    
    // Restart io_context if it was previously stopped
    io_context_.restart();
    
    // Call base implementation to setup socket server
    if (!http_server_base::listen(host, port)) {
        return false;
    }
    
    // Create work guard to keep io_context running
    work_guard_ = std::make_unique<work_guard_type>(io_context_.get_executor());
    
    // Mark as running
    running_ = true;
    LOG_INFO("Standalone server listening on {}:{} (single-threaded)", host, port);
    
    // Note: io_context_.run() will be called in wait() method
    
    return true;
}

bool server::listen_unix(const std::string& unix_path) {
    if (running_) {
        LOG_WARNING("Server already running");
        return false;
    }

    // Restart io_context if it was previously stopped
    io_context_.restart();

    // Call base implementation to setup unix socket server
    if (!http_server_base::listen_unix(unix_path)) {
        return false;
    }

    // Create work guard to keep io_context running
    work_guard_ = std::make_unique<work_guard_type>(io_context_.get_executor());

    // Mark as running
    running_ = true;
    LOG_INFO("Standalone server listening on unix:{} (single-threaded)", unix_path);

    return true;
}

void server::wait() {
    if (!running_) {
        return;
    }
    
    // Run io_context in the current thread
    // This blocks until the server is stopped
    LOG_DEBUG("Running io_context in current thread");
    io_context_.run();
    LOG_DEBUG("io_context stopped");
}

bool server::stop() {
    if (!running_) {
        return false;
    }
    
    LOG_DEBUG("Stopping standalone HTTP server");
    running_ = false;
    
    // Stop accepting new connections first (this is always safe to call)
    http_server_base::stop();
    
    // Reset work guard to allow io_context to stop when it's running
    work_guard_.reset();
    
    // Stop the io_context if it's running
    // Note: io_context_.stop() is safe to call even if not running
    io_context_.stop();
    
    LOG_DEBUG("Standalone HTTP server stopped");
    return true;
}

} // namespace thinger::http
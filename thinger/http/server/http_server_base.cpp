#include "http_server_base.hpp"
#include "server_connection.hpp"
#include "request.hpp"
#include "../../util/logger.hpp"

namespace thinger::http {

// Configuration
void http_server_base::enable_ssl(bool enabled) {
    ssl_enabled_ = enabled;
}

void http_server_base::set_connection_timeout(std::chrono::seconds timeout) {
    connection_timeout_ = timeout;
}

void http_server_base::set_max_listening_attempts(int attempts) {
    max_listening_attempts_ = attempts;
}

void http_server_base::log_bodies(bool enabled, size_t max_length) {
    body_logging_ = {enabled, max_length};
}

// Server control
bool http_server_base::listen(const std::string& host, uint16_t port) {
    reset_shutdown();
    host_ = host;
    port_ = std::to_string(port);
    use_unix_socket_ = false;
    
    // Create socket server using virtual method
    socket_server_ = create_socket_server(host, port_);
    if (!socket_server_) {
        LOG_ERROR("Failed to create socket server");
        return false;
    }
    
    // Configure socket server
    socket_server_->set_max_listening_attempts(max_listening_attempts_);
    
    // Setup connection handler
    setup_connection_handler();
    
    // Start listening
    if (!socket_server_->start()) return false;
    log_listening();
    return true;
}

bool http_server_base::listen_unix(const std::string& unix_path) {
    reset_shutdown();
    unix_path_ = unix_path;
    use_unix_socket_ = true;
    
    // Create Unix socket server using virtual method
    socket_server_ = create_unix_socket_server(unix_path);
    if (!socket_server_) {
        LOG_ERROR("Failed to create Unix socket server");
        return false;
    }
    
    // Configure socket server
    socket_server_->set_max_listening_attempts(max_listening_attempts_);
    
    // Setup connection handler
    setup_connection_handler();
    
    // Start listening
    if (!socket_server_->start()) return false;
    log_listening();
    return true;
}

bool http_server_base::start(uint16_t port) {
    return start("0.0.0.0", port);
}

bool http_server_base::start(uint16_t port, const sync_callback<void()> &on_listening) {
    return start("0.0.0.0", port, on_listening);
}

bool http_server_base::start(const std::string& host, uint16_t port) {
    return start(host, port, nullptr);
}

bool http_server_base::start(const std::string& host, uint16_t port, const sync_callback<void()> &on_listening) {
    if (!listen(host, port)) {
        return false;
    }
    if (on_listening) {
        on_listening();
    }
    wait();
    return true;
}

bool http_server_base::start_unix(const std::string& unix_path) {
    return start_unix(unix_path, nullptr);
}

bool http_server_base::start_unix(const std::string& unix_path, const sync_callback<void()> &on_listening) {
    if (!listen_unix(unix_path)) {
        return false;
    }
    if (on_listening) {
        on_listening();
    }
    wait();
    return true;
}

bool http_server_base::stop() {
    end_shutdown();
    // Not when a shutdown stops it: it stopped listening already, and the shutdown tells
    bool listening = is_listening();
    bool stopped = stop_listening();
    if (listening) LOG_INFO("{} stopped", log_name());
    return stopped;
}

bool http_server_base::stop_listening() {
    if (socket_server_) {
        bool result = socket_server_->stop();
        socket_server_.reset();
        return result;
    }
    return false;
}

bool http_server_base::is_listening() const {
    return socket_server_ != nullptr && socket_server_->is_running();
}

uint16_t http_server_base::local_port() const {
    return socket_server_ ? socket_server_->local_port() : 0;
}


// Private methods
void http_server_base::log_listening() {
    if (use_unix_socket_) {
        listening_on_ = "unix:" + unix_path_;
    } else {
        // The port bound (for port 0, the one chosen), and IPv6 addresses in brackets
        auto host = host_.find(':') == std::string::npos ? host_ : "[" + host_ + "]";
        listening_on_ = (ssl_enabled_ ? "https://" : "http://") + host + ":" + std::to_string(local_port());
    }
    LOG_INFO("listening on {}", listening_on_);
}

std::string http_server_base::log_name() const {
    return listening_on_.empty() ? "server" : "server on " + listening_on_;
}

void http_server_base::setup_connection_handler() {
    socket_server_->set_handler([this](std::shared_ptr<asio::socket> socket) {
        auto connection = std::make_shared<server_connection>(socket);
        connection->track(connections_);
        connection->set_body_logging(body_logging_);

        connection->set_handler([this](std::shared_ptr<request> req) {
            auto sink = std::make_shared<connection_sink>(*req);
            return handle(std::move(req), std::move(sink));
        });

        // Start handling the connection with configured timeout
        connection->start(connection_timeout_);
    });
}

} // namespace thinger::http

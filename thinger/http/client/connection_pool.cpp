#include "connection_pool.hpp"
#include "../../util/logger.hpp"

namespace thinger::http {

connection_pool::~connection_pool() {
    // Always close connections when destroying the pool
    // There's no point keeping them alive as they won't be reachable
    clear();
}

std::shared_ptr<client_connection> connection_pool::get_connection(const std::string& host, 
                                                                   uint16_t port, 
                                                                   bool ssl) {
    return get_connection_impl(host, port, ssl, "");
}

std::shared_ptr<client_connection> connection_pool::get_unix_connection(const std::string& unix_path) {
    return get_connection_impl("", 0, false, unix_path);
}

void connection_pool::store_connection(const std::string& host, 
                                      uint16_t port, 
                                      bool ssl,
                                      std::shared_ptr<client_connection> connection) {
    store_connection_impl(host, port, ssl, "", connection);
}

void connection_pool::store_unix_connection(const std::string& unix_path,
                                           std::shared_ptr<client_connection> connection) {
    store_connection_impl("", 0, false, unix_path, connection);
}

size_t connection_pool::cleanup_expired() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    size_t removed = 0;
    auto& seq_index = connections_.get<by_sequence>();
    auto it = seq_index.begin();
    while (it != seq_index.end()) {
        if (it->connection.expired()) {
            it = seq_index.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

size_t connection_pool::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return connections_.size();
}

void connection_pool::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    // Always close all active connections gracefully before clearing
    // There's no use case for keeping connections alive outside the pool
    auto& seq_index = connections_.get<by_sequence>();
    for (auto it = seq_index.begin(); it != seq_index.end(); ++it) {
        if (auto conn = it->connection.lock()) {
            // Use the connection's close method to ensure graceful shutdown
            // This will notify all pending requests with an error
            conn->close();
        }
    }
    
    connections_.clear();
}

std::shared_ptr<client_connection> connection_pool::get_connection_impl(const std::string& host,
                                                                       uint16_t port,
                                                                       bool ssl,
                                                                       const std::string& unix_path) {
    // First try with shared lock for reading
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto& key_index = connections_.get<by_key>();
        auto it = key_index.find(std::make_tuple(host, port, ssl, unix_path));
        
        if (it != key_index.end()) {
            // Try to convert weak_ptr to shared_ptr
            if (auto conn = it->connection.lock()) {
                // Connection is still alive, return it
                return conn;
            }
            // Connection expired, need to remove it with write lock
        } else {
            // Not found
            return nullptr;
        }
    }
    
    // Only take write lock if we need to remove an expired connection
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& key_index = connections_.get<by_key>();
    auto it = key_index.find(std::make_tuple(host, port, ssl, unix_path));
    
    if (it != key_index.end() && it->connection.expired()) {
        key_index.erase(it);
    }
    
    return nullptr;
}

void connection_pool::store_connection_impl(const std::string& host,
                                           uint16_t port,
                                           bool ssl,
                                           const std::string& unix_path,
                                           std::shared_ptr<client_connection> connection) {
    if (!connection) return;
    
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    // Remove any existing entry with the same key
    auto& key_index = connections_.get<by_key>();
    auto it = key_index.find(std::make_tuple(host, port, ssl, unix_path));
    if (it != key_index.end()) {
        key_index.erase(it);
    }
    
    // Insert the new connection
    connections_.emplace(host, port, ssl, unix_path, connection);
}

} // namespace thinger::http
#ifndef THINGER_HTTP_CLIENT_CONNECTION_POOL_HPP
#define THINGER_HTTP_CLIENT_CONNECTION_POOL_HPP

#include <memory>
#include <string>
#include <shared_mutex>
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include "client_connection.hpp"

namespace thinger::http {

class connection_pool {
private:
    // Connection entry in the pool
    struct connection_entry {
        std::string host;
        uint16_t port;
        bool ssl;
        std::string unix_socket_path;  // Empty for TCP connections
        std::weak_ptr<client_connection> connection;
        
        connection_entry(const std::string& h, uint16_t p, bool s, 
                        const std::string& unix_path,
                        std::shared_ptr<client_connection> conn)
            : host(h), port(p), ssl(s), unix_socket_path(unix_path), connection(conn) {}
    };
    
    // Tags for multi_index_container
    struct by_key {};
    struct by_sequence {};
    
    // Define the multi_index_container with composite key indexing
    using connection_container = boost::multi_index_container<
        connection_entry,
        boost::multi_index::indexed_by<
            // Hashed index by composite key (host, port, ssl, unix_socket_path)
            boost::multi_index::hashed_unique<
                boost::multi_index::tag<by_key>,
                boost::multi_index::composite_key<
                    connection_entry,
                    boost::multi_index::member<connection_entry, std::string, &connection_entry::host>,
                    boost::multi_index::member<connection_entry, uint16_t, &connection_entry::port>,
                    boost::multi_index::member<connection_entry, bool, &connection_entry::ssl>,
                    boost::multi_index::member<connection_entry, std::string, &connection_entry::unix_socket_path>
                >
            >,
            // Sequenced index for LRU-style access if needed in future
            boost::multi_index::sequenced<
                boost::multi_index::tag<by_sequence>
            >
        >
    >;
    
    connection_container connections_;
    mutable std::shared_mutex mutex_;
    
public:
    connection_pool() = default;
    ~connection_pool();
    
    // Get a connection from the pool (returns nullptr if not found or expired)
    std::shared_ptr<client_connection> get_connection(const std::string& host, 
                                                     uint16_t port, 
                                                     bool ssl);
    
    // Get a unix socket connection from the pool
    std::shared_ptr<client_connection> get_unix_connection(const std::string& unix_path);
    
    // Store a connection in the pool
    void store_connection(const std::string& host, 
                         uint16_t port, 
                         bool ssl,
                         std::shared_ptr<client_connection> connection);
    
    // Store a unix socket connection in the pool
    void store_unix_connection(const std::string& unix_path,
                              std::shared_ptr<client_connection> connection);
    
    // Remove expired connections (optional cleanup method)
    // Returns the number of connections removed
    size_t cleanup_expired();
    
    // Get the number of connections in the pool
    size_t size() const;
    
    // Clear all connections from the pool (always closes them)
    void clear();
    
private:
    std::shared_ptr<client_connection> get_connection_impl(const std::string& host,
                                                          uint16_t port,
                                                          bool ssl,
                                                          const std::string& unix_path);
    
    void store_connection_impl(const std::string& host,
                              uint16_t port,
                              bool ssl,
                              const std::string& unix_path,
                              std::shared_ptr<client_connection> connection);
};

} // namespace thinger::http

#endif // THINGER_HTTP_CLIENT_CONNECTION_POOL_HPP
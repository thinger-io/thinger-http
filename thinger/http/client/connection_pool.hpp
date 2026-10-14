#ifndef THINGER_HTTP_CLIENT_CONNECTION_POOL_HPP
#define THINGER_HTTP_CLIENT_CONNECTION_POOL_HPP

#include <chrono>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "client_connection.hpp"

namespace thinger::http {

/**
 * Keep-alive pool of the connections of a client (RFC 9112 section 9.3). A connection is lent
 * to one request at a time (a lease), and given back when the request ends: kept idle for the
 * next request with the same key if it can carry another one, closed otherwise. The key tells
 * which connections are interchangeable: same origin (or Unix socket), reached the same way
 * (directly or through which proxy, trusting which CAs).
 *
 * Idle connections are bounded per key and in total (the oldest is closed to make room),
 * and closed after an idle timeout, shortened to the one the server announces
 * (Keep-Alive: timeout=N) minus a margin. One taken from the pool is checked first, without
 * waiting: if the server closed it, reset it or sent something unasked, it is closed and the
 * next one tried. Thread-safe: a lease may be taken and given back on any thread.
 */
class connection_pool {
public:
    struct config {
        // Idle connections kept per key (0: none, the pool is disabled)
        size_t max_idle_per_host = 8;
        // Idle connections kept in total (0: none)
        size_t max_idle = 100;
        // Time an idle connection is kept
        std::chrono::milliseconds idle_timeout{4000};
    };

    // Subtracted from the idle timeout a server announces, so the connection is not reused
    // just as the server closes it
    static constexpr std::chrono::milliseconds keep_alive_margin{1000};

    struct host_stats {
        size_t idle = 0;
        size_t active = 0;
    };

    struct stats {
        size_t idle = 0;            // connections kept idle
        size_t active = 0;          // connections lent to requests
        size_t created = 0;         // connections created (since the pool was)
        size_t reused = 0;          // requests sent on an idle connection
        std::map<std::string, host_stats> hosts;    // by origin ("https://host:port", "unix:/path")
    };

private:
    struct state;

public:
    // A connection lent to a request, given back to the pool when released or destroyed
    class lease {
    public:
        lease() = default;
        lease(lease&& other) noexcept = default;
        lease& operator=(lease&& other) noexcept;
        lease(const lease&) = delete;
        lease& operator=(const lease&) = delete;
        ~lease() { release(); }

        explicit operator bool() const { return connection_ != nullptr; }
        client_connection* operator->() const { return connection_.get(); }
        client_connection& operator*() const { return *connection_; }
        const std::shared_ptr<client_connection>& connection() const { return connection_; }

        // Give the connection back: kept idle if it can carry another request, closed
        // otherwise. The lease is empty afterwards
        void release();

    private:
        friend class connection_pool;
        lease(std::weak_ptr<state> pool, std::string key, std::shared_ptr<client_connection> connection)
            : pool_(std::move(pool)), key_(std::move(key)), connection_(std::move(connection)) {}

        std::weak_ptr<state> pool_;
        std::string key_;
        std::shared_ptr<client_connection> connection_;
    };

    connection_pool();
    ~connection_pool();
    connection_pool(const connection_pool&) = delete;
    connection_pool& operator=(const connection_pool&) = delete;

    // Limits, applied to the connections already idle too
    void configure(const config& config);
    config get_config() const;

    // Close each idle connection with a timer on its io_context (the default). Without them,
    // expired connections are closed whenever the pool is used (a connection taken or given
    // back): for an io_context that only runs while requests do, as a pending timer would
    // keep it running
    void use_idle_timers(bool enabled);

    // An idle connection for `key`, claimed for a request; empty if there is none usable
    lease acquire(const std::string& key);

    // Lend a new connection for `key`; `origin` names it in the stats
    lease add(const std::string& key, const std::string& origin, std::shared_ptr<client_connection> connection);

    // Idle connections
    size_t size() const;

    stats get_stats() const;

    // Close the idle connections (those lent are not affected)
    void clear();

private:
    std::shared_ptr<state> state_;
};

} // namespace thinger::http

#endif // THINGER_HTTP_CLIENT_CONNECTION_POOL_HPP

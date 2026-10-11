#include "connection_pool.hpp"
#include "../../util/logger.hpp"

#include <algorithm>

namespace thinger::http {

struct connection_pool::state : std::enable_shared_from_this<state> {
    struct idle_entry {
        std::string key;
        std::shared_ptr<client_connection> connection;
        std::chrono::steady_clock::time_point expires;
        uint64_t id;
    };
    using idle_list = std::list<idle_entry>;

    struct host {
        std::string origin;
        std::vector<idle_list::iterator> idle;     // oldest first
        size_t active = 0;
    };

    mutable std::mutex mutex;
    config limits;
    idle_list idle;                                 // oldest first, of every key
    std::unordered_map<std::string, host> hosts;
    uint64_t next_id = 0;
    size_t created = 0;
    size_t reused = 0;
    bool closed = false;
    bool timers = true;

    // Take an idle connection out of the pool (lock held), to close or lend
    std::shared_ptr<client_connection> remove(idle_list::iterator entry) {
        auto& entries = hosts[entry->key].idle;
        entries.erase(std::find(entries.begin(), entries.end(), entry));
        auto connection = std::move(entry->connection);
        idle.erase(entry);
        return connection;
    }

    // Forget a key with nothing idle nor lent (lock held)
    void prune(const std::string& key) {
        auto it = hosts.find(key);
        if (it != hosts.end() && it->second.idle.empty() && it->second.active == 0) hosts.erase(it);
    }

    // Take the expired idle connections out, to close (lock held)
    void sweep(std::chrono::steady_clock::time_point now, std::vector<std::shared_ptr<client_connection>>& closing) {
        for (auto it = idle.begin(); it != idle.end();) {
            auto entry = it++;
            if (entry->expires > now) continue;
            auto key = entry->key;
            closing.push_back(remove(entry));
            prune(key);
        }
    }

    // Close the oldest idle connections beyond the limits (lock held)
    void trim(std::vector<std::shared_ptr<client_connection>>& closing) {
        for (auto& [key, entry] : hosts) {
            while (entry.idle.size() > limits.max_idle_per_host) closing.push_back(remove(entry.idle.front()));
        }
        while (idle.size() > limits.max_idle) closing.push_back(remove(idle.begin()));
        for (auto it = hosts.begin(); it != hosts.end();) {
            it = it->second.idle.empty() && it->second.active == 0 ? hosts.erase(it) : std::next(it);
        }
    }

    void release(const std::string& key, std::shared_ptr<client_connection> connection) {
        // (the connection is not shared with another request now)
        bool reusable = connection->reusable();
        auto announced = connection->keep_alive_timeout();
        bool keep = reusable;
        std::vector<std::shared_ptr<client_connection>> closing;
        {
            std::lock_guard lock(mutex);
            auto it = hosts.find(key);
            if (it == hosts.end()) {
                keep = false;
            } else {
                if (it->second.active > 0) --it->second.active;

                // The server closes the connection once idle for the time it announced: it is
                // not reused near that time (as undici does), nor at all if it is too short
                auto timeout = limits.idle_timeout;
                if (announced) {
                    timeout = std::min(timeout, std::chrono::duration_cast<std::chrono::milliseconds>(*announced) -
                                                    keep_alive_margin);
                }
                keep = keep && !closed && timeout.count() > 0 && limits.max_idle_per_host > 0 && limits.max_idle > 0;

                if (keep) {
                    // Room for it, closing the oldest idle ones: the most likely to be closed
                    // by their servers soon
                    auto& entry = it->second;
                    while (entry.idle.size() >= limits.max_idle_per_host) closing.push_back(remove(entry.idle.front()));
                    while (idle.size() >= limits.max_idle) {
                        auto oldest = idle.front().key;
                        closing.push_back(remove(idle.begin()));
                        if (oldest != key) prune(oldest);
                    }

                    auto id = ++next_id;
                    connection->unclaim();
                    if (timers) {
                        connection->expire_after(timeout, [pool = weak_from_this(), key, id]() {
                            if (auto self = pool.lock()) self->expire(key, id);
                        });
                    }
                    idle.push_back(idle_entry{key, connection, std::chrono::steady_clock::now() + timeout, id});
                    entry.idle.push_back(std::prev(idle.end()));
                } else {
                    prune(key);
                }
            }
        }
        // Not reusable, it is already closed, or belongs to a new protocol (a 101 response)
        if (reusable && !keep) connection->close();
        for (auto& stale : closing) stale->close();
    }

    // The idle timeout of a connection elapsed (on its io_context)
    void expire(const std::string& key, uint64_t id) {
        std::shared_ptr<client_connection> connection;
        {
            std::lock_guard lock(mutex);
            auto it = hosts.find(key);
            if (it == hosts.end()) return;
            auto& entries = it->second.idle;
            auto entry = std::find_if(entries.begin(), entries.end(), [id](auto e) { return e->id == id; });
            // (lent again, or closed, meanwhile)
            if (entry == entries.end()) return;
            connection = remove(*entry);
            prune(key);
        }
        LOG_DEBUG("closing a connection idle for too long ({})", key);
        connection->close();
    }
};

connection_pool::lease& connection_pool::lease::operator=(lease&& other) noexcept {
    if (this != &other) {
        release();
        pool_ = std::move(other.pool_);
        key_ = std::move(other.key_);
        connection_ = std::move(other.connection_);
    }
    return *this;
}

void connection_pool::lease::release() {
    if (!connection_) return;
    auto connection = std::move(connection_);
    connection_.reset();
    if (auto pool = pool_.lock()) {
        pool->release(key_, std::move(connection));
    } else if (connection->reusable()) {
        connection->close();
    }
}

connection_pool::connection_pool() : state_(std::make_shared<state>()) {
}

connection_pool::~connection_pool() {
    {
        std::lock_guard lock(state_->mutex);
        state_->closed = true;
    }
    // Nothing can reach the idle connections anymore. Those lent are closed by their
    // requests: they are not kept once given back
    clear();
}

void connection_pool::configure(const config& config) {
    std::vector<std::shared_ptr<client_connection>> closing;
    {
        std::lock_guard lock(state_->mutex);
        state_->limits = config;
        state_->trim(closing);
    }
    for (auto& connection : closing) connection->close();
}

void connection_pool::use_idle_timers(bool enabled) {
    std::lock_guard lock(state_->mutex);
    state_->timers = enabled;
}

connection_pool::config connection_pool::get_config() const {
    std::lock_guard lock(state_->mutex);
    return state_->limits;
}

connection_pool::lease connection_pool::acquire(const std::string& key) {
    std::shared_ptr<client_connection> found;
    std::vector<std::shared_ptr<client_connection>> closing;
    {
        std::lock_guard lock(state_->mutex);
        // (expired ones of any key, in case their timers did not run)
        state_->sweep(std::chrono::steady_clock::now(), closing);
        if (auto it = state_->hosts.find(key); it != state_->hosts.end()) {
            auto& entry = it->second;
            // The most recently used first, so the others expire if not needed. A server may
            // close an idle connection at any time: one it closed (or wrote to) is not used
            while (!entry.idle.empty()) {
                auto connection = state_->remove(entry.idle.back());
                if (!connection->stale() && connection->claim()) {
                    found = std::move(connection);
                    break;
                }
                closing.push_back(std::move(connection));
            }
            if (found) {
                ++entry.active;
                ++state_->reused;
            } else {
                state_->prune(key);
            }
        }
    }
    if (!closing.empty()) LOG_DEBUG("closing {} idle connections closed or expired ({})", closing.size(), key);
    for (auto& connection : closing) connection->close();
    if (!found) return {};
    return lease(state_, key, std::move(found));
}

connection_pool::lease connection_pool::add(const std::string& key, const std::string& origin,
                                            std::shared_ptr<client_connection> connection) {
    {
        std::lock_guard lock(state_->mutex);
        auto& entry = state_->hosts[key];
        if (entry.origin.empty()) entry.origin = origin;
        ++entry.active;
        ++state_->created;
    }
    connection->claim();
    return lease(state_, key, std::move(connection));
}

size_t connection_pool::size() const {
    std::lock_guard lock(state_->mutex);
    return state_->idle.size();
}

connection_pool::stats connection_pool::get_stats() const {
    std::lock_guard lock(state_->mutex);
    stats result;
    result.created = state_->created;
    result.reused = state_->reused;
    for (const auto& [key, entry] : state_->hosts) {
        auto& host = result.hosts[entry.origin];
        host.idle += entry.idle.size();
        host.active += entry.active;
        result.idle += entry.idle.size();
        result.active += entry.active;
    }
    return result;
}

void connection_pool::clear() {
    std::vector<std::shared_ptr<client_connection>> closing;
    {
        std::lock_guard lock(state_->mutex);
        for (auto& entry : state_->idle) closing.push_back(std::move(entry.connection));
        state_->idle.clear();
        for (auto it = state_->hosts.begin(); it != state_->hosts.end();) {
            it->second.idle.clear();
            it = it->second.active == 0 ? state_->hosts.erase(it) : std::next(it);
        }
    }
    for (auto& connection : closing) connection->close();
}

} // namespace thinger::http

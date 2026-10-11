#include "connection_tracker.hpp"
#include "../../util/logger.hpp"
#include <boost/asio/post.hpp>

namespace thinger::http {

// tracked_connection

tracked_connection::~tracked_connection() {
    untrack();
}

void tracked_connection::track(std::shared_ptr<connection_tracker> tracker, std::weak_ptr<void> self,
                               boost::asio::io_context& io_context, connection_type type) {
    tracker_ = std::move(tracker);
    self_ = std::move(self);
    io_context_ = &io_context;
    type_ = type;
    tracker_->add(*this);
}

void tracked_connection::untrack() {
    if (!tracker_) return;
    tracker_->remove(*this);
    tracker_.reset();
}

void tracked_connection::set_type(connection_type type) {
    if (tracker_) tracker_->retype(*this, type);
    else type_ = type;
}

// connection_tracker

size_t connection_tracker::shard_of(const boost::asio::io_context& io_context) {
    // The connections of an io_context share a shard, so it is not contended by the others
    auto address = reinterpret_cast<uintptr_t>(&io_context);
    return ((address >> 6) * 0x9E3779B97F4A7C15ull) >> 60;
}

void connection_tracker::add(tracked_connection& connection) {
    connection.shard_ = static_cast<uint8_t>(shard_of(*connection.io_context_));
    auto& shard = shards_[connection.shard_];
    std::scoped_lock lock(shard.mutex);
    connection.next_ = shard.head;
    if (shard.head) shard.head->prev_ = &connection;
    shard.head = &connection;
    connection.linked_ = true;
    ++shard.active[static_cast<size_t>(connection.type_)];
    if (connection.type_ == connection_type::http) ++shard.total_connections;
}

void connection_tracker::remove(tracked_connection& connection) {
    auto& shard = shards_[connection.shard_];
    std::scoped_lock lock(shard.mutex);
    if (!connection.linked_) return;
    if (connection.prev_) connection.prev_->next_ = connection.next_;
    else shard.head = connection.next_;
    if (connection.next_) connection.next_->prev_ = connection.prev_;
    connection.prev_ = connection.next_ = nullptr;
    connection.linked_ = false;
    --shard.active[static_cast<size_t>(connection.type_)];
}

void connection_tracker::retype(tracked_connection& connection, connection_type type) {
    auto& shard = shards_[connection.shard_];
    std::scoped_lock lock(shard.mutex);
    if (connection.linked_) {
        --shard.active[static_cast<size_t>(connection.type_)];
        ++shard.active[static_cast<size_t>(type)];
    }
    connection.type_ = type;
}

server_stats connection_tracker::stats() const {
    server_stats stats;
    for (const auto& shard : shards_) {
        std::scoped_lock lock(shard.mutex);
        stats.http_connections += shard.active[static_cast<size_t>(connection_type::http)];
        stats.websocket_connections += shard.active[static_cast<size_t>(connection_type::websocket)];
        stats.sse_connections += shard.active[static_cast<size_t>(connection_type::sse)];
        stats.tunnel_connections += shard.active[static_cast<size_t>(connection_type::tunnel)];
        stats.taken_over_connections += shard.active[static_cast<size_t>(connection_type::taken_over)];
        stats.total_connections += shard.total_connections;
    }
    return stats;
}

size_t connection_tracker::active() const {
    size_t active = 0;
    for (const auto& shard : shards_) {
        std::scoped_lock lock(shard.mutex);
        for (auto count : shard.active) active += count;
    }
    return active;
}

std::vector<std::pair<std::shared_ptr<void>, tracked_connection*>> connection_tracker::snapshot() const {
    std::vector<std::pair<std::shared_ptr<void>, tracked_connection*>> connections;
    for (const auto& shard : shards_) {
        std::scoped_lock lock(shard.mutex);
        for (auto* connection = shard.head; connection; connection = connection->next_) {
            // (one being destroyed waits for the lock to leave: it cannot be locked anymore)
            if (auto owner = connection->self_.lock()) connections.emplace_back(std::move(owner), connection);
        }
    }
    return connections;
}

void connection_tracker::drain_all() {
    for (auto& [owner, connection] : snapshot()) {
        boost::asio::post(*connection->io_context_, [owner = std::move(owner), connection = connection] {
            connection->shutdown_drain();
        });
    }
}

void connection_tracker::close_all(std::function<void()> done) {
    auto connections = snapshot();
    if (connections.empty()) {
        if (done) done();
        return;
    }
    struct pending_closes {
        std::atomic<size_t> left;
        std::function<void()> done;
    };
    auto pending = std::make_shared<pending_closes>(connections.size(), std::move(done));
    for (auto& [owner, connection] : connections) {
        boost::asio::post(*connection->io_context_, [owner = std::move(owner), connection = connection, pending] {
            connection->shutdown_close();
            if (pending->left.fetch_sub(1, std::memory_order_acq_rel) == 1 && pending->done) pending->done();
        });
    }
}

// taken_over_connection

std::shared_ptr<taken_over_connection> taken_over_connection::track(std::shared_ptr<connection_tracker> tracker,
                                                                    const std::shared_ptr<asio::socket>& socket) {
    std::shared_ptr<taken_over_connection> connection(new taken_over_connection());
    connection->socket_ = socket;
    connection->tracked_connection::track(std::move(tracker), connection, socket->get_io_context(),
                                          connection_type::taken_over);
    socket->set_attachment(connection);
    return connection;
}

std::shared_ptr<taken_over_connection> taken_over_connection::of(const asio::socket& socket) {
    return std::dynamic_pointer_cast<taken_over_connection>(socket.get_attachment());
}

taken_over_connection::~taken_over_connection() = default;

void taken_over_connection::handed_over() {
    if (!tracked()) return;
    switch (tracker()->current_phase()) {
        case connection_tracker::phase::serving: break;
        case connection_tracker::phase::draining: shutdown_drain(); break;
        case connection_tracker::phase::closing: shutdown_close(); break;
    }
}

void taken_over_connection::shutdown_drain() {
    if (notified_ || !tracked()) return;
    notified_ = true;
    // Called once: released before, as it may own the socket (which owns this)
    auto callback = std::move(on_shutdown_);
    on_shutdown_ = nullptr;
    if (!callback) return;
    try {
        callback();
    } catch (const std::exception& e) {
        LOG_ERROR("error notifying a connection of the server shutdown: {}", e.what());
    }
}

void taken_over_connection::shutdown_close() {
    if (!tracked()) return;
    on_shutdown_ = nullptr;
    if (auto socket = socket_.lock()) socket->close();
    untrack();
}

void set_connection_type(const asio::socket& socket, connection_type type) {
    if (auto connection = taken_over_connection::of(socket)) connection->set_type(type);
}

void wrap_connection(const asio::socket& socket, asio::socket& wrapper, connection_type type) {
    auto connection = taken_over_connection::of(socket);
    if (!connection) return;
    connection->set_type(type);
    wrapper.set_attachment(std::move(connection));
}

void on_server_shutdown(asio::socket& socket, std::function<void()> callback) {
    if (auto connection = taken_over_connection::of(socket)) connection->on_shutdown(std::move(callback));
}

}

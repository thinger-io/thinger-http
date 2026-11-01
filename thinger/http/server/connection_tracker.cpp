#include "connection_tracker.hpp"
#include "../../util/logger.hpp"
#include <boost/asio/post.hpp>
#include <utility>

namespace thinger::http {

// tracked_connection

tracked_connection::~tracked_connection() {
    untrack();
}

void tracked_connection::track(std::shared_ptr<connection_tracker> tracker, std::weak_ptr<void> self,
                               boost::asio::io_context& io_context, connection_type type) {
    tracker_ = tracker;
    self_ = std::move(self);
    io_context_ = &io_context;
    type_ = type;
    tracker->add(*this);
}

void tracked_connection::untrack() {
    // (a tracker gone no longer lists it)
    auto tracker = tracker_.lock();
    tracker_.reset();
    if (tracker) tracker->remove(*this);
}

void tracked_connection::set_type(connection_type type) {
    if (auto tracker = tracker_.lock()) tracker->retype(*this, type);
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
    {
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
    // Only while a shutdown waits for the last one. Set before notify_when_empty() counted the
    // connections: if that count saw this one, this sees it set (the lock of the shard orders
    // them), and the last one to leave tells it
    if (watching_.load(std::memory_order_acquire)) [[unlikely]] check_empty();
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

void connection_tracker::notify_when_empty(std::function<void()> empty) {
    {
        std::scoped_lock lock(empty_mutex_);
        on_empty_ = std::move(empty);
        watching_.store(static_cast<bool>(on_empty_), std::memory_order_release);
    }
    // None left already (or the last left before it was set)
    check_empty();
}

void connection_tracker::check_empty() {
    std::function<void()> empty;
    {
        // (held while counting: told once, by whoever counts none first)
        std::scoped_lock lock(empty_mutex_);
        if (!on_empty_ || active() > 0) return;
        empty = std::exchange(on_empty_, nullptr);
        watching_.store(false, std::memory_order_release);
    }
    empty();
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

void connection_tracker::cancel_and_close_all(std::function<void()> done) {
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
        auto* io_context = connection->io_context_;
        boost::asio::post(*io_context, [io_context, owner = std::move(owner), connection = connection, pending]() mutable {
            // In one call: the handlers interrupted resume once closed. Posted apart, the
            // first could run, and a handler resume and write, before the second was posted
            connection->shutdown_cancel();
            connection->shutdown_close();
            // Closed once what was posted before the close ran too: the handlers interrupted
            // resume (on a closed connection) before the server may stop
            boost::asio::post(*io_context, [owner = std::move(owner), pending] {
                if (pending->left.fetch_sub(1, std::memory_order_acq_rel) == 1 && pending->done) pending->done();
            });
        });
    }
}

// taken_over_tracking

namespace detail {

std::shared_ptr<taken_over_tracking> taken_over_tracking::track(std::shared_ptr<connection_tracker> tracker,
                                                                std::shared_ptr<asio::socket> socket) {
    auto& io_context = socket->get_io_context();
    std::shared_ptr<taken_over_tracking> tracking(new taken_over_tracking(std::move(socket)));
    tracking->tracked_connection::track(std::move(tracker), tracking, io_context, connection_type::taken_over);
    return tracking;
}

taken_over_tracking::~taken_over_tracking() = default;

upgraded_connection taken_over_tracking::hand_out(std::string buffered) {
    // The socket handed out shares the ownership of this tracking: the connection counts while
    // the new owner keeps it, and the socket never knows of the server
    auto self = shared_from_this();
    return upgraded_connection{std::shared_ptr<asio::socket>(self, socket_.get()), std::move(buffered),
                               server_scope(self)};
}

void taken_over_tracking::handed_over() {
    auto tracker = this->tracker();
    if (!tracker) return;
    switch (tracker->current_phase()) {
        case connection_tracker::phase::serving: break;
        case connection_tracker::phase::draining: shutdown_drain(); break;
        case connection_tracker::phase::closing: shutdown_close(); break;
    }
}

void taken_over_tracking::on_shutdown(sync_callback<void()> callback) {
    // Closed, or told already: never told (again)
    if (!tracked() || notified_) return;
    if (!draining_) {
        on_shutdown_ = std::move(callback);
        return;
    }
    // Set once the drain started (e.g. by a coroutine handler, after the takeover): told now,
    // posted, unless closed meanwhile. Not kept here, as it may own the socket
    notified_ = true;
    boost::asio::post(socket_->get_io_context(), [weak = weak_from_this(), callback = std::move(callback)] {
        auto self = weak.lock();
        if (self && self->tracked()) notify(callback);
    });
}

void taken_over_tracking::closed() {
    on_shutdown_ = nullptr;
    untrack();
}

void taken_over_tracking::notify(const sync_callback<void()>& callback) {
    try {
        callback();
    } catch (const std::exception& e) {
        LOG_ERROR("error notifying a connection of the server shutdown: {}", e.what());
    }
}

void taken_over_tracking::shutdown_drain() {
    if (draining_ || !tracked()) return;
    draining_ = true;
    // Called once: released before, as it may own the socket (which owns this). Without one
    // yet, the owner is told once it sets it (on_shutdown())
    auto callback = std::move(on_shutdown_);
    on_shutdown_ = nullptr;
    if (!callback) return;
    notified_ = true;
    notify(callback);
}

void taken_over_tracking::shutdown_close() {
    if (!tracked()) return;
    on_shutdown_ = nullptr;
    socket_->close();
    untrack();
}

}

// server_scope

void server_scope::set_type(connection_type type) const {
    if (auto tracking = tracking_.lock()) tracking->set_type(type);
}

void server_scope::on_shutdown(sync_callback<void()> callback) const {
    if (auto tracking = tracking_.lock()) tracking->on_shutdown(std::move(callback));
}

void server_scope::closed() const {
    if (auto tracking = tracking_.lock()) tracking->closed();
}

}

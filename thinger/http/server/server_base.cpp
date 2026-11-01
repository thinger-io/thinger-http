#include "server_base.hpp"
#include "server_connection.hpp"
#include "request.hpp"
#include "../../util/logger.hpp"
#include <boost/asio/error.hpp>
#include <algorithm>
#include <variant>

namespace thinger::http {

namespace {

using listen_endpoint = decltype(asio::listener_options::endpoint);

// Whether a listener opened for `opened` (listening on `port`) listens where `wanted` asks to:
// the same Unix socket, or the same host and port (a port 0 asks for a new one)
bool same_endpoint(const listen_endpoint& opened, uint16_t port, const listen_endpoint& wanted) {
    if (opened.index() != wanted.index()) return false;
    if (auto* local = std::get_if<asio::unix_endpoint>(&wanted)) {
        return std::get<asio::unix_endpoint>(opened).path == local->path;
    }
    const auto& tcp = std::get<asio::tcp_endpoint>(wanted);
    return tcp.port != 0 && tcp.port == port && std::get<asio::tcp_endpoint>(opened).host == tcp.host;
}

}

void server_base::set_connection_options(connection_options options) {
    auto shared = std::make_shared<const connection_options>(std::move(options));
    std::scoped_lock lock(listeners_mutex_);
    options_ = std::move(shared);
}

boost::system::error_code server_base::listen(asio::listener_options options) {
    std::shared_ptr<asio::listener> listener;
    {
        // Reserved before it opens (out of the locks): what stops the listeners from now on, a
        // shutdown starting too, stops it, and another listen() on its endpoint is refused
        std::scoped_lock shutdown_lock(shutdown_mutex_);
        std::scoped_lock lock(listeners_mutex_);
        for (const auto& open : listeners_) {
            if (!same_endpoint(open.endpoint, open.port, options.endpoint)) continue;
            if (open.url.empty()) LOG_ERROR("cannot listen: another listen() is opening that endpoint");
            else LOG_ERROR("cannot listen on {}: the server listens there already", open.url);
            return boost::asio::error::address_in_use;
        }
        if (!reset_shutdown()) {
            LOG_ERROR("cannot listen: the server is shutting down");
            return boost::asio::error::shut_down;
        }
        auto endpoint = options.endpoint;
        auto* tcp = std::get_if<asio::tcp_endpoint>(&endpoint);
        uint16_t port = tcp ? tcp->port : 0;
        listener = create_listener(std::move(options));
        listeners_.push_back({listener, std::move(endpoint), port, {}});
    }
    // (why it cannot is logged by the listener; operation_aborted if stopped meanwhile)
    if (auto ec = listener->open()) {
        std::scoped_lock lock(listeners_mutex_);
        std::erase_if(listeners_, [&](const open_listener& open) { return open.listener == listener; });
        return ec;
    }
    auto url = listener->url();
    {
        std::scoped_lock lock(listeners_mutex_);
        auto open = std::ranges::find(listeners_, listener, &open_listener::listener);
        // Taken by stop_listening() (from stop() or a shutdown too) once open: stopped by it
        if (open == listeners_.end()) return boost::asio::error::operation_aborted;
        open->port = listener->local_port();
        open->url = url;
        // Under the lock, as it only starts accepting (on the server's threads): whatever stops
        // the listeners takes it once it accepts
        listener->start([this](std::shared_ptr<asio::socket> socket) { serve(std::move(socket)); });
    }
    LOG_INFO("listening on {}", url);
    listening();
    return {};
}

boost::system::error_code server_base::listen(std::string host, uint16_t port) {
    asio::listener_options options;
    options.endpoint = asio::tcp_endpoint{std::move(host), port};
    return listen(std::move(options));
}

bool server_base::start(uint16_t port) {
    return start("0.0.0.0", port);
}

bool server_base::start(uint16_t port, const sync_callback<void()> &on_listening) {
    return start("0.0.0.0", port, on_listening);
}

bool server_base::start(const std::string& host, uint16_t port) {
    return start(host, port, nullptr);
}

bool server_base::start(const std::string& host, uint16_t port, const sync_callback<void()> &on_listening) {
    if (listen(host, port)) {
        return false;
    }
    if (on_listening) {
        on_listening();
    }
    wait();
    return true;
}

void server_base::stop() {
    end_shutdown();
    // Not logged when a shutdown stops it: it stopped listening already, and the shutdown tells
    auto name = is_listening() ? log_name() : std::string();
    // (a listen() in progress too)
    stop_listening();
    // The connections still open are closed (those accepted meanwhile too, see
    // server_connection::start()), and the handlers still running cancelled: none is served
    // again, even if the server listens again. Not waited for: the closes keep the tracker, so
    // they close the connections once the server is gone too
    connections_->set_phase(connection_tracker::phase::closing);
    connections_->cancel_and_close_all([tracker = connections_] {});
    if (!name.empty()) LOG_INFO("{} stopped", name);
}

void server_base::stop_listening() {
    std::vector<open_listener> listeners;
    {
        std::scoped_lock lock(listeners_mutex_);
        listeners.swap(listeners_);
    }
    // Out of the lock: stopping waits for the handlers running on other threads, which serve().
    // A listener still opening gives up (see listen())
    for (auto& open : listeners) open.listener->stop();
}

bool server_base::is_listening() const {
    std::scoped_lock lock(listeners_mutex_);
    return std::ranges::any_of(listeners_, [](const open_listener& open) { return !open.url.empty(); });
}

std::vector<std::string> server_base::listening_on() const {
    std::scoped_lock lock(listeners_mutex_);
    std::vector<std::string> urls;
    urls.reserve(listeners_.size());
    for (const auto& open : listeners_) {
        if (!open.url.empty()) urls.push_back(open.url);
    }
    return urls;
}


// Private methods
std::string server_base::log_name() const {
    std::string name;
    for (const auto& url : listening_on()) {
        name += (name.empty() ? "server on " : ", ") + url;
    }
    return name.empty() ? "server" : name;
}

void server_base::serve(std::shared_ptr<asio::socket> socket) {
    std::shared_ptr<const connection_options> options;
    {
        std::scoped_lock lock(listeners_mutex_);
        options = options_;
    }
    auto connection = std::make_shared<server_connection>(std::move(socket), std::move(options));
    connection->track(connections_);

    // Never this server, which may be destroyed while the connection lives
    connection->set_handler(connection_handler());

    connection->start();
}

} // namespace thinger::http

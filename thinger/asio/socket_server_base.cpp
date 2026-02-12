#include "socket_server_base.hpp"

namespace thinger::asio {

socket_server_base::socket_server_base(io_context_provider acceptor_context_provider,
                                     io_context_provider connection_context_provider,
                                     std::set<std::string> allowed_remotes, 
                                     std::set<std::string> forbidden_remotes)
    : allowed_remotes_(std::move(allowed_remotes))
    , forbidden_remotes_(std::move(forbidden_remotes))
    , max_listening_attempts_(MAX_LISTENING_ATTEMPTS)
    , acceptor_context_provider_(std::move(acceptor_context_provider))
    , connection_context_provider_(std::move(connection_context_provider))
{
}

void socket_server_base::set_max_listening_attempts(int attempts) {
    max_listening_attempts_ = attempts;
}

void socket_server_base::set_handler(std::function<void(std::shared_ptr<socket>)> handler) {
    handler_ = std::move(handler);
}

void socket_server_base::set_allowed_remotes(std::set<std::string> allowed) {
    allowed_remotes_ = std::move(allowed);
}

void socket_server_base::set_forbidden_remotes(std::set<std::string> forbidden) {
    forbidden_remotes_ = std::move(forbidden);
}

bool socket_server_base::is_remote_allowed(const std::string& remote_ip) const {
    if (forbidden_remotes_.contains(remote_ip)) {
        return false;
    }
    if (!allowed_remotes_.empty() && !allowed_remotes_.contains(remote_ip)) {
        return false;
    }
    return true;
}

bool socket_server_base::start() {
    if (!running_ && handler_) {
        if (create_acceptor()) {
            running_ = true;
            accept_connection();
            return true;
        }
    }
    return false;
}

bool socket_server_base::stop() {
    running_ = false;
    return true;
}

} // namespace thinger::asio
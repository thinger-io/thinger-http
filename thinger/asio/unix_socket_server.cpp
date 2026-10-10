#include "unix_socket_server.hpp"
#include "workers.hpp"
#include "../util/logger.hpp"
#include <filesystem>

namespace thinger::asio {

// Constructor with io_context providers
unix_socket_server::unix_socket_server(std::string unix_path,
                                     io_context_provider acceptor_context_provider,
                                     io_context_provider connection_context_provider,
                                     std::set<std::string> allowed_remotes,
                                     std::set<std::string> forbidden_remotes)
    : basic_socket_server(std::move(acceptor_context_provider),
                         std::move(connection_context_provider),
                         std::move(allowed_remotes),
                         std::move(forbidden_remotes))
    , unix_path_(std::move(unix_path))
{
}

// Legacy constructor for backward compatibility
unix_socket_server::unix_socket_server(std::string unix_path,
                                     std::set<std::string> allowed_remotes,
                                     std::set<std::string> forbidden_remotes)
    : unix_socket_server(unix_path,
                        []() -> boost::asio::io_context& { return get_workers().get_thread_io_context(); },
                        []() -> boost::asio::io_context& { return get_workers().get_next_io_context(); },
                        std::move(allowed_remotes),
                        std::move(forbidden_remotes))
{
    use_acceptor_thread();
}

unix_socket_server::~unix_socket_server() {
    // Before the acceptor thread could use what is destroyed here
    stop();
}

bool unix_socket_server::stop() {
    // Stop accepting connections and close the acceptor
    basic_socket_server::stop();

    // Remove the socket file when server is stopped
    if (!unix_path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(unix_path_, ec);
        if (ec) {
            LOG_WARNING("Failed to remove Unix socket file {}: {}", unix_path_, ec.message());
        }
    }

    return true;
}

std::string unix_socket_server::get_service_name() const {
    return "unix_server@" + unix_path_;
}

std::optional<boost::asio::local::stream_protocol::endpoint> unix_socket_server::listening_endpoint(boost::asio::io_context&) {
    // Remove existing socket file if it exists
    std::error_code ec;
    std::filesystem::remove(unix_path_, ec);
    return boost::asio::local::stream_protocol::endpoint(unix_path_);
}

void unix_socket_server::on_listening() {
    LOG_INFO("Unix socket server is now listening on {}", unix_path_);

    // Set permissions to allow access (you might want to make this configurable)
    std::filesystem::permissions(unix_path_,
                               std::filesystem::perms::owner_all |
                               std::filesystem::perms::group_read |
                               std::filesystem::perms::group_write,
                               std::filesystem::perm_options::replace);
}

unix_socket_server::connection_server unix_socket_server::make_connection_server() const {
    // Connections are not filtered: they have no remote address
    return [handler = handler_, unix_path = unix_path_](boost::asio::local::stream_protocol::socket peer) {
        auto sock = std::make_shared<unix_socket>("unix_socket_server", std::move(peer));

        LOG_INFO("received connection on Unix socket: {}", unix_path);

        // Call handler with the connected socket
        if (handler) {
            handler(std::move(sock));
        }
    };
}

} // namespace thinger::asio

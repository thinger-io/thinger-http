// Protocol upgrades (WebSocket...) through the reverse proxy: a tunnel between the client and
// the target
#include "proxy_exchange.hpp"
#include "../connection_tracker.hpp"
#include "../../../asio/socket_pipe.hpp"
#include "../../../asio/sockets/websocket.hpp"
#include "../../../asio/with_timeout.hpp"

namespace thinger::http::proxy_detail {

// A quiet WebSocket behind the proxy, with the server's default keepalive, pings before the
// default tunnel deadline cuts it (see websocket_connection, upgrade.hpp's ping_interval)
static_assert(proxy_options::DEFAULT_TUNNEL_IDLE_TIMEOUT > asio::websocket::CONNECTION_TIMEOUT_SECONDS,
              "the default tunnel idle timeout must exceed the default WebSocket ping interval");

namespace {

// Write what a side sent right after the handshake, within the idle deadline, if any: a
// peer that does not read it ends the tunnel as the pipe would
awaitable<bool> write_early(asio::socket& to, const std::string& data, std::chrono::milliseconds idle_timeout) {
    if (data.empty()) co_return true;
    if (idle_timeout.count() <= 0) {
        auto [ec, bytes] = co_await to.write(data);
        co_return !ec;
    }
    auto result = co_await asio::with_timeout(to.write(data), idle_timeout);
    co_return result && !std::get<0>(*result);
}

}

awaitable<void> run_tunnel(std::shared_ptr<asio::socket> client, std::shared_ptr<asio::socket> target,
                           std::string to_client, std::string to_target,
                           std::chrono::milliseconds idle_timeout) {
    // Bytes each side sent right after the handshake go first
    if (!co_await write_early(*client, to_client, idle_timeout) ||
        !co_await write_early(*target, to_target, idle_timeout)) {
        client->close();
        target->close();
        co_return;
    }
    auto pipe = std::make_shared<asio::socket_pipe>(client, target);
    pipe->set_idle_timeout(idle_timeout);
    co_await pipe->run();
}

void proxy_exchange::upgrade() {
    const auto& protocol = head_->get_header(header::upgrade);
    if (!upgrade_requested_ || protocol.empty()) {
        fail(http_response::status::bad_gateway, "Invalid response from the target", "unexpected protocol switch");
        return;
    }

    http_response out;
    out.set_status(http_response::status::switching_protocols);
    copy_end_to_end(*head_, out);
    if (options_.on_response) options_.on_response(req_, out);
    remove_framing_headers(out);
    if (!has_valid_headers(out)) {
        fail(http_response::status::bad_gateway, "Invalid response from the target", "invalid response header");
        return;
    }

    res_.status(http_response::status::switching_protocols);
    for (const auto& [name, value] : out.get_headers()) res_.header(name, value);
    res_.header(header::upgrade, protocol);
    res_.header(header::connection, "Upgrade");

    // Once the 101 is written, the client connection and the target become a tunnel,
    // starting with what each one sent past its head
    auto target = std::exchange(target_socket_, nullptr);
    res_.take_over([target, early = std::exchange(pending_, {}),
                    idle_timeout = options_.tunnel_idle_timeout](upgraded_connection client) {
        client.scope.set_type(connection_type::tunnel);
        auto& io_context = client.socket->get_io_context();
        co_spawn(io_context, run_tunnel(std::move(client.socket), target, early, std::move(client.buffered),
                                        idle_timeout), detached);
    });
    result_.status = 101;
    result_.from_target = true;
    result_.upgraded = true;
    result_.complete = true;
}

}

// Protocol upgrades (WebSocket...) through the reverse proxy: a tunnel between the client and
// the target
#include "proxy_exchange.hpp"
#include "../connection_tracker.hpp"
#include "../../../asio/socket_pipe.hpp"

namespace thinger::http::proxy_detail {

awaitable<void> run_tunnel(std::shared_ptr<asio::socket> client, std::shared_ptr<asio::socket> target,
                           std::string to_client, std::string to_target) {
    // Bytes each side sent right after the handshake go first
    bool ok = true;
    if (!to_client.empty()) {
        auto [ec, bytes] = co_await client->write(to_client);
        ok = !ec;
    }
    if (ok && !to_target.empty()) {
        auto [ec, bytes] = co_await target->write(to_target);
        ok = !ec;
    }
    if (!ok) {
        client->close();
        target->close();
        co_return;
    }
    auto pipe = std::make_shared<asio::socket_pipe>(client, target);
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
    res_.take_over([target, early = std::exchange(pending_, {})](std::shared_ptr<asio::socket> client, std::string buffered) {
        set_connection_type(*client, connection_type::tunnel);
        co_spawn(client->get_io_context(), run_tunnel(client, target, early, std::move(buffered)), detached);
    });
    result_.status = 101;
    result_.from_target = true;
    result_.upgraded = true;
    result_.complete = true;
}

}

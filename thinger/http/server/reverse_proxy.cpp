// The reverse proxy, and the course of an exchange: the connection to the target, and the
// timeouts. The request, its body, the response and the tunnels are in reverse_proxy/.
#include "reverse_proxy/proxy_exchange.hpp"
#include "../../asio/sockets/ssl_socket.hpp"
#include "../../asio/sockets/tcp_socket.hpp"
#include "../../asio/sockets/unix_socket.hpp"
#include "../../util/logger.hpp"
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/strand.hpp>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <algorithm>

namespace thinger::http {

namespace proxy_detail {

namespace {

    // The io_context of an executor (of one of its executor or a strand of it), or null
    boost::asio::io_context* io_context_of(const boost::asio::any_io_executor& executor) {
        using untracked = boost::asio::io_context::basic_executor_type<std::allocator<void>, 0>;
        using tracked = boost::asio::io_context::basic_executor_type<std::allocator<void>, 4>;
        if (auto* inner = executor.target<untracked>()) return &inner->context();
        if (auto* inner = executor.target<tracked>()) return &inner->context();
        if (auto* strand = executor.target<boost::asio::strand<boost::asio::any_io_executor>>()) {
            return io_context_of(strand->get_inner_executor());
        }
        if (auto* strand = executor.target<boost::asio::strand<untracked>>()) {
            return &strand->get_inner_executor().context();
        }
        return nullptr;
    }

}

boost::asio::io_context* proxy_exchange::io_context() {
    if (auto socket = req_.get_socket()) return &socket->get_io_context();
    // A request dispatched in memory: the context running the handler, if it is an
    // io_context (null otherwise: the sockets need one)
    return io_context_of(executor_);
}

std::string proxy_exchange::describe() const {
    std::string where;
    switch (target_.type) {
        case proxy_target::transport::unix_socket:
            where = "unix:" + target_.socket_path;
            break;
        case proxy_target::transport::tls:
            where = "https://" + target_.authority();
            break;
        default:
            where = "http://" + target_.authority();
            break;
    }
    return get_method(incoming_->get_method()) + " " + incoming_->get_uri() + " -> " + where;
}

void proxy_exchange::fail(http_response::status status, const std::string& message, const std::string& detail) {
    // Once stopped (timeout, client gone, invalid request body), what the exchange waits
    // for fails as a consequence: that is answered by whoever stopped it
    if (stopped_ != stop_reason::none) return;
    LOG_WARNING("reverse proxy {}: {}", describe(), detail);
    if (result_.error.empty()) result_.error = detail;
    result_.complete = false;
    if (response_started_) {
        res_.abort_stream();
        return;
    }
    if (res_.has_responded()) return;
    res_.error(status, message);
    result_.status = static_cast<int>(status);
    result_.from_target = false;
}

void proxy_exchange::close_target() {
    if (target_socket_) target_socket_->close();
}

// --- Connection ---

awaitable<boost::system::error_code> proxy_exchange::connect_target() {
    auto& io = *io_context();
    // The connect timeout is enforced by connect(): this one only bounds the socket
    auto limit = std::chrono::ceil<std::chrono::seconds>(options_.connect_timeout) + std::chrono::seconds(1);

    if (target_.type == proxy_target::transport::unix_socket) {
        auto socket = std::make_shared<asio::unix_socket>("reverse_proxy", io);
        target_socket_ = socket;
        co_return co_await socket->connect(target_.socket_path, limit);
    }

    auto tcp = std::make_shared<asio::tcp_socket>("reverse_proxy", io);
    target_socket_ = tcp;
    auto ec = co_await tcp->connect(target_.host, std::to_string(target_.port), limit);
    if (ec) co_return ec;
    tcp->enable_tcp_no_delay();
    if (target_.type != proxy_target::transport::tls) co_return ec;
    co_return co_await start_tls(std::move(tcp));
}

awaitable<boost::system::error_code> proxy_exchange::start_tls(std::shared_ptr<asio::tcp_socket> tcp) {
    std::shared_ptr<boost::asio::ssl::context> context;
    try {
        context = proxy_.tls_context(target_.tls);
    } catch (const std::exception& e) {
        LOG_ERROR("reverse proxy: invalid TLS settings for {}: {}", target_.authority(), e.what());
        tls_config_error_ = true;
        co_return boost::asio::error::invalid_argument;
    }

    auto tls = std::make_shared<asio::ssl_socket>("reverse_proxy", tcp, context);
    target_socket_ = tls;
    const auto& name = target_.tls.server_name.empty() ? target_.host : target_.tls.server_name;
    if (target_.tls.verify) {
        // The certificate must be issued for the name (or address) of the target
        auto* ssl = tls->native_handle();
        int set = is_ip_address(name) ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), name.c_str())
                                      : SSL_set1_host(ssl, name.c_str());
        if (set != 1) {
            LOG_ERROR("reverse proxy: cannot verify the name {} of {}", name, target_.authority());
            tls_config_error_ = true;
            co_return boost::asio::error::invalid_argument;
        }
    }
    co_return co_await tls->handshake(name);
}

awaitable<bool> proxy_exchange::connect() {
    auto limit = options_.connect_timeout;
    if (options_.total_timeout.count() > 0) limit = std::min(limit, options_.total_timeout);

    // On timeout the socket is closed, so the connection or handshake in progress fails
    boost::asio::steady_timer timer(executor_, limit);
    auto timeout = [&]() -> awaitable<void> {
        auto [ec] = co_await timer.async_wait(use_nothrow_awaitable);
        if (!ec) close_target();
    };
    auto result = co_await (connect_target() || timeout());

    boost::system::error_code ec = boost::asio::error::timed_out;
    if (result.index() == 0) ec = std::get<0>(result);
    if (!ec) {
        touch();
        co_return true;
    }

    close_target();
    target_socket_.reset();
    if (ec == boost::asio::error::timed_out) {
        fail(http_response::status::gateway_timeout, "The target did not answer in time",
             "timeout connecting to the target");
    } else if (tls_config_error_) {
        fail(http_response::status::bad_gateway, "Cannot connect to the target", "invalid TLS settings");
    } else {
        fail(http_response::status::bad_gateway, "Cannot connect to the target",
             "cannot connect to the target: " + ec.message());
    }
    co_return false;
}

// --- Exchange ---

awaitable<void> proxy_exchange::exchange() {
    bool target_open = co_await send_head();
    if (target_open && body_mode_ != body_mode::none && !body_buffered_) {
        // The response is read while the body is sent: a target may answer before reading
        // it all (e.g. 401, 413), and stop reading it
        auto result = co_await (read_head() || stream_body());
        if (stopped_ != stop_reason::none) co_return;
        if (result.index() != 0 || !std::get<0>(result)) co_return;
        // Answered before the end of the body: what is left of it is not read
        if (!request_sent_) close_client_after_response();
    } else {
        if (target_open) request_done();
        if (!co_await read_head()) co_return;
    }
    if (status_code_ == 101) {
        upgrade();
        co_return;
    }
    co_await relay_response();
}

awaitable<void> proxy_exchange::wait_forever() {
    boost::asio::steady_timer timer(executor_, clock::time_point::max());
    co_await timer.async_wait(use_nothrow_awaitable);
}

awaitable<void> proxy_exchange::watch_client() {
    // Only once the client sent the whole request: then reading from its connection
    // tells whether it closed it, without taking anything that belongs to the request
    if (!request_sent_) {
        co_await request_sent_signal_->async_wait(use_nothrow_awaitable);
        if (!request_sent_) co_return;  // the exchange ended
    }
    auto socket = req_.get_socket();
    // Data of the next request already read: the client is there
    if (!socket || req_.read_ahead_available() > 0) {
        co_await wait_forever();
        co_return;
    }
    auto ec = co_await socket->wait(boost::asio::socket_base::wait_read);
    bool closed = ec || co_await socket->peer_closed();
    // The exchange ended meanwhile: the wait was cancelled
    auto state = co_await boost::asio::this_coro::cancellation_state;
    if (state.cancelled() != boost::asio::cancellation_type::none) co_return;
    // Not closed: the next request (pipelined), kept for the server, so the client is there.
    // Or the server closed the connection itself once the whole response was queued
    if (!closed || response_ended_) {
        co_await wait_forever();
        co_return;
    }
    // Gone: nobody would take the response (e.g. an event stream that stays silent)
    if (stopped_ == stop_reason::none) stopped_ = stop_reason::client_left;
    close_target();
}

awaitable<void> proxy_exchange::watchdog() {
    boost::asio::steady_timer timer(executor_);
    while (true) {
        auto deadline = clock::time_point::max();
        if (options_.idle_timeout.count() > 0) deadline = last_activity_ + options_.idle_timeout;
        if (options_.total_timeout.count() > 0) deadline = std::min(deadline, start_ + options_.total_timeout);
        if (clock::now() >= deadline) break;
        timer.expires_at(deadline);
        auto [ec] = co_await timer.async_wait(use_nothrow_awaitable);
        if (ec) co_return;  // the exchange ended
    }

    // Make whatever the exchange waits for fail: the target, and a client that does
    // not take the response (its connection is closed, as the response is incomplete)
    if (stopped_ == stop_reason::none) stopped_ = stop_reason::timeout;
    close_target();
    if (response_started_) {
        if (auto connection = res_.get_connection()) connection->release();
    }
}

void proxy_exchange::finish_stopped() {
    switch (std::exchange(stopped_, stop_reason::none)) {
        case stop_reason::timeout:
            if (response_started_) {
                result_.complete = false;
                result_.error = "timeout relaying the response";
                LOG_WARNING("reverse proxy {}: {}", describe(), result_.error);
                res_.abort_stream();
            } else {
                // what the exchange failed with is the consequence of the timeout
                result_.error.clear();
                fail(http_response::status::gateway_timeout, "The target did not answer in time",
                     "timeout waiting for the target");
            }
            break;
        case stop_reason::client_left:
            result_.complete = false;
            result_.error = "the client closed the connection";
            LOG_DEBUG("reverse proxy {}: {}", describe(), result_.error);
            if (response_started_) res_.abort_stream();
            break;
        default:
            break;
    }
}

awaitable<proxy_result> proxy_exchange::run() {
    executor_ = co_await boost::asio::this_coro::executor;
    start_ = clock::now();
    last_activity_ = start_;
    if (!incoming_) {
        result_.error = "no request";
        co_return result_;
    }
    if (!prepare()) co_return result_;
    if (!io_context()) {
        fail(http_response::status::not_implemented, "", "the handler does not run on an io_context");
        co_return result_;
    }
    if (!co_await connect()) co_return result_;

    request_sent_signal_.emplace(executor_, clock::time_point::max());
    co_await (exchange() || watchdog() || watch_client());
    finish_stopped();
    close_target();
    co_return result_;
}

}

// --- reverse_proxy ---

std::shared_ptr<boost::asio::ssl::context> reverse_proxy::tls_context(const proxy_tls_options& tls) {
    auto key = std::make_tuple(tls.verify, tls.ca_file, tls.ca_pem);
    std::lock_guard lock(tls_mutex_);
    if (auto it = tls_contexts_.find(key); it != tls_contexts_.end()) return it->second;

    auto context = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_client);
    context->set_options(boost::asio::ssl::context::default_workarounds | boost::asio::ssl::context::no_sslv2 |
                         boost::asio::ssl::context::no_sslv3 | boost::asio::ssl::context::no_tlsv1 |
                         boost::asio::ssl::context::no_tlsv1_1);
    if (tls.verify) {
        context->set_verify_mode(boost::asio::ssl::verify_peer);
        if (!tls.ca_file.empty()) {
            context->load_verify_file(tls.ca_file);
        } else if (!tls.ca_pem.empty()) {
            context->add_certificate_authority(boost::asio::buffer(tls.ca_pem));
        } else {
            context->set_default_verify_paths();
        }
    } else {
        context->set_verify_mode(boost::asio::ssl::verify_none);
    }
    tls_contexts_.emplace(key, context);
    return context;
}

awaitable<proxy_result> reverse_proxy::forward(request& req, response& res, const proxy_target& target) {
    co_return co_await forward(req, res, target, defaults_);
}

awaitable<proxy_result> reverse_proxy::forward(request& req, response& res, const proxy_target& target,
                                               const proxy_options& options) {
    proxy_detail::proxy_exchange exchange(*this, req, res, target, options);
    co_return co_await exchange.run();
}

} // namespace thinger::http

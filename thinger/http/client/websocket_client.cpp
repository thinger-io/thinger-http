#include "websocket_client.hpp"
#include "../../asio/io_context_of.hpp"
#include "../../util/logger.hpp"

#include <boost/asio/use_future.hpp>

#include <future>
#include <stdexcept>

namespace thinger::http {

namespace {

    // A synchronous call: `operation` on the executor of the WebSocket, waiting for it to
    // complete. On an io_context owned by its client (http::client) the call waits running
    // it, until the operation completes (not until it runs out of work: the keepalive keeps it
    // busy). Otherwise something else runs it, and the call only waits. Refused, before
    // anything starts, on a thread running the io_context (a handler, callback or coroutine
    // of it, its strands included): it would wait there for a handler it holds up, or run
    // the handlers of other connections within its caller
    template<typename T>
    T run_blocking(asio::websocket& websocket, bool owned_io_context, const char* call, awaitable<T> operation) {
        auto& io_context = websocket.get_io_context();
        if (io_context.get_executor().running_in_this_thread() ||
            asio::running_in_this_thread(websocket.get_executor())) {
            throw std::logic_error(std::string("websocket_client::") + call +
                                   "() called on a thread running the io_context of its WebSocket, where it "
                                   "cannot wait: use the coroutine API (send_text_async(), receive_async(), "
                                   "close_async()...), or stop() to close");
        }
        auto future = boost::asio::co_spawn(websocket.get_executor(), std::move(operation), boost::asio::use_future);
        if (owned_io_context) {
            asio::run_until(io_context, [&] {
                return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            });
        }
        return future.get();
    }

    awaitable<void> apply_keepalive(std::shared_ptr<asio::websocket> websocket,
                                    std::chrono::milliseconds ping_interval, std::chrono::milliseconds idle_timeout) {
        websocket->set_keepalive(ping_interval, idle_timeout);
        co_return;
    }

}

websocket_client::websocket_client(std::shared_ptr<asio::websocket> ws)
    : websocket_(std::move(ws)) {}

websocket_client::websocket_client(std::shared_ptr<asio::websocket> ws, bool owned_io_context)
    : websocket_(std::move(ws)), owned_io_context_(owned_io_context) {}

websocket_client::~websocket_client() {
    stop();
}

websocket_client::websocket_client(websocket_client&& other) noexcept
    : websocket_(std::move(other.websocket_)),
      running_(other.running_.load()),
      on_message_(std::move(other.on_message_)),
      on_close_(std::move(other.on_close_)),
      on_error_(std::move(other.on_error_)),
      owned_io_context_(other.owned_io_context_) {
    other.running_ = false;
}

websocket_client& websocket_client::operator=(websocket_client&& other) noexcept {
    if (this != &other) {
        stop();
        websocket_ = std::move(other.websocket_);
        running_ = other.running_.load();
        on_message_ = std::move(other.on_message_);
        on_close_ = std::move(other.on_close_);
        on_error_ = std::move(other.on_error_);
        owned_io_context_ = other.owned_io_context_;
        other.running_ = false;
    }
    return *this;
}

bool websocket_client::is_open() const {
    return websocket_ && websocket_->is_open();
}

std::shared_ptr<asio::websocket> websocket_client::release_socket() {
    running_ = false;
    return std::move(websocket_);
}

// ============================================
// Synchronous API
// ============================================

bool websocket_client::send_text(std::string_view message) {
    if (!is_open()) return false;
    return run_blocking(*websocket_, owned_io_context_, "send_text", send_text_async(std::string(message)));
}

bool websocket_client::send_binary(const void* data, size_t size) {
    return send_binary(std::string_view(static_cast<const char*>(data), size));
}

bool websocket_client::send_binary(std::string_view data) {
    if (!is_open()) return false;
    return run_blocking(*websocket_, owned_io_context_, "send_binary", send_binary_async(std::vector<uint8_t>(data.begin(), data.end())));
}

std::pair<std::string, bool> websocket_client::receive() {
    if (!is_open()) return {"", false};
    return run_blocking(*websocket_, owned_io_context_, "receive", receive_async());
}

void websocket_client::close() {
    if (!websocket_) return;
    run_blocking(*websocket_, owned_io_context_, "close", close_async());
}

void websocket_client::set_keepalive(std::chrono::milliseconds ping_interval, std::chrono::milliseconds idle_timeout) {
    if (!websocket_) return;
    if (asio::running_in_this_thread(websocket_->get_executor())) {
        websocket_->set_keepalive(ping_interval, idle_timeout);
        return;
    }
    run_blocking(*websocket_, owned_io_context_, "set_keepalive", apply_keepalive(websocket_, ping_interval, idle_timeout));
}

std::chrono::milliseconds websocket_client::get_ping_interval() const {
    return websocket_ ? websocket_->get_ping_interval() : std::chrono::milliseconds{0};
}

std::chrono::milliseconds websocket_client::get_idle_timeout() const {
    return websocket_ ? websocket_->get_idle_timeout() : std::chrono::milliseconds{0};
}

// ============================================
// Async/Callback API
// ============================================

void websocket_client::run() {
    if (!is_open() || running_) return;

    running_ = true;

    boost::asio::co_spawn(
        websocket_->get_executor(),
        [this]() -> awaitable<void> {
            co_await message_loop();
        },
        boost::asio::detached
    );
}

void websocket_client::stop() {
    running_ = false;
    if (websocket_) {
        websocket_->close();
    }
}

awaitable<void> websocket_client::message_loop() {
    while (running_ && is_open()) {
        // An empty message is a message: the loop ends when reading fails (closed or lost)
        auto message = co_await read_message();
        if (!message) break;

        if (on_message_) {
            on_message_(message->first, message->second);
        }
    }

    running_ = false;

    if (on_close_) {
        on_close_();
    }
}

// ============================================
// Coroutine API
// ============================================

awaitable<bool> websocket_client::send_text_async(std::string message) {
    if (!is_open()) co_return false;

    websocket_->set_binary(false);
    auto [ec, bytes] = co_await websocket_->write_message(message);
    co_return !ec;
}

awaitable<bool> websocket_client::send_binary_async(std::vector<uint8_t> data) {
    if (!is_open()) co_return false;

    websocket_->set_binary(true);
    auto [ec, bytes] = co_await websocket_->write_message(
        std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
    co_return !ec;
}

awaitable<std::pair<std::string, bool>> websocket_client::receive_async() {
    auto message = co_await read_message();
    if (!message) co_return std::make_pair(std::string{}, false);
    co_return std::move(*message);
}

awaitable<std::optional<std::pair<std::string, bool>>> websocket_client::read_message() {
    if (!is_open()) co_return std::nullopt;

    // A whole message: every frame of it, read in pieces (it may be empty)
    std::string message;
    std::array<uint8_t, 65536> buffer;
    for (;;) {
        auto [ec, bytes_read] = co_await websocket_->read_message_some(buffer.data(), buffer.size());
        if (ec) co_return std::nullopt;
        message.append(reinterpret_cast<const char*>(buffer.data()), bytes_read);
        if (message.size() > MAX_MESSAGE_SIZE) {
            LOG_ERROR("websocket message larger than {} bytes, closing", MAX_MESSAGE_SIZE);
            websocket_->close();
            co_return std::nullopt;
        }
        if (websocket_->remaining_in_frame() == 0 && websocket_->is_message_complete()) break;
    }

    bool is_binary = websocket_->is_binary();
    co_return std::make_pair(std::move(message), is_binary);
}

awaitable<void> websocket_client::close_async() {
    if (websocket_) {
        co_await websocket_->close_graceful();
    }
    running_ = false;
}

} // namespace thinger::http

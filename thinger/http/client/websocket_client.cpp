#include "websocket_client.hpp"
#include "../../util/logger.hpp"

#include <future>

namespace thinger::http {

websocket_client::websocket_client(std::shared_ptr<asio::websocket> ws)
    : websocket_(std::move(ws)) {}

websocket_client::~websocket_client() {
    stop();
}

websocket_client::websocket_client(websocket_client&& other) noexcept
    : websocket_(std::move(other.websocket_)),
      running_(other.running_.load()),
      on_message_(std::move(other.on_message_)),
      on_close_(std::move(other.on_close_)),
      on_error_(std::move(other.on_error_)) {
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

    std::promise<bool> promise;
    auto future = promise.get_future();

    auto& io_context = websocket_->get_io_context();

    boost::asio::co_spawn(
        io_context,
        [this, msg = std::string(message), &promise]() -> awaitable<void> {
            try {
                bool result = co_await send_text_async(std::move(msg));
                promise.set_value(result);
            } catch (...) {
                promise.set_exception(std::current_exception());
            }
        },
        boost::asio::detached
    );

    // If we're already running on the io_context's thread (e.g., from async_client callback),
    // we need to poll to let our coroutine run. Otherwise, drive the io_context ourselves.
    if (io_context.get_executor().running_in_this_thread()) {
        while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            io_context.poll_one();
        }
    } else {
        io_context.run();
        io_context.restart();
    }

    return future.get();
}

bool websocket_client::send_binary(const void* data, size_t size) {
    return send_binary(std::string_view(static_cast<const char*>(data), size));
}

bool websocket_client::send_binary(std::string_view data) {
    if (!is_open()) return false;

    std::promise<bool> promise;
    auto future = promise.get_future();

    auto& io_context = websocket_->get_io_context();
    std::vector<uint8_t> data_vec(data.begin(), data.end());

    boost::asio::co_spawn(
        io_context,
        [this, data_vec = std::move(data_vec), &promise]() mutable -> awaitable<void> {
            try {
                bool result = co_await send_binary_async(std::move(data_vec));
                promise.set_value(result);
            } catch (...) {
                promise.set_exception(std::current_exception());
            }
        },
        boost::asio::detached
    );

    if (io_context.get_executor().running_in_this_thread()) {
        while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            io_context.poll_one();
        }
    } else {
        io_context.run();
        io_context.restart();
    }

    return future.get();
}

std::pair<std::string, bool> websocket_client::receive() {
    if (!is_open()) return {"", false};

    std::promise<std::pair<std::string, bool>> promise;
    auto future = promise.get_future();

    auto& io_context = websocket_->get_io_context();

    boost::asio::co_spawn(
        io_context,
        [this, &promise]() -> awaitable<void> {
            try {
                auto result = co_await receive_async();
                promise.set_value(std::move(result));
            } catch (...) {
                promise.set_exception(std::current_exception());
            }
        },
        boost::asio::detached
    );

    if (io_context.get_executor().running_in_this_thread()) {
        while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            io_context.poll_one();
        }
    } else {
        io_context.run();
        io_context.restart();
    }

    return future.get();
}

void websocket_client::close() {
    if (!websocket_) return;

    std::promise<void> promise;
    auto future = promise.get_future();

    auto& io_context = websocket_->get_io_context();

    boost::asio::co_spawn(
        io_context,
        [this, &promise]() -> awaitable<void> {
            try {
                co_await close_async();
                promise.set_value();
            } catch (...) {
                promise.set_exception(std::current_exception());
            }
        },
        boost::asio::detached
    );

    if (io_context.get_executor().running_in_this_thread()) {
        while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            io_context.poll_one();
        }
    } else {
        io_context.run();
        io_context.restart();
    }

    try {
        future.get();
    } catch (...) {
        // Ignore close errors
    }
}

// ============================================
// Async/Callback API
// ============================================

void websocket_client::run() {
    if (!is_open() || running_) return;

    running_ = true;

    boost::asio::co_spawn(
        websocket_->get_io_context(),
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
        try {
            auto [message, is_binary] = co_await receive_async();

            if (message.empty() && !is_open()) {
                break;
            }

            if (on_message_) {
                on_message_(message, is_binary);
            }

        } catch (const std::exception& e) {
            LOG_DEBUG("WebSocket message loop ended: {}", e.what());
            if (on_error_) {
                on_error_(e.what());
            }
            break;
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

    try {
        websocket_->set_binary(false);
        co_await websocket_->write(message);
        co_return true;
    } catch (const std::exception& e) {
        LOG_ERROR("WebSocket send error: {}", e.what());
        co_return false;
    }
}

awaitable<bool> websocket_client::send_binary_async(std::vector<uint8_t> data) {
    if (!is_open()) co_return false;

    try {
        websocket_->set_binary(true);
        co_await websocket_->write(data.data(), data.size());
        co_return true;
    } catch (const std::exception& e) {
        LOG_ERROR("WebSocket send error: {}", e.what());
        co_return false;
    }
}

awaitable<std::pair<std::string, bool>> websocket_client::receive_async() {
    if (!is_open()) {
        co_return std::make_pair(std::string{}, false);
    }

    try {
        std::array<uint8_t, 65536> buffer;
        size_t bytes_read = co_await websocket_->read_some(buffer.data(), buffer.size());

        std::string message(reinterpret_cast<char*>(buffer.data()), bytes_read);
        bool is_binary = websocket_->is_binary();

        co_return std::make_pair(std::move(message), is_binary);

    } catch (const std::exception& e) {
        LOG_DEBUG("WebSocket receive ended: {}", e.what());
        co_return std::make_pair(std::string{}, false);
    }
}

awaitable<void> websocket_client::close_async() {
    if (websocket_) {
        try {
            co_await websocket_->close_graceful();
        } catch (...) {
            // Ignore close errors
        }
    }
    running_ = false;
}

} // namespace thinger::http

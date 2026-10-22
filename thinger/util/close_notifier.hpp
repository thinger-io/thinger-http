#ifndef THINGER_UTIL_CLOSE_NOTIFIER_HPP
#define THINGER_UTIL_CLOSE_NOTIFIER_HPP

#include <optional>
#include <tuple>
#include <utility>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/post.hpp>
#include "logger.hpp"
#include "types.hpp"

namespace thinger::util {

// The close callback of a connection (see asio::websocket::on_close(),
// http::websocket_connection::on_close() and http::sse_connection::on_close()), with what the
// connection tells it (`Args`): called once, the connection closed, always posted to its
// executor, so never from within the call that closed it (e.g. a send from a loop over the
// connections, which may then remove this one), and what it throws is logged. Set before or
// after the close: set once closed, it is posted at once. Used on the thread of the connection,
// as the connection itself.
template<typename... Args>
class close_notifier {
public:
    using callback_type = sync_callback<void(Args...)>;

    explicit close_notifier(boost::asio::any_io_executor executor) : executor_(std::move(executor)) {}

    void set(callback_type callback) {
        if (released_) return;
        if (closed_) {
            if (callback) post(std::move(callback), *closed_);
            return;
        }
        callback_ = std::move(callback);
    }

    // The connection closed (once: later calls are ignored)
    void notify(Args... args) {
        if (closed_ || released_) return;
        closed_.emplace(std::move(args)...);
        // released here: it may own the connection
        auto callback = std::exchange(callback_, nullptr);
        if (callback) post(std::move(callback), *closed_);
    }

    // Drop the callback: the connection will never be told closed (e.g. its socket was
    // released, or it is destroyed open), so neither any callback set afterwards. Once told
    // closed, a callback set afterwards is still posted
    void release() {
        if (!closed_) released_ = true;
        callback_ = nullptr;
    }

private:
    void post(callback_type callback, std::tuple<Args...> args) {
        boost::asio::post(executor_, [callback = std::move(callback), args = std::move(args)]() mutable {
            try {
                std::apply(callback, std::move(args));
            } catch (const std::exception& e) {
                LOG_ERROR("error in the close callback of a connection: {}", e.what());
            } catch (...) {
                LOG_ERROR("unknown error in the close callback of a connection");
            }
        });
    }

    boost::asio::any_io_executor executor_;
    callback_type callback_;
    // What the connection told once closed
    std::optional<std::tuple<Args...>> closed_;
    bool released_ = false;
};

} // namespace thinger::util

#endif // THINGER_UTIL_CLOSE_NOTIFIER_HPP

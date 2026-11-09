#include "server_connection.hpp"
#include "request.hpp"
#include "../../asio/sockets/socket.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace thinger::http {

namespace detail {

    // Source of a request read from a server connection: what its client sends after the
    // request head, through the connection while it lives (a request may outlive it). Owned
    // by the request, as is the stream it refers to
    class connection_source final : public request_source {
    public:
        connection_source(std::weak_ptr<server_connection> connection, http_stream& stream)
            : connection_(std::move(connection)), stream_(stream) {}

        awaitable<io_result> read_body_some(uint8_t* buffer, size_t size) override {
            auto connection = connection_.lock();
            if (!connection) co_return io_result{boost::asio::error::eof, 0};
            // On the thread of the connection, whose socket and deadline are not thread-safe: a
            // handler that moved to another executor reads there, and resumes on its own
            auto& io_context = connection->socket_->get_io_context();
            if (!io_context.get_executor().running_in_this_thread()) [[unlikely]] {
                co_return co_await boost::asio::co_spawn(io_context, read_body_some(buffer, size),
                                                         boost::asio::use_awaitable);
            }
            // Nothing more is read once the connection closes gracefully: it drops the rest
            // itself (see server_connection::linger())
            if (connection->lingering()) co_return io_result{boost::asio::error::operation_aborted, 0};
            // The body is needed now: ask the client for it (Expect: 100-continue)
            if (stream_.get_continue() == http_stream::continue_state::pending) [[unlikely]] {
                connection->send_continue(stream_);
            }
            io_result result{boost::system::errc::make_error_code(read_deadline_expired), 0};
            if (!connection->read_deadline_passed()) [[likely]] {
                auto caller = (co_await boost::asio::this_coro::cancellation_state).slot();
                connection->body_read_started(caller);
                result = co_await connection->socket_->read_some_cancellable(buffer, size, connection->read_cancel_.slot());
                connection->body_read_ended(caller, result);
            }
            auto& [ec, bytes] = result;
            // A body that keeps arriving keeps the connection alive (e.g. a long upload)
            if (bytes > 0) connection->extend_timeout();
            // Read only while the body is not complete: its end, or an error, before it is the
            // client gone. Not a read cancelled (by the handler, or as the connection closes
            // gracefully), nor one of a connection closed here. Past its read deadline the
            // client is still there, but the body stays unread: the connection closes after
            // the response, which tells the client
            if (ec) [[unlikely]] {
                if (ec == read_deadline_expired) {
                    stream_.must_close(close_reason::body_unread);
                } else if (ec != boost::asio::error::operation_aborted && !connection->lingering() &&
                           connection->socket_->is_open()) {
                    stream_.cancel(cancel_reason::client_gone);
                }
            }
            co_return result;
        }

        connection_info info() const override {
            auto connection = connection_.lock();
            if (!connection) return {};
            const auto& socket = *connection->socket_;
            return {socket.get_remote_ip(), socket.get_remote_port(), socket.get_local_port(), socket.is_secure()};
        }

        std::optional<asio::tls_session> tls() const override {
            auto connection = connection_.lock();
            return connection ? connection->socket_->tls() : std::nullopt;
        }

        bool misdirected(std::string_view host) const override {
            auto connection = connection_.lock();
            return connection && connection->socket_->misdirected(host);
        }

        void hold_until(std::chrono::steady_clock::time_point time) override {
            if (auto connection = connection_.lock()) connection->hold_until(time);
        }

        void set_request_deadline(std::chrono::milliseconds deadline) override {
            if (auto connection = connection_.lock()) connection->set_request_deadline(stream_, deadline);
        }

        void set_read_deadline(std::chrono::steady_clock::time_point deadline) override {
            auto connection = connection_.lock();
            if (!connection) return;
            // On the thread of the connection, right here if called there. The stream is only
            // compared there, as it may be gone by then
            boost::asio::dispatch(connection->socket_->get_io_context(),
                                  [connection, stream = &stream_, deadline] {
                connection->set_read_deadline(stream, deadline);
            });
        }

        std::weak_ptr<server_connection> connection() const override { return connection_; }

        boost::asio::io_context* io_context() const override {
            auto connection = connection_.lock();
            return connection ? &connection->socket_->get_io_context() : nullptr;
        }

    private:
        std::weak_ptr<server_connection> connection_;
        http_stream& stream_;
    };

}

std::unique_ptr<detail::request_source> server_connection::make_source(http_stream& stream) {
    return std::make_unique<detail::connection_source>(weak_from_this(), stream);
}

}

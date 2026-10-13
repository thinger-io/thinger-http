#ifndef THINGER_ASIO_SOCKET_PIPE_HPP
#define THINGER_ASIO_SOCKET_PIPE_HPP

#include <memory>
#include <functional>
#include <atomic>

#include "sockets/socket.hpp"
#include "../util/types.hpp"

namespace thinger::asio {

/// Bidirectional coroutine-based pipe between two sockets: a tunnel between any two sockets
/// (TCP, TLS, Unix, a WebSocket, or one defined by the application, see sockets/socket.hpp).
/// Each direction writes what it read before reading more, so a slow reader slows its writer
/// down. Takes exclusive ownership of both sockets and closes them when the pipe ends, which
/// it does when either direction ends (the end of a stream, or an error): closing one end of
/// the tunnel closes the other.
/// Both sockets are used from the coroutine running run() (start() runs it on the io_context
/// of the source), so they must belong to that io_context.
/// set_on_end() must be called before run()/start().
/// cancel(), the transfer stats and the socket getters may be called from any thread.
class socket_pipe : public std::enable_shared_from_this<socket_pipe> {
public:
    static constexpr size_t BUFFER_SIZE = 8192;

    socket_pipe(std::shared_ptr<socket> source, std::shared_ptr<socket> target);
    ~socket_pipe();

    /// Run bidirectionally. Completes when either direction ends or errors.
    awaitable<void> run();

    /// Fire-and-forget: co_spawn(run()) on the source socket's io_context.
    void start();

    /// Close both sockets, each on its own io_context (right away when called from a thread
    /// running it), so run() completes. Thread-safe; only the first call closes them.
    void cancel();

    /// Completion callback (called from destructor).
    void set_on_end(sync_callback<void()> listener);

    /// Transfer stats (safe to read from any thread after run() completes).
    size_t bytes_source_to_target() const;
    size_t bytes_target_to_source() const;

    /// Socket access.
    std::shared_ptr<socket> get_source() const;
    std::shared_ptr<socket> get_target() const;

private:
    /// Forward data in one direction: from -> to.
    awaitable<void> forward(
        std::shared_ptr<socket> from,
        std::shared_ptr<socket> to,
        std::atomic<size_t>& bytes_transferred);

    std::shared_ptr<socket> source_;
    std::shared_ptr<socket> target_;
    sync_callback<void()> on_end_;
    std::atomic<bool> cancelled_{false};
    std::atomic<size_t> bytes_s2t_{0};
    std::atomic<size_t> bytes_t2s_{0};
};

} // namespace thinger::asio

#endif

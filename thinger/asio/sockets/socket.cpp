#include <array>
#include <iostream>
#include "socket.hpp"
#if !defined(BOOST_ASIO_WINDOWS)
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#endif
#if defined(__linux__)
#include <linux/sockios.h>
#include <sys/ioctl.h>
#endif

namespace thinger::asio {

    std::atomic<unsigned long> socket::connections(0);

    socket::socket(const std::string&, boost::asio::io_context& io_context)
        : io_context_(io_context) {
        ++connections;
    }

    socket::~socket() {
        --connections;
    }

    boost::asio::io_context& socket::get_io_context() const {
        return io_context_;
    }

    bool socket::requires_handshake() const {
        return false;
    }

    awaitable<boost::system::error_code> socket::handshake(const std::string& host) {
        co_return boost::system::error_code{};
    }

    size_t socket::write_now(const std::vector<boost::asio::const_buffer>&, boost::system::error_code& ec) {
        ec = boost::asio::error::operation_not_supported;
        return 0;
    }

    size_t socket::send_now(native_handle_type descriptor, const std::vector<boost::asio::const_buffer>& buffers,
                            boost::system::error_code& ec) {
#if defined(MSG_DONTWAIT) && !defined(BOOST_ASIO_WINDOWS)
        std::array<iovec, 64> iov;
        size_t count = std::min(buffers.size(), iov.size());
        for (size_t i = 0; i < count; ++i) {
            iov[i].iov_base = const_cast<void*>(buffers[i].data());
            iov[i].iov_len = buffers[i].size();
        }
        msghdr message{};
        message.msg_iov = iov.data();
        message.msg_iovlen = count;
        int flags = MSG_DONTWAIT;
#if defined(MSG_NOSIGNAL)
        flags |= MSG_NOSIGNAL;
#endif
        ssize_t bytes;
        do {
            bytes = ::sendmsg(descriptor, &message, flags);
        } while (bytes < 0 && errno == EINTR);
        if (bytes < 0) {
            ec = boost::system::error_code(errno, boost::asio::error::get_system_category());
            if (ec == boost::asio::error::try_again) ec = boost::asio::error::would_block;
            return 0;
        }
        ec = {};
        return static_cast<size_t>(bytes);
#else
        ec = boost::asio::error::operation_not_supported;
        return 0;
#endif
    }

    awaitable<boost::system::error_code> socket::peer_closed() {
        co_return boost::asio::error::operation_not_supported;
    }

    awaitable<boost::system::error_code> socket::connect(const std::string&, const std::string&, std::chrono::seconds) {
        co_return boost::asio::error::operation_not_supported;
    }

    awaitable<boost::system::error_code> socket::wait(boost::asio::socket_base::wait_type) {
        co_return boost::asio::error::operation_not_supported;
    }

    bool socket::is_secure() const {
        return false;
    }

    size_t socket::available() const {
        return 0;
    }

    bool socket::readable_now() const {
        return false;
    }

    size_t socket::send_backlog() const {
        return 0;
    }

    size_t socket::output_queue(native_handle_type descriptor) {
        int bytes = 0;
#if defined(__linux__)
        // TCP: not acknowledged, sent or not; Unix: not read
        if (::ioctl(descriptor, SIOCOUTQ, &bytes) != 0) return 0;
#elif defined(SO_NWRITE)
        socklen_t size = sizeof(bytes);
        if (::getsockopt(descriptor, SOL_SOCKET, SO_NWRITE, &bytes, &size) != 0) return 0;
#else
        (void) descriptor;
#endif
        return bytes > 0 ? size_t(bytes) : 0;
    }

    bool socket::poll_readable(native_handle_type descriptor) {
#if !defined(BOOST_ASIO_WINDOWS)
        pollfd fd{};
        fd.fd = descriptor;
        fd.events = POLLIN;
        int ready;
        do {
            ready = ::poll(&fd, 1, 0);
        } while (ready < 0 && errno == EINTR);
        // POLLIN, POLLHUP, POLLERR or POLLNVAL, or poll itself failing
        return ready != 0;
#else
        (void) descriptor;
        return false;
#endif
    }

    boost::system::error_code socket::peek_closed(native_handle_type descriptor) {
#if defined(MSG_DONTWAIT) && !defined(BOOST_ASIO_WINDOWS)
        char byte;
        ssize_t bytes;
        do {
            bytes = ::recv(descriptor, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        } while (bytes < 0 && errno == EINTR);
        if (bytes > 0) return {};
        if (bytes == 0) return boost::asio::error::eof;
        boost::system::error_code ec(errno, boost::asio::error::get_system_category());
        // Nothing to read yet: no conclusion, as with data
        if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) return {};
        return ec;
#else
        (void) descriptor;
        return boost::asio::error::operation_not_supported;
#endif
    }

    std::string socket::get_remote_ip() const {
        return {};
    }

    std::string socket::get_local_port() const {
        return {};
    }

    std::string socket::get_remote_port() const {
        return {};
    }

    std::optional<tls_session> socket::tls() const {
        return std::nullopt;
    }

    awaitable<io_result> socket::read(uint8_t buffer[], size_t size) {
        size_t total = 0;
        while (total < size) {
            auto [ec, bytes] = co_await read_some(buffer + total, size - total);
            total += bytes;
            if (ec) co_return io_result{ec, total};
            // a socket that keeps the guarantees never returns 0 bytes here: do not spin
            if (bytes == 0) co_return io_result{boost::asio::error::eof, total};
        }
        co_return io_result{boost::system::error_code{}, total};
    }

    awaitable<io_result> socket::read(boost::asio::streambuf& buffer, size_t size) {
        size_t total = 0;
        while (total < size) {
            boost::asio::streambuf::mutable_buffers_type space;
            try {
                space = buffer.prepare(size - total);
            } catch (const std::length_error&) {
                co_return io_result{boost::asio::error::no_buffer_space, total};
            }
            auto [ec, bytes] = co_await read_some(static_cast<uint8_t*>(space.data()), space.size());
            buffer.commit(bytes);
            total += bytes;
            if (ec) co_return io_result{ec, total};
            if (bytes == 0) co_return io_result{boost::asio::error::eof, total};
        }
        co_return io_result{boost::system::error_code{}, total};
    }

    awaitable<io_result> socket::read_until(boost::asio::streambuf& buffer, std::string_view delim) {
        // Look for the delimiter in what the buffer holds, then in what is read, as
        // boost::asio::async_read_until does
        size_t search_from = 0;
        for (;;) {
            auto data = buffer.data();
            std::string_view held(static_cast<const char*>(data.data()), data.size());
            auto position = held.find(delim, search_from);
            if (position != std::string_view::npos) {
                co_return io_result{boost::system::error_code{}, position + delim.size()};
            }
            // the delimiter may start in the last bytes held
            search_from = held.size() >= delim.size() ? held.size() - delim.size() + 1 : 0;
            if (buffer.size() >= buffer.max_size()) {
                co_return io_result{boost::asio::error::not_found, 0};
            }
            auto read_size = std::min<size_t>(std::max<size_t>(512, buffer.capacity() - buffer.size()),
                                              std::min<size_t>(65536, buffer.max_size() - buffer.size()));
            auto space = buffer.prepare(read_size);
            auto [ec, bytes] = co_await read_some(static_cast<uint8_t*>(space.data()), space.size());
            buffer.commit(bytes);
            if (ec) co_return io_result{ec, 0};
            if (bytes == 0) co_return io_result{boost::asio::error::eof, 0};
        }
    }

    awaitable<io_result> socket::write(std::string_view str) {
        co_return co_await write(reinterpret_cast<const uint8_t*>(str.data()), str.size());
    }

    awaitable<io_result> socket::write(const std::vector<boost::asio::const_buffer>& buffers) {
        size_t total = 0;
        for (const auto& buffer : buffers) {
            if (buffer.size() == 0) continue;
            auto [ec, bytes] = co_await write(static_cast<const uint8_t*>(buffer.data()), buffer.size());
            total += bytes;
            if (ec) co_return io_result{ec, total};
        }
        co_return io_result{boost::system::error_code{}, total};
    }

    awaitable<io_result> socket::write_slices(std::vector<boost::asio::const_buffer>& buffers,
                                              const sync_callback<void()>& progress) {
        std::vector<boost::asio::const_buffer> slice;
        size_t total = 0;
        consume(buffers, 0);
        while (!buffers.empty()) {
            slice.clear();
            for (size_t size = 0, i = 0; i < buffers.size() && size < WRITE_SLICE_SIZE; ++i) {
                slice.push_back(boost::asio::buffer(buffers[i], WRITE_SLICE_SIZE - size));
                size += slice.back().size();
            }
            auto [ec, bytes] = co_await write(slice);
            total += bytes;
            consume(buffers, bytes);
            if (ec) co_return io_result{ec, total};
            progress();
        }
        co_return io_result{boost::system::error_code{}, total};
    }

    void socket::consume(std::vector<boost::asio::const_buffer>& buffers, size_t size) {
        auto remaining = buffers.begin();
        while (remaining != buffers.end() && size >= remaining->size()) {
            size -= remaining->size();
            ++remaining;
        }
        if (remaining != buffers.end()) *remaining += size;
        buffers.erase(buffers.begin(), remaining);
    }

}
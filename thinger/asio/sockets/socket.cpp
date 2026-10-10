#include <array>
#include <iostream>
#include "socket.hpp"
#if !defined(BOOST_ASIO_WINDOWS)
#include <sys/socket.h>
#include <sys/uio.h>
#endif

namespace thinger::asio {

    std::atomic<unsigned long> socket::connections(0);

    socket::socket(const std::string& context, boost::asio::io_context& io_context)
        : context_(context), io_context_(io_context) {
        ++connections;
        std::lock_guard<std::mutex> lock(mutex_);
        context_count[context_]++;
    }

    socket::~socket() {
        --connections;
        std::lock_guard<std::mutex> lock(mutex_);
        if (--context_count[context_] == 0) {
            context_count.erase(context_);
        }
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

    awaitable<bool> socket::peer_closed() {
        // readable with nothing to read: end of stream, or an error
        co_return available() == 0;
    }

    std::map<std::string, unsigned long> socket::context_count;
    std::mutex socket::mutex_;
}
#include <iostream>
#include "socket.hpp"

namespace thinger::asio {

    std::atomic<unsigned long> socket::connections(0);

    socket::socket(const std::string& context, boost::asio::io_context& io_context)
        : context_(context), io_context_(io_context) {
        ++connections;
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        context_count[context_]++;
    }

    socket::~socket() {
        --connections;
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        context_count[context_]--;
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

    std::map<std::string, unsigned long> socket::context_count;
    std::mutex socket::mutex_;
}
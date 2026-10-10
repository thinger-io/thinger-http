#include "basic_socket_server.hpp"
#include "worker_thread.hpp"
#include "../util/logger.hpp"
#include <cerrno>
#include <chrono>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <sys/socket.h>
#include <unistd.h>

namespace thinger::asio {

namespace {

// An accepted descriptor on its way to the io_context serving it, closed if it never gets there
class accepted_descriptor {
public:
    explicit accepted_descriptor(int descriptor) : descriptor_(descriptor) {}
    accepted_descriptor(accepted_descriptor&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    accepted_descriptor& operator=(accepted_descriptor&&) = delete;
    ~accepted_descriptor() { if (descriptor_ >= 0) ::close(descriptor_); }

    int get() const { return descriptor_; }
    int release() { return std::exchange(descriptor_, -1); }

private:
    int descriptor_;
};

} // namespace

template<typename Protocol>
basic_socket_server<Protocol>::~basic_socket_server() {
    stop_acceptor_thread();
    close_acceptor();
}

template<typename Protocol>
bool basic_socket_server<Protocol>::stop() {
    // First call base class to set running_ = false
    socket_server_base::stop();

    // Now close the acceptor, once its own thread (if any) is not using it
    stop_acceptor_thread();
    close_acceptor();

    return true;
}

template<typename Protocol>
void basic_socket_server<Protocol>::stop_acceptor_thread() {
    // The thread object is kept while the acceptor uses its io_context
    if (acceptor_thread_) acceptor_thread_->stop();
}

template<typename Protocol>
void basic_socket_server<Protocol>::close_acceptor() {
    // Close the acceptor to cancel pending async operations, but do NOT
    // destroy it (reset) here. The async_accept handler may still be in
    // flight on the io_context thread and needs the acceptor alive until
    // the handler completes. The unique_ptr will clean up on destruction.
    if (acceptor_ && acceptor_->is_open()) {
        boost::system::error_code ec;
        acceptor_->close(ec);
        if (ec) {
            LOG_WARNING("Error closing acceptor: {}", ec.message());
        }
    }
}

template<typename Protocol>
bool basic_socket_server<Protocol>::create_acceptor() {
    if (own_acceptor_thread_) {
        acceptor_.reset(); // it uses the io_context of the previous thread, if any
        acceptor_thread_ = std::make_unique<worker_thread>(get_service_name() + " acceptor");
        acceptor_thread_->start();
        if (listen(acceptor_thread_->get_io_context())) return true;
        stop_acceptor_thread();
        return false;
    }
    return listen(acceptor_context_provider_());
}

template<typename Protocol>
bool basic_socket_server<Protocol>::listen(boost::asio::io_context& io_context) {
    auto endpoint = listening_endpoint(io_context);
    if (!endpoint) return false;

    int num_attempts = 0;
    while (true) {
        LOG_DEBUG("starting acceptor of {}", get_service_name());
        try {
            auto acceptor = std::make_unique<acceptor_type>(io_context);
            acceptor->open(endpoint->protocol());
            if constexpr (std::is_same_v<Protocol, boost::asio::ip::tcp>) {
                acceptor->set_option(typename acceptor_type::reuse_address(true));
            }
            acceptor->bind(*endpoint);
            acceptor->listen();
            acceptor_ = std::move(acceptor);
            protocol_ = endpoint->protocol();
            on_listening();
            return true;
        } catch (const boost::system::system_error& error) {
            LOG_ERROR("cannot start listening on {}: {}", get_service_name(), error.code().message());
        }
        if (max_listening_attempts_ >= 0 && ++num_attempts >= max_listening_attempts_) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

template<typename Protocol>
void basic_socket_server<Protocol>::accept_connection() {
    auto serve = make_connection_server();
    if (!serve) return;
    serve_ = std::make_shared<const connection_server>(std::move(serve));

    // On a thread of its own, every pending connection is accepted at once, without waiting
    if (acceptor_thread_) {
        boost::system::error_code ec;
        acceptor_->non_blocking(true, ec);
        if (ec) {
            LOG_ERROR("cannot accept connections: {}", ec.message());
            return;
        }
        boost::asio::post(acceptor_thread_->get_io_context(), [this] { accept_pending(); });
        return;
    }

    accept_next();
}

template<typename Protocol>
void basic_socket_server<Protocol>::accept_next() {
    // The connection is accepted on the next io_context from the provider
    boost::asio::any_io_executor executor = connection_context_provider_().get_executor();
    acceptor_->async_accept(executor, [this](const boost::system::error_code& e, socket_type peer) {
        if (!e) {
            if (is_connection_allowed(peer.native_handle())) {
                (*serve_)(std::move(peer));
            }

            // Continue accepting connections
            if (running_) accept_next();
        } else if (e != boost::asio::error::operation_aborted) {
            LOG_ERROR("cannot accept more connections: {}", e.message());
            retry_accept();
        } else {
            LOG_INFO("stop accepting connections");
        }
    });
}

template<typename Protocol>
void basic_socket_server<Protocol>::wait_connections() {
    acceptor_->async_wait(boost::asio::socket_base::wait_read, [this](const boost::system::error_code& e) {
        if (!e) {
            accept_pending();
        } else if (e != boost::asio::error::operation_aborted) {
            LOG_ERROR("cannot accept more connections: {}", e.message());
            retry_accept();
        } else {
            LOG_INFO("stop accepting connections");
        }
    });
}

template<typename Protocol>
void basic_socket_server<Protocol>::accept_pending() {
    // Only this thread uses the acceptor (it is closed once the thread stops), and it is the
    // only one running its io_context, so no other thread takes its readiness meanwhile
    while (running_) {
        int descriptor = ::accept(acceptor_->native_handle(), nullptr, nullptr);
        if (descriptor >= 0) {
#if defined(SO_NOSIGPIPE)
            int enabled = 1;
            ::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
            hand_over(descriptor);
            continue;
        }
        int error = errno;
        if (error == EINTR || error == ECONNABORTED || error == EPROTO) continue;
        if (error == EAGAIN || error == EWOULDBLOCK) {
            wait_connections();
        } else {
            LOG_ERROR("cannot accept more connections: {}", std::error_code(error, std::system_category()).message());
            retry_accept();
        }
        return;
    }
}

template<typename Protocol>
void basic_socket_server<Protocol>::hand_over(native_handle_type accepted) {
    accepted_descriptor descriptor(accepted);
    if (!is_connection_allowed(descriptor.get())) return;

    // The connection is registered and set up on the worker serving it, waking it up once
    boost::asio::io_context& io_context = connection_context_provider_();
    boost::asio::post(io_context,
        [&io_context, descriptor = std::move(descriptor), protocol = protocol_, serve = serve_]() mutable {
            socket_type peer(io_context);
            boost::system::error_code ec;
            peer.assign(protocol, descriptor.get(), ec);
            if (ec) {
                LOG_ERROR("cannot serve an accepted connection: {}", ec.message());
                return;
            }
            descriptor.release();
            (*serve)(std::move(peer));
        });
}

template<typename Protocol>
void basic_socket_server<Protocol>::retry_accept() {
    if (!running_) return;
    // Retry after a delay to avoid tight loop on persistent errors
    auto timer = std::make_shared<boost::asio::steady_timer>(acceptor_->get_executor(), std::chrono::seconds(1));
    timer->async_wait([this, timer](const boost::system::error_code& e) {
        if (e == boost::asio::error::operation_aborted) return;
        if (acceptor_thread_) accept_pending();
        else accept_next();
    });
}

template class basic_socket_server<boost::asio::ip::tcp>;
template class basic_socket_server<boost::asio::local::stream_protocol>;

} // namespace thinger::asio

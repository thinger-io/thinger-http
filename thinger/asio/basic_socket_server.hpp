#ifndef THINGER_ASIO_BASIC_SOCKET_SERVER_HPP
#define THINGER_ASIO_BASIC_SOCKET_SERVER_HPP

#include "socket_server_base.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <optional>

namespace thinger::asio {

class worker_thread;

// A socket server listening with an acceptor of the given protocol (TCP or Unix domain sockets).
//
// Given io_context providers, it accepts on the acceptor context and serves each connection on
// the next connection context. On the workers, it accepts on a thread of its own instead: once
// the listening socket is readable it accepts every pending connection and hands each one over
// to the next worker, in turn, where the socket is registered and set up, waking that worker up
// once. Accepting does not wait behind the connections a worker serves.
//
// Derived servers stop in their destructors: the acceptor thread uses their overrides.
template<typename Protocol>
class basic_socket_server : public socket_server_base {
public:
    using socket_server_base::socket_server_base;
    ~basic_socket_server() override;

    // Stop accepting connections and close the acceptor
    bool stop() override;

protected:
    using acceptor_type = typename Protocol::acceptor;
    using endpoint_type = typename Protocol::endpoint;
    using socket_type = typename Protocol::socket;
    using native_handle_type = typename socket_type::native_handle_type;

    // Sets an accepted connection up and hands it to the handler, on the io_context serving it.
    // It is a copy of what it needs: the server may be stopped and gone by then
    using connection_server = std::function<void(socket_type)>;

    // Accept the connections on a thread of the server (for servers on the workers)
    void use_acceptor_thread() { own_acceptor_thread_ = true; }

    // The endpoint to listen on, or none if there is none (logged)
    virtual std::optional<endpoint_type> listening_endpoint(boost::asio::io_context& io_context) = 0;
    // Called once listening
    virtual void on_listening() {}
    // What serves the accepted connections, or nothing if they cannot be served (logged)
    virtual connection_server make_connection_server() const = 0;
    // Whether to serve a connection just accepted, e.g. for its remote address
    virtual bool is_connection_allowed(native_handle_type /*descriptor*/) const { return true; }

    // The acceptor, while listening
    const acceptor_type* acceptor() const { return acceptor_.get(); }

private:
    bool create_acceptor() final;
    void accept_connection() final;

    bool listen(boost::asio::io_context& io_context);
    void close_acceptor();
    void stop_acceptor_thread();
    void retry_accept();

    // With io_context providers: accept the next connection on the next connection context
    void accept_next();

    // With a thread of its own: wait for connections, accept the pending ones, and hand each
    // one over to the worker serving it
    void wait_connections();
    void accept_pending();
    void hand_over(native_handle_type descriptor);

    // Thread accepting the connections of a server on the workers. Declared before the
    // acceptor, which uses its io_context
    bool own_acceptor_thread_ = false;
    std::unique_ptr<worker_thread> acceptor_thread_;

    std::unique_ptr<acceptor_type> acceptor_;
    Protocol protocol_ = endpoint_type().protocol();
    std::shared_ptr<const connection_server> serve_;
};

extern template class basic_socket_server<boost::asio::ip::tcp>;
extern template class basic_socket_server<boost::asio::local::stream_protocol>;

} // namespace thinger::asio

#endif // THINGER_ASIO_BASIC_SOCKET_SERVER_HPP

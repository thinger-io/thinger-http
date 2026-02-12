#ifndef THINGER_ASIO_SOCKET_SERVER_HPP
#define THINGER_ASIO_SOCKET_SERVER_HPP

// For backward compatibility, include tcp_socket_server and provide an alias
#include "tcp_socket_server.hpp"

namespace thinger::asio {

// Backward compatibility alias
using socket_server = tcp_socket_server;

} // namespace thinger::asio

#endif // THINGER_ASIO_SOCKET_SERVER_HPP
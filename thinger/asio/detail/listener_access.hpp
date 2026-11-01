#ifndef THINGER_ASIO_DETAIL_LISTENER_ACCESS_HPP
#define THINGER_ASIO_DETAIL_LISTENER_ACCESS_HPP

#include "../listener.hpp"

namespace thinger::asio::detail {

// Internal switches of a listener, for the library's own servers and its tests: not part of
// the public API
struct listener_access {
    // Accept on a thread of the listener's own instead of on `accept_on` (which may then be
    // empty): once the listening socket is readable, it accepts every pending connection and
    // hands each one over to the next `serve_on` executor, where it is registered and set up,
    // waking that thread once. Accepting does not wait behind the connections a thread serves.
    // Before open(); nothing afterwards
    static void use_acceptor_thread(listener& l);
};

} // namespace thinger::asio::detail

#endif // THINGER_ASIO_DETAIL_LISTENER_ACCESS_HPP

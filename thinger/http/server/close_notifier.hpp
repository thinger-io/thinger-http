#ifndef THINGER_HTTP_SERVER_CLOSE_NOTIFIER_HPP
#define THINGER_HTTP_SERVER_CLOSE_NOTIFIER_HPP

#include "../../util/close_notifier.hpp"

namespace thinger::http {

// Moved to thinger/util/close_notifier.hpp (used by asio::websocket too)
template<typename... Args>
using close_notifier = ::thinger::util::close_notifier<Args...>;

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_CLOSE_NOTIFIER_HPP

#include "sse_connection.hpp"

namespace thinger::http {
    std::atomic<unsigned long> sse_connection::connections(0);
}
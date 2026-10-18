#ifndef THINGER_HTTP_SERVER_RESPONSE_OBSERVER_HPP
#define THINGER_HTTP_SERVER_RESPONSE_OBSERVER_HPP

#include "../common/http_response.hpp"
#include "../../util/types.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace thinger::http {

class request;

// How a response ended (see response_info)
enum class response_outcome : uint8_t {
    // Written whole (for a request dispatched in memory, collected whole)
    completed,
    // Its head written, the connection was handed over: a WebSocket upgrade, an event stream
    // (start_sse()) or take_over(). What happens on the connection afterwards is not reported
    // (see websocket_connection::on_close() and sse_connection::on_close())
    taken_over,
    // Not completed: the connection was closed or failed first (the client left, the connection
    // timed out, the server shut down...), the response was aborted (abort_stream(), or a
    // handler failing once it started), or nothing answered the request
    aborted
};

// A response, as http_application::on_response() reports it
struct response_info {
    // Status of the response head (0 if nothing was answered)
    http_response::status status{};
    response_outcome outcome = response_outcome::completed;
    // The response that ended had its body sent in parts as it was produced: start_chunked()
    // or start_stream() (not for HEAD, answered with the head alone, nor for a response that
    // replaced it, as the 502 of a stream aborted in memory)
    bool streamed = false;
    // Bytes of this response written to the connection, head included (and an interim
    // 100 Continue), before any TLS; for a request dispatched in memory, the size of the body
    uint64_t bytes = 0;
    // From the moment the application received the request (its headers read) until the
    // response ended: written, handed over, or aborted
    std::chrono::steady_clock::duration duration{};
};

// See http_application::on_response()
using response_hook = sync_callback<void(const request&, const response_info&)>;

// The end of a request, for the response hook: its response ended (as told by the sink writing
// it) and the application is done with the request, in either order. Created only while a hook
// is set: the server costs nothing without one.
class response_observer {
public:
    // The peer address of the request is kept, for a hook called once its connection is gone
    response_observer(std::shared_ptr<request> req, std::shared_ptr<const response_hook> hook);

    // Reports the response if it was not yet: as aborted, unless it ended
    ~response_observer();

    response_observer(const response_observer&) = delete;
    response_observer& operator=(const response_observer&) = delete;

    // The response, from the sink writing it (one thread at a time): its head, with whether
    // its body follows in parts, the bytes written, and its end (once, later ones are ignored)
    void head(http_response::status status, bool streamed);
    void add_bytes(uint64_t bytes) { info_.bytes += bytes; }
    void ended(response_outcome outcome);

    // From any thread: the response is aborted (it ends as such)
    void abort() { aborted_.store(true, std::memory_order_relaxed); }

    // The application is done with the request
    void handled();

private:
    // One side is done: the last one reports
    void done();
    void report() noexcept;

    std::shared_ptr<request> request_;
    std::shared_ptr<const response_hook> hook_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    response_info info_;
    bool ended_ = false;
    bool handled_ = false;
    bool reported_ = false;
    std::atomic<bool> aborted_{false};
    // The sides not done yet: the response and the application
    std::atomic<uint8_t> pending_{2};
};

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_RESPONSE_OBSERVER_HPP

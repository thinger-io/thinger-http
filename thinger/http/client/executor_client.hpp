#ifndef THINGER_HTTP_CLIENT_EXECUTOR_CLIENT_HPP
#define THINGER_HTTP_CLIENT_EXECUTOR_CLIENT_HPP

#include "http_client_base.hpp"
#include "request_builder.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <memory>
#include <optional>
#include <string>

namespace thinger::http {

/**
 * An HTTP client running on an io_context the caller already runs, e.g. the IO thread of the
 * coroutine using it (see for_current_coroutine()): no threads of its own, no idle timers, and
 * its connections are closed when it is destroyed. Requests are awaited (as with async_client),
 * and run on that io_context: no hop to another thread.
 *
 * Meant for occasional calls from server coroutines (a token refresh, a webhook, an order),
 * far apart: it keeps no idle connection by default, so each request opens its own. For many
 * calls to the same host, use a pooled client (async_client); max_idle_connections_per_host()
 * enables keep-alive here too, closing expired connections when the next request comes.
 *
 * Use it on the threads running the io_context, as its sockets (e.g. from a coroutine on it or
 * on a strand of it), and destroy it there, before the io_context. Cancelling the coroutine
 * awaiting a request (a cancellation slot, or `||` with a timer) cancels the request.
 *
 * Usage:
 *   server.get("/weather", [](http::response& res) -> thinger::awaitable<void> {
 *       auto client = co_await http::executor_client::for_current_coroutine();
 *       auto forecast = co_await client->get("https://api.example.com/forecast");
 *       res.json(forecast.json());
 *   });
 */
class executor_client : public http_client_base {
public:
    explicit executor_client(boost::asio::io_context& io_context);
    // The idle connections (if any) are closed while their io_context exists
    ~executor_client() override;

    executor_client(const executor_client&) = delete;
    executor_client& operator=(const executor_client&) = delete;

    // A client on the io_context of the calling coroutine (see io_context_of()). Throws
    // std::logic_error if the coroutine runs on another kind of executor (e.g. a thread_pool)
    static awaitable<std::unique_ptr<executor_client>> for_current_coroutine();

    /**
     * Create a request builder for fluent API.
     *
     * Usage:
     *   auto res = co_await client.request("https://api.com/data")
     *       .header("Authorization", "Bearer xxx")
     *       .get();
     */
    request_builder<executor_client> request(const std::string& url) {
        return request_builder<executor_client>(this, url);
    }

    // Connect to a WebSocket server (its socket runs on the same io_context)
    awaitable<std::optional<websocket_client>> websocket(const std::string& url,
                                                         const std::string& subprotocol = "") {
        return upgrade_websocket(url, subprotocol);
    }

    // With a request of its own (for the request builder)
    awaitable<std::optional<websocket_client>> websocket(std::shared_ptr<http_request> request,
                                                         const std::string& subprotocol = "") {
        return upgrade_websocket(std::move(request), subprotocol);
    }

protected:
    boost::asio::io_context& get_io_context() override { return io_context_; }

private:
    boost::asio::io_context& io_context_;
};

// The io_context of an executor: an io_context executor, or a strand of one (of any depth);
// null for any other executor (e.g. of a thread_pool)
boost::asio::io_context* io_context_of(const boost::asio::any_io_executor& executor);

} // namespace thinger::http

#endif // THINGER_HTTP_CLIENT_EXECUTOR_CLIENT_HPP

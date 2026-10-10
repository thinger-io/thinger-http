// Reverse proxy example
//
// A front server on port 8096 forwarding requests to a backend, here a second server started
// by the example itself on port 8097 (or the URL given as argument, e.g. http://127.0.0.1:3000):
//   - /api/...: forwarded without the /api prefix, with the credentials of the proxy instead
//     of the client's, and with a header added to every response
//   - app.localhost: every request forwarded as it is, WebSockets and events included
//
// Try:
//   curl -i localhost:8096/api/hello?name=proxy
//   curl -i -H "Authorization: Bearer client-token" localhost:8096/api/whoami
//   curl -N -H "Host: app.localhost" localhost:8096/events
//   curl -i -H "Host: app.localhost" --data-binary @large-file localhost:8096/upload
//   websocat -H "Host: app.localhost" ws://localhost:8096/ws
//   curl -i localhost:8096/api/hello   (with the backend down: 502)

#include <thinger/http_server.hpp>
#include <iostream>
#include <thread>

using namespace thinger;

namespace {

    // A backend to forward to: JSON, an event stream, uploads and a WebSocket echo
    void add_backend_routes(http::server& backend) {
        backend.get("/hello", [](http::request& req, http::response& res) {
            res.json({{"hello", req.query("name", "world")},
                      {"forwarded_for", req.header("X-Forwarded-For")},
                      {"forwarded_host", req.header("X-Forwarded-Host")}});
        });

        backend.get("/whoami", [](http::request& req, http::response& res) {
            res.json({{"authorization", req.header("Authorization")}});
        });

        // An event per second: the proxy relays each one as it arrives
        backend.get("/events", [&backend](http::response& res) {
            res.start_sse([&backend](std::shared_ptr<http::sse_connection> sse) {
                co_spawn(backend.get_io_context(), [sse]() -> thinger::awaitable<void> {
                    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
                    for (int i = 1; i <= 3; ++i) {
                        sse->send_data("event " + std::to_string(i));
                        timer.expires_after(std::chrono::seconds(1));
                        co_await timer.async_wait(thinger::use_nothrow_awaitable);
                    }
                    sse->stop();
                }, thinger::detached);
            });
        });

        // The body arrives as the client sends it, through the proxy
        backend.post("/upload", [](http::request& req, http::response& res) -> thinger::awaitable<void> {
            uint8_t buffer[16384];
            size_t total = 0;
            while (size_t bytes = co_await req.read_some(buffer, sizeof(buffer))) total += bytes;
            res.json({{"received", total}});
        });

        backend.get("/ws", [](http::response& res) {
            res.upgrade_websocket([](std::shared_ptr<http::websocket_connection> ws) {
                ws->on_message([weak = std::weak_ptr<http::websocket_connection>(ws)](const std::string& message, bool) {
                    if (auto ws = weak.lock()) ws->send_text("echo: " + message);
                });
            });
        });
    }

}

int main(int argc, char* argv[]) {
    // Backend: the URL given, or a local server started here
    std::string backend_url = argc > 1 ? argv[1] : "http://127.0.0.1:8097";
    http::server backend;
    std::thread backend_thread;
    if (argc <= 1) {
        add_backend_routes(backend);
        if (!backend.listen("127.0.0.1", 8097)) return 1;
        backend_thread = std::thread([&backend] { backend.wait(); });
    }

    auto target = http::proxy_target::from_url(backend_url);
    if (!target) {
        std::cerr << "Invalid backend URL: " << backend_url << std::endl;
        return 1;
    }

    http::server server;

    // Errors of the proxy (502, 504...) in JSON
    server.set_error_formatter([](const http::http_error& error, http::http_response& response) {
        nlohmann::json body = {{"error", {{"message", error.message.empty()
            ? http::http_response::get_reason_phrase(error.status) : error.message}}}};
        response.set_content(body.dump(), "application/json");
    });

    // One proxy for every route: no idle timeout, as event streams may stay silent
    http::reverse_proxy proxy({.idle_timeout = std::chrono::milliseconds(0)});

    // /api/...: path rewritten, credentials replaced, a header added to the responses
    auto api = [&proxy, target](http::request& req, http::response& res) -> thinger::awaitable<void> {
        http::proxy_options options = proxy.options();
        options.path = "/" + req["path"];
        options.on_request = [](http::request&, http::http_request& outgoing) {
            outgoing.remove_headers("Authorization");
            outgoing.add_header("Authorization", "Bearer backend-token");
        };
        options.on_response = [](http::request&, http::http_response& response) {
            response.add_header("X-Proxy", "thinger-http");
        };
        auto result = co_await proxy.forward(req, res, *target, options);
        if (!result.error.empty()) std::cerr << "proxy error: " << result.error << std::endl;
    };
    server.get("/api/:path(.*)", api);
    server.post("/api/:path(.*)", api);
    server.put("/api/:path(.*)", api);
    server.del("/api/:path(.*)", api);
    server.patch("/api/:path(.*)", api);

    // app.localhost: every request of any method, as it is
    server.host("app.localhost").set_not_found_handler(
        [&proxy, target](http::request& req, http::response& res) -> thinger::awaitable<void> {
            co_await proxy.forward(req, res, *target);
        });

    std::cout << "Reverse proxy on http://localhost:8096 -> " << backend_url << std::endl;
    server.start("0.0.0.0", 8096);

    backend.stop();
    if (backend_thread.joinable()) backend_thread.join();
    return 0;
}

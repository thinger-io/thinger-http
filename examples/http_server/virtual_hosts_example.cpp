// Virtual hosts example
//
// One listener serving several sites, chosen by the Host header:
//   - api.localhost: a JSON API (with its own OpenAPI document)
//   - www.localhost: static files from the directory given as argument (current by default)
//   - <id>.devices.localhost: any device subdomain, e.g. to proxy the traffic to a device
//   - any other host: the routes registered on the server itself (the default host)
// Errors use a JSON format, and the client IP is taken from X-Forwarded-For when the
// request comes from a local reverse proxy.
//
// Try:
//   curl -H "Host: api.localhost" localhost:8095/v1/status
//   curl -H "Host: api.localhost" localhost:8095/openapi.json
//   curl -H "Host: www.localhost" localhost:8095/
//   curl -H "Host: sensor1.devices.localhost" localhost:8095/some/path
//   curl -H "X-Forwarded-For: 203.0.113.7" localhost:8095/
//   curl -i -H "Host: api.localhost" localhost:8095/missing     -> 404 {"error":{"message":"Not Found"}}
//   curl -i -X DELETE -H "Host: api.localhost" localhost:8095/v1/status  -> 405

#include <thinger/http_server.hpp>
#include <iostream>

using namespace thinger;

int main(int argc, char* argv[]) {
    http::server server;

    // Same error format for every host: {"error": {"message": "..."}}
    server.set_error_formatter([](const http::http_error& error, http::http_response& response) {
        nlohmann::json body = {{"error", {{"message", error.message.empty()
            ? http::http_response::get_reason_phrase(error.status) : error.message}}}};
        if (error.details.is_object()) body["error"].update(error.details);
        response.set_content(body.dump(), "application/json");
    });

    // Requests forwarded by a reverse proxy running on this machine
    server.set_trusted_proxies({"127.0.0.1", "::1"});

    // Global middleware: runs for every host, after the route of that host is matched
    server.use([](http::request& req, http::response& res) -> thinger::awaitable<bool> {
        std::cout << req.get_request_ip() << " -> " << req.get_virtual_host()->name() << " "
                  << req.get_http_request()->get_uri() << std::endl;
        co_return true;
    });

    // Default host
    server.get("/", [](http::request& req, http::response& res) {
        res.json({{"host", "default"}, {"client_ip", req.get_request_ip()}, {"peer_ip", req.get_peer_ip()}});
    });

    // API host, with the same registration API as the server
    auto& api = server.host("api.localhost");
    api.group("/v1").tag("Status").get("/status", [](http::response& res) {
        res.json({{"status", "ok"}});
    }).summary("Service status");

    static http::openapi_generator api_doc(api.router());
    api_doc.title("Example API");
    api.get("/openapi.json", [](http::response& res) {
        res.json(api_doc.generate());
    }).hidden();

    // Static site
    server.host("www.localhost").serve_static("/", argc > 1 ? argv[1] : ".");

    // Dynamic subdomains: "*" matches one label, available through get_host_matches()
    server.host("*.devices.localhost").get("/:path(.*)", [](http::request& req, http::response& res) {
        res.json({{"device", req.get_host_matches().at(1)}, {"path", "/" + req["path"]}});
    });

    std::cout << "Listening on http://localhost:8095" << std::endl;
    server.start(8095);
    return 0;
}

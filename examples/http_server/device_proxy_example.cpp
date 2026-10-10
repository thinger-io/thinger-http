// Device web service proxy example
//
// A console serving the web interface of devices under /devices/<id>/services/web/..., to
// show it inside an iframe of the console. The proxy adapts the responses of the device with
// an explicit application policy (device_proxy_policy.hpp), which documents what each rule
// costs. The recommended variant lets only the console origin embed the device pages; the
// device here (a server started by the example on port 8099) sets cookies for its own domain,
// forbids being framed, and redirects to its own paths.
//
// Try:
//   curl -i localhost:8098/devices/sensor1/services/web/
//   curl -i localhost:8098/devices/sensor1/services/web/login     (redirect kept under the proxy)
//   curl -i localhost:8098/devices/unknown/services/web/          (404)

#include <thinger/http_server.hpp>
#include "device_proxy_policy.hpp"
#include <iostream>
#include <map>
#include <thread>

using namespace thinger;

int main() {
    // A device web interface
    http::server device;
    device.get("/", [](http::response& res) {
        res.header("X-Frame-Options", "DENY");
        res.header("Content-Security-Policy", "default-src 'self'; frame-ancestors 'none'");
        res.header("Set-Cookie", "session=abc123; Domain=device.local; Path=/; HttpOnly");
        res.html("<h1>Device</h1><a href=\"/login\">Log in</a>");
    });
    device.get("/login", [](http::response& res) {
        res.redirect("/");    // an absolute path of the device
    });
    if (!device.listen("127.0.0.1", 8099)) return 1;
    std::thread device_thread([&device] { device.wait(); });

    // Where each device is reached (e.g. through a tunnel or a Unix socket)
    std::map<std::string, http::proxy_target> devices = {
        {"sensor1", *http::proxy_target::from_url("http://127.0.0.1:8099")}
    };

    http::server console;
    http::reverse_proxy proxy;

    auto web_service = [&](http::request& req, http::response& res) -> thinger::awaitable<void> {
        auto it = devices.find(req["device"]);
        if (it == devices.end()) {
            res.error(http::http_response::status::not_found, "Unknown device");
            co_return;
        }
        const auto& target = it->second;

        device_proxy::policy policy;
        policy.base_path = "/devices/" + req["device"] + "/services/web";
        // only the console may embed the device pages (instead of removing their protections)
        policy.console_origin = "https://console.example.com";
        // cross-site cookies only if the console is on another site than this proxy
        policy.cross_site_cookies = false;

        http::proxy_options options;
        options.path = "/" + req["path"];
        options.on_response = [policy, target](http::request& req, http::http_response& response) {
            // hosts the device may use for itself: the Host it got, and the public one
            device_proxy::apply(policy, {target.authority(), req.header("Host")}, response);
        };
        co_await proxy.forward(req, res, target, options);
    };
    const std::string route = "/devices/:device/services/web/:path(.*)";
    console.get(route, web_service);
    console.post(route, web_service);
    console.put(route, web_service);
    console.del(route, web_service);
    console.patch(route, web_service);

    std::cout << "Console on http://localhost:8098/devices/sensor1/services/web/" << std::endl;
    console.start("0.0.0.0", 8098);

    device.stop();
    device_thread.join();
    return 0;
}

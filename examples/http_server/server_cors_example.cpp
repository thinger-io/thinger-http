// CORS example: a list of origins allowed, with credentials
//
// The server listens on two ports, so that its own page is a cross-origin client of its API:
// open http://localhost:8092/ (the origin allowed) and it calls http://localhost:8091.
//
// Without a browser:
//   Allowed, so the response says it:
//     curl -i -H "Origin: http://localhost:8092" localhost:8091/api/data
//   Not allowed: the handler runs the same, with no Access-Control-* (only Vary: Origin):
//     curl -i -H "Origin: https://evil.example" localhost:8091/api/data
//   A preflight: 204, without the basic auth of its route, which it could not pass:
//     curl -i -X OPTIONS -H "Origin: http://localhost:8092" \
//          -H "Access-Control-Request-Method: GET" localhost:8091/api/protected
//   The errors have the headers too, for the browser to read them:
//     curl -i -H "Origin: http://localhost:8092" localhost:8091/api/protected
//     curl -i -H "Origin: http://localhost:8092" localhost:8091/api/error
//     curl -i -H "Origin: http://localhost:8092" localhost:8091/nothing-here

#include <thinger/http_server.hpp>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

using namespace thinger;

namespace {

    // Runs each kind of request against `api`: cookies and Authorization need the credentials
    // and the preflights that the server allows
    std::string test_page(uint16_t api_port) {
        return R"html(<!DOCTYPE html>
<html>
<head>
    <title>CORS Test Page</title>
    <script>
        const api = 'http://localhost:)html" + std::to_string(api_port) + R"html(';

        async function check(name, path, options) {
            const results = document.getElementById('results');
            try {
                const response = await fetch(api + path, options);
                const total = response.headers.get('X-Total-Count');
                const body = await response.text();
                results.innerHTML += `<div>${name}: ${response.status}` +
                    `${total ? ', X-Total-Count ' + total : ''}: ${body}</div>`;
            } catch (error) {
                results.innerHTML += `<div style="color: red;">${name}: ${error.message}</div>`;
            }
        }

        async function testCORS() {
            document.getElementById('results').innerHTML = '';
            await check('Exposed header', '/api/data');
            await check('Preflight, PUT with JSON', '/api/data', {
                method: 'PUT',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({test: 'data'})
            });
            await check('Set a cookie', '/api/login', {method: 'POST', credentials: 'include'});
            await check('Send the cookie', '/api/session', {credentials: 'include'});
            await check('Preflight, Authorization', '/api/protected', {
                credentials: 'include',
                headers: {'Authorization': 'Basic ' + btoa('user:pass')}
            });
            await check('An error the browser can read', '/api/error');
            await check('Not found', '/nothing-here');
        }
    </script>
</head>
<body>
    <h1>CORS Test Page</h1>
    <p>Open it from the port after the one of the API, the origin the server allows.</p>
    <button onclick="testCORS()">Test CORS Requests</button>
    <div id="results"></div>
</body>
</html>
)html";
    }

}

int main(int argc, char* argv[]) {
    // Get port from command line or use default
    const uint16_t port = argc > 1 ? static_cast<uint16_t>(std::stoi(argv[1])) : 8091;
    const uint16_t page_port = static_cast<uint16_t>(port + 1);

    http::server server;

    // With credentials the origins must be listed, or decided by an origin_filter: browsers
    // reject them with "*", so enable_cors() returns invalid_argument for that. The defaults,
    // enable_cors() alone, are those of cors() in Express: any origin, without credentials.
    if (auto ec = server.enable_cors({
            .origins = {"http://localhost:" + std::to_string(page_port), "https://console.example.com"},
            .allowed_headers = std::vector<std::string>{"Content-Type", "Authorization"},
            .exposed_headers = {"X-Total-Count"},
            .max_age = std::chrono::minutes(10),
            .credentials = true,
        })) {
        std::cerr << "Invalid CORS options: " << ec.message() << std::endl;
        return 1;
    }

    server.get("/", [page = test_page(port)](http::response& res) {
        res.html(page);
    });

    // A header the browser only lets the page read because it is in exposed_headers
    server.get("/api/data", [](http::response& res) {
        res.header("X-Total-Count", "5");
        res.json({
            {"message", "CORS is working!"},
            {"timestamp", std::time(nullptr)},
            {"data", {1, 2, 3, 4, 5}}
        });
    });

    // A browser asks with a preflight first: not a simple method, nor a simple Content-Type
    server.put("/api/data", [](nlohmann::json& body, http::response& res) {
        res.json({
            {"received", body},
            {"message", "PUT after its preflight"}
        });
    });

    // Cookies cross origins only with credentials: 'include' in the page, and
    // Access-Control-Allow-Credentials from here
    server.post("/api/login", [](http::response& res) {
        res.header("Set-Cookie", "session=demo; Path=/; HttpOnly");
        res.json({{"message", "Logged in"}});
    });

    server.get("/api/session", [](http::request& req, http::response& res) {
        res.json({{"cookie", req.header("Cookie")}});
    });

    // A preflight does not reach this middleware (it could not pass it: browsers send preflights
    // without credentials), and its 401 has the CORS headers
    server.set_basic_auth("/api/protected", "Protected API", "user", "pass");

    server.get("/api/protected", [](http::request& req, http::response& res) {
        res.json({
            {"message", "Authenticated with CORS!"},
            {"user", req.get_auth_user()}
        });
    });

    // The errors of handlers and of the library have the headers too
    server.get("/api/error", [](http::response& res) {
        res.error(http::http_response::status::internal_server_error, "A test error with CORS headers");
    });

    server.set_not_found_handler([](http::request& req, http::response& res) {
        res.json({
            {"error", "Not Found"},
            {"path", req.get_http_request()->get_uri()}
        }, http::http_response::status::not_found);
    });

    if (auto ec = server.listen("0.0.0.0", page_port)) {
        std::cerr << "Failed to listen on port " << page_port << ": " << ec.message() << std::endl;
        return 1;
    }

    if (!server.start("0.0.0.0", port, [port, page_port]() {
        std::cout << "API on http://localhost:" << port << std::endl;
        std::cout << "Open the test page on http://localhost:" << page_port << "/" << std::endl;
        std::cout << "Press Ctrl+C to stop" << std::endl;
    })) {
        std::cerr << "Failed to start server on port " << port << std::endl;
        return 1;
    }

    return 0;
}

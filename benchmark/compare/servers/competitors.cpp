// The routes of servers/thinger_server.cpp on cpp-httplib (argv[1] == "httplib") or Crow ("crow").
// Usage: competitors <httplib|crow> <port> <threads>
#include <httplib.h>
#include <crow.h>
#include <nlohmann/json.hpp>
#include <string>
#include <cstdlib>

int main(int argc, char** argv) {
    std::string which = argc > 1 ? argv[1] : "httplib";
    int port = argc > 2 ? std::atoi(argv[2]) : 9080;
    // cpp-httplib serves one connection per thread: give it as many threads as connections
    int threads = argc > 3 ? std::atoi(argv[3]) : 8;
    if (which == "httplib") {
        httplib::Server svr;
        svr.new_task_queue = [threads] { return new httplib::ThreadPool(threads); };
        svr.Get("/hello", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("Hello World!", "text/plain");
        });
        svr.Get(R"(/v1/users/([^/]+)/devices/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
            nlohmann::json out = {{"user", req.matches[1]}, {"device", req.matches[2]}, {"connected", true}};
            res.set_content(out.dump(), "application/json");
        });
        svr.Post("/v1/echo", [](const httplib::Request& req, httplib::Response& res) {
            auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded()) { res.status = 400; return; }
            res.set_content(body.dump(), "application/json");
        });
        svr.listen("0.0.0.0", port);
    } else {
        crow::SimpleApp app;
        app.loglevel(crow::LogLevel::Warning);
        CROW_ROUTE(app, "/hello")([] { return "Hello World!"; });
        CROW_ROUTE(app, "/v1/users/<string>/devices/<string>")([](const std::string& user, const std::string& device) {
            nlohmann::json out = {{"user", user}, {"device", device}, {"connected", true}};
            crow::response res(out.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });
        CROW_ROUTE(app, "/v1/echo").methods(crow::HTTPMethod::Post)([](const crow::request& req) {
            auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded()) return crow::response(400);
            crow::response res(body.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });
        app.port(port).concurrency(threads).run();
    }
    return 0;
}

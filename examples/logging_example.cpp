#include <thinger/http_server.hpp>
#include <thinger/util/logger.hpp>
#include <iostream>

using namespace thinger::http;

int main() {
    // Option 1: Enable default console logging
    thinger::logging::enable();
    
    // Option 2: Use custom spdlog logger (comment out Option 1 first)
    // auto custom_logger = spdlog::basic_logger_mt("custom", "logs/server.log");
    // thinger::logging::set_logger(custom_logger);
    
    // Option 3: Change log level
    // thinger::logging::set_log_level(spdlog::level::debug);
    
    server srv;
    
    srv.get("/", [](request& req, response& res) {
        LOG_INFO("Received request to /");
        res.send("Hello World!");
    });
    
    srv.get("/error", [](request& req, response& res) {
        LOG_ERROR("Simulated error endpoint accessed");
        res.send("Internal Server Error", "text/plain");
    });
    
    LOG_INFO("Starting server on port 8080");
    srv.start("0.0.0.0", 8080);
    
    return 0;
}
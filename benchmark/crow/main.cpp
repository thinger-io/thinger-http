#include <crow.h>
#include <iostream>

int main() {
    crow::SimpleApp app;
    
    // Disable Crow logging
    app.loglevel(crow::LogLevel::Warning);
    
    CROW_ROUTE(app, "/")([]() {
        return "Hello World!";
    });
    
    std::cout << "Crow server running at http://localhost:9082" << std::endl;
    app.port(9082).multithreaded().run();
    
    return 0;
}
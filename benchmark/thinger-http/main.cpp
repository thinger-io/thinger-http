#include <thinger/http.hpp>
#include <iostream>

using namespace thinger;

int main() {
    http::server srv;
    
    srv.get("/", [](http::request& req, http::response& res) {
        res.send("Hello World!");
    });
    
    std::cout << "Server running at http://localhost:9080" << std::endl;
    srv.start("0.0.0.0", 9080);
    
    return 0;
}
#include <httplib.h>
#include <iostream>

int main() {
    httplib::Server svr;
    
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("Hello World!", "text/plain");
    });
    
    std::cout << "cpp-httplib server running at http://localhost:9081" << std::endl;
    svr.listen("0.0.0.0", 9081);
    
    return 0;
}